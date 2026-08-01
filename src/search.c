/* ============================ Find / Replace ===============================
 */

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "edit.h"
#include "keyevent.h"
#include "localvars.h"
#include "regex.h"
#include "syntax.h"

#define KILO_QUERY_LEN 256

/* Key sets the search prompts ask about; see key_in_list(). */
static const struct key_event erase_keys[]
    = { { KEY_BASE_DELETE, 0 }, { 'h', KEY_MOD_CTRL }, { KEY_BASE_DEL, 0 } };
static const struct key_event replace_stop_keys[] = { { KEY_BASE_ESC, 0 },
	{ 'g', KEY_MOD_CTRL }, { 'q', 0 } };
static const struct key_event replace_do_keys[]
    = { { 'y', 0 }, { KEY_BASE_RET, 0 }, { ' ', 0 } };
static const struct key_event isearch_end_keys[]
    = { { KEY_BASE_ESC, 0 }, { KEY_BASE_RET, 0 }, { 'g', KEY_MOD_CTRL } };
static const struct key_event isearch_forward_keys[] = { { KEY_BASE_RIGHT, 0 },
	{ KEY_BASE_DOWN, 0 }, { 's', KEY_MOD_CTRL } };
static const struct key_event isearch_backward_keys[] = { { KEY_BASE_LEFT, 0 },
	{ KEY_BASE_UP, 0 }, { 'r', KEY_MOD_CTRL } };
static const struct key_event isearch_history_keys[]
    = { { 'p', KEY_MOD_META }, { 'n', KEY_MOD_META } };

/* Emacs keeps literal and regexp searches in separate rings (search-ring
 * and regexp-search-ring) and runs every query-replace prompt — both
 * strings, both the literal and the regexp command — off one shared
 * query-replace-history.  These do the same. */
static struct minibuf_history search_history;
static struct minibuf_history regexp_search_history;
static struct minibuf_history query_replace_history;

/* One search's highlight snapshot: the bytes lifted out of row->hl, how
 * many of them there were, which row they came from, and the buffer's
 * content generation when they were taken.  All four are needed because
 * the snapshot outlives keys that can edit the buffer -- isearch hands
 * C-d off to editor_del_forward_char(), which at end of line joins the
 * next row into this one and grows its rsize under a snapshot sized
 * before the join. */
struct hl_snapshot {
	unsigned char *hl;
	int len;
	int line;
	uint64_t generation;
};

/* Put a snapshot back and drop it.  A row that changed since the save is
 * left alone: the edit re-ran the highlighter over it already, so the
 * snapshot describes text that is no longer there. */
static void hl_snapshot_restore(struct hl_snapshot *snap)
{
	if (!snap->hl) {
		return;
	}
	if (snap->line < bcur()->numrows
	    && snap->generation == bcur()->content_generation) {
		erow *row = &bcur()->row[snap->line];
		/* TODO(markers): the saved length is what bounds this copy
		 * until decorations live on the edit transaction; row->rsize
		 * is the restore-time size and describes a different row. */
		int n = snap->len < row->rsize ? snap->len : row->rsize;

		KG_ASSERT_RENDER_OFF(row, n);
		memcpy(row->hl, snap->hl, (size_t)n);
	}
	free(snap->hl);
	snap->hl = NULL;
}

/* Snapshot row `filerow`'s highlight, then paint the match at chars
 * offsets [col, col + len) as HL_MATCH so the search question is asked
 * about a visibly marked match.  row->hl is indexed in render bytes, so
 * the span is converted here and every caller stays in chars space.
 * Returns 0, or -1 when the snapshot could not be allocated; a row
 * carrying no highlight array needs neither and reports success. */
static int hl_snapshot_mark(
    int filerow, int col, int len, struct hl_snapshot *snap)
{
	erow *row = &bcur()->row[filerow];
	int rcol, rlen;

	KG_ASSERT_CHARS_OFF(row, col);
	KG_ASSERT_CHARS_OFF(row, col + len);
	rcol = chars_to_render_col(row, col);
	rlen = chars_to_render_col(row, col + len) - rcol;
	KG_ASSERT_RENDER_OFF(row, rcol + rlen);
	if (!row->hl) {
		return 0;
	}
	snap->hl = malloc((size_t)row->rsize);
	if (!snap->hl) {
		return -1;
	}
	snap->len = row->rsize;
	snap->line = filerow;
	snap->generation = bcur()->content_generation;
	memcpy(snap->hl, row->hl, (size_t)row->rsize);
	if (rcol + rlen <= row->rsize) {
		memset(row->hl + rcol, HL_MATCH, (size_t)rlen);
	}
	return 0;
}

static int query_has_upper(const char *q, int qlen)
{
	int i;
	for (i = 0; i < qlen; i++) {
		if (isupper((unsigned char)q[i])) {
			return 1;
		}
	}
	return 0;
}

static char *case_strstr(const char *hay, const char *needle, int fold)
{
	if (!fold) {
		return strstr(hay, needle);
	}
	if (!*needle) {
		return (char *)hay;
	}
	for (; *hay; hay++) {
		const char *h = hay;
		const char *n = needle;
		while (*h && *n
		    && tolower((unsigned char)*h)
			== tolower((unsigned char)*n)) {
			h++;
			n++;
		}
		if (!*n) {
			return (char *)hay;
		}
	}
	return NULL;
}

static char *isearch_find_last_before(
    char *s, char *query, int limit, int qlen, int fold)
{
	char *best = NULL;
	char *match = s;

	while ((match = case_strstr(match, query, fold)) != NULL) {
		if (match - s + qlen > limit) {
			break;
		}
		best = match;
		match++;
	}
	return best;
}

/* Look for the query from (start_row, start_col), wrapping once around the
 * buffer.  Returns 1 with the match filled in, 0 when there is none, and
 * -1 when a match attempt ran out of the engine's budget: the subject may
 * well hold a match, so that is not the same answer as "no match". */
static int isearch_find_match(int start_row, int start_col, int direction,
    enum search_kind kind, const struct kg_regex *rx, char *query, int qlen,
    int fold, int *match_row, int *match_col, int *match_len)
{
	int current, i;

	if (bcur()->numrows == 0 || qlen == 0) {
		return 0;
	}

	if (start_row < 0) {
		start_row = 0;
	} else if (start_row >= bcur()->numrows) {
		start_row = bcur()->numrows - 1;
	}

	current = start_row;
	for (i = 0; i < bcur()->numrows; i++) {
		erow *row = &bcur()->row[current];
		int col
		    = (i == 0) ? start_col : (direction > 0 ? 0 : row->size);

		if (col < 0) {
			col = 0;
		} else if (col > row->size) {
			col = row->size;
		}

		if (kind == SEARCH_REGEXP) {
			struct kg_match match_res;
			int status;
			if (direction > 0) {
				status = kg_regex_match_forward(
				    rx, row->chars, col, &match_res);
			} else {
				status = kg_regex_match_backward(
				    rx, row->chars, col, &match_res);
			}
			if (status == KG_REGEX_OK) {
				*match_row = current;
				*match_col = match_res.spans[0].start;
				*match_len = match_res.spans[0].end
				    - match_res.spans[0].start;
				return 1;
			}
			if (status == KG_REGEX_TOO_COMPLEX) {
				return -1;
			}
		} else {
			char *match;
			if (direction > 0) {
				match = case_strstr(
				    row->chars + col, query, fold);
			} else {
				match = isearch_find_last_before(
				    row->chars, query, col, qlen, fold);
			}

			if (match) {
				*match_row = current;
				*match_col = match - row->chars;
				*match_len = qlen;
				return 1;
			}
		}

		current += direction;
		if (current < 0) {
			current = bcur()->numrows - 1;
		} else if (current == bcur()->numrows) {
			current = 0;
		}
	}
	return 0;
}

/* What one answer to the "(y/n/!/q)" question means.  Emacs takes y,
 * Space and Enter as yes, "!" as yes to this match and every one after
 * it, q, Esc and C-g as stop, and anything else -- n included -- as skip
 * this one. */
enum replace_answer {
	REPLACE_SKIP,
	REPLACE_DO,
	REPLACE_ALL,
	REPLACE_STOP,
};

static enum replace_answer query_replace_answer(struct key_event c)
{
	if (KEY_IN_LIST(replace_stop_keys, c)) {
		return REPLACE_STOP;
	}
	if (KEY_IS(c, '!', 0)) {
		return REPLACE_ALL;
	}
	if (KEY_IN_LIST(replace_do_keys, c)) {
		return REPLACE_DO;
	}
	return REPLACE_SKIP;
}

/* Replace the separator ending row `rowidx` with `replace`.  One byte
 * out and `rlen` bytes in is one replacement, so it is one row rebuild
 * and one C-_ -- where this used to push a kill record and a yank record
 * for the two halves it did by hand, so undoing one accepted
 * replacement took two. */
static void query_replace_newline_at(int rowidx, char *replace, int rlen)
{
	size_t pos = buffer_row_col_to_position(
	    bcur(), rowidx, bcur()->row[rowidx].size);
	struct kg_edit e
	    = kg_edit_user(bcur(), pos, pos + 1, replace, (size_t)rlen);

	kg_buffer_replace(&e, NULL);
}

/* Emacs restricts query-replace to the region when one is active; otherwise
 * it runs from point to the end of the buffer.  The bounds are returned as
 * an inclusive row range with a column on each end. */
static void query_replace_bounds(
    int *start_row, int *start_col, int *end_row, int *end_col)
{
	if (bcur()->mark_set && bcur()->mark_highlight
	    && editor_region_bounds(start_row, start_col, end_row, end_col)) {
		return;
	}
	*start_row = wcur()->rowoff + wcur()->cy;
	*start_col = wcur()->coloff + wcur()->cx;
	*end_row = bcur()->numrows > 0 ? bcur()->numrows - 1 : 0;
	*end_col = bcur()->numrows > 0 ? bcur()->row[*end_row].size : 0;
}

static void editor_query_replace_newline(
    int fd, char *replace, int rlen, int start_row, int end_row)
{
	int filerow = start_row;
	int count = 0, replace_all = 0;

	/* The newline ending row R is inside the region only while R is above
	 * the last region row. */
	while (filerow < end_row && filerow < bcur()->numrows - 1) {
		enum replace_answer answer = REPLACE_DO;

		editor_cursor_goto(filerow, bcur()->row[filerow].size);
		if (!replace_all) {
			editor_set_status_message(
			    "Replace \"^J\" with \"%s\"? (y/n/!/q)", replace);
			editor_refresh_screen();
			answer = query_replace_answer(editor_read_key(fd));
		}

		if (answer == REPLACE_STOP) {
			break;
		}
		if (answer == REPLACE_ALL) {
			replace_all = 1;
		}

		if (answer != REPLACE_SKIP) {
			query_replace_newline_at(filerow, replace, rlen);
			/* The join pulled the next row up, so the last region
			 * row moved with it. */
			end_row--;
			count++;
		} else {
			filerow++;
		}
	}

	editor_set_status_message(
	    count ? "Replaced %d occurrence%s." : "No replacements made.",
	    count, count == 1 ? "" : "s");
}

/* The plain motion `c` hands off to, as the legacy direction constant
 * editor_move_cursor() takes, or 0 when `c` is not one of the six. */
static int isearch_handoff_direction(struct key_event c)
{
	static const struct {
		struct key_event key;
		int direction;
	} moves[] = {
		{ { 'a', KEY_MOD_CTRL }, HOME_KEY },
		{ { KEY_BASE_HOME, 0 }, HOME_KEY },
		{ { 'b', KEY_MOD_CTRL }, ARROW_LEFT },
		{ { KEY_BASE_LEFT, 0 }, ARROW_LEFT },
		{ { 'e', KEY_MOD_CTRL }, END_KEY },
		{ { KEY_BASE_END, 0 }, END_KEY },
		{ { 'f', KEY_MOD_CTRL }, ARROW_RIGHT },
		{ { KEY_BASE_RIGHT, 0 }, ARROW_RIGHT },
		{ { 'n', KEY_MOD_CTRL }, ARROW_DOWN },
		{ { KEY_BASE_DOWN, 0 }, ARROW_DOWN },
		{ { 'p', KEY_MOD_CTRL }, ARROW_UP },
		{ { KEY_BASE_UP, 0 }, ARROW_UP },
	};
	size_t i;

	for (i = 0; i < sizeof(moves) / sizeof(*moves); i++) {
		if (key_event_equal(moves[i].key, c)) {
			return moves[i].direction;
		}
	}
	return 0;
}

typedef void (*isearch_move_fn)(void);

/* The remaining handoff keys, each a plain call with no result to act
 * on: unlike isearch_handoff_direction()'s six, editor_move_cursor()
 * does not cover these, so each gets the function it hands off to
 * directly rather than a legacy direction constant. */
static const struct {
	struct key_event key;
	isearch_move_fn move;
} isearch_moves[] = {
	{ { KEY_BASE_HOME, KEY_MOD_CTRL }, editor_move_to_beginning },
	{ { '<', KEY_MOD_META }, editor_move_to_beginning },
	{ { KEY_BASE_END, KEY_MOD_CTRL }, editor_move_to_end },
	{ { '>', KEY_MOD_META }, editor_move_to_end },
	{ { 'b', KEY_MOD_META }, editor_move_word_backward },
	{ { KEY_BASE_LEFT, KEY_MOD_CTRL }, editor_move_word_backward },
	{ { 'f', KEY_MOD_META }, editor_move_word_forward },
	{ { KEY_BASE_RIGHT, KEY_MOD_CTRL }, editor_move_word_forward },
	{ { 'm', KEY_MOD_META }, editor_move_to_indentation },
	{ { 'a', KEY_MOD_META }, editor_move_sentence_backward },
	{ { 'e', KEY_MOD_META }, editor_move_sentence_forward },
	{ { '{', KEY_MOD_META }, editor_move_paragraph_backward },
	{ { KEY_BASE_UP, KEY_MOD_CTRL }, editor_move_paragraph_backward },
	{ { '}', KEY_MOD_META }, editor_move_paragraph_forward },
	{ { KEY_BASE_DOWN, KEY_MOD_CTRL }, editor_move_paragraph_forward },
};

static isearch_move_fn isearch_lookup_move(struct key_event c)
{
	size_t i;

	for (i = 0; i < sizeof(isearch_moves) / sizeof(*isearch_moves); i++) {
		if (key_event_equal(isearch_moves[i].key, c)) {
			return isearch_moves[i].move;
		}
	}
	return NULL;
}

static int isearch_handoff_key(int fd, struct key_event c)
{
	int direction;
	isearch_move_fn move;

	(void)fd;
	if (KEY_IS(c, ' ', KEY_MOD_CTRL)) {
		editor_set_mark();
		return 1;
	}
	direction = isearch_handoff_direction(c);
	if (direction) {
		editor_set_status_message("");
		editor_move_cursor(direction);
		return 1;
	}
	if (KEY_IS(c, 'd', KEY_MOD_CTRL)) {
		if (bcur()->readonly) {
			editor_set_status_message("Buffer is read-only");
		} else {
			editor_set_status_message("");
			editor_del_forward_char();
		}
		return 1;
	}
	move = isearch_lookup_move(c);
	if (move) {
		editor_set_status_message("");
		move();
		return 1;
	}
	return 0;
}

/* Install `entry` as the in-progress isearch query.  Ring entries are
 * literal text, so the caller re-searches with it exactly as if it had
 * been typed — smart case included. */
static void isearch_set_query(char *query, int *qlen, const char *entry)
{
	int len = (int)strnlen(entry, KILO_QUERY_LEN);

	memcpy(query, entry, (size_t)len);
	query[len] = '\0';
	*qlen = len;
}

/* Emacs' C-s with an empty search string reuses the last search, so a bare
 * C-s C-s repeats the previous one.  No-op once anything has been typed. */
static void isearch_recall_last(
    struct minibuf_history *hist, char *query, int *qlen, int *hist_index)
{
	const char *entry = *qlen == 0 ? minibuf_history_get(hist, 0) : NULL;

	if (entry) {
		isearch_set_query(query, qlen, entry);
		*hist_index = 0;
	}
}

static void do_isearch(int fd, int direction, enum search_kind kind)
{
	char query[KILO_QUERY_LEN + 1] = { 0 };
	char draft[KILO_QUERY_LEN + 1] = { 0 };
	struct minibuf_history *hist
	    = kind == SEARCH_REGEXP ? &regexp_search_history : &search_history;
	int hist_index = -1; /* -1 == the query being typed, not a recall */
	int saved_cx = wcur()->cx, saved_cy = wcur()->cy;
	int saved_coloff = wcur()->coloff, saved_rowoff = wcur()->rowoff;
	int start_row = wcur()->rowoff + wcur()->cy;
	int start_col = 0;
	int last_match_row = -1;
	int last_match_col = -1;
	struct hl_snapshot snap = { 0 };
	int find_next = 0; /* if 1 search next, if -1 search prev. */
	int too_complex = 0; /* last attempt ran out of engine budget */
	int qlen = 0;

	/* Everything in this function is a chars byte offset: the search
	 * reads row->chars, so a TAB is one character and never the eight
	 * columns it is drawn as.  Only the highlight below is converted to
	 * render space, because row->hl is indexed that way. */
	start_col = wcur()->coloff + wcur()->cx;

	while (1) {
		struct key_event c;
		int fold = !query_has_upper(query, qlen);
		int rx_valid = 1;
		if (kind == SEARCH_REGEXP && qlen > 0) {
			struct kg_regex rx;
			int rx_flags = fold ? KG_REGEX_ICASE : 0;
			if (kg_regex_compile(&rx, query, rx_flags)
			    != KG_REGEX_OK) {
				rx_valid = 0;
			}
		}

		/* One prompt for both kinds.  "[too complex]" says the
		 * pattern is fine and matching it against this buffer is
		 * what the engine gave up on; a literal search reaches
		 * neither that state nor an invalid pattern. */
		editor_set_status_message("%sI-search %s: %s",
		    kind == SEARCH_REGEXP ? "Regexp " : "",
		    !rx_valid	      ? "[bad regexp]"
			: too_complex ? "[too complex]"
			: fold	      ? "[fold]"
				      : "[case]",
		    query);
		editor_refresh_screen();

		c = editor_read_key(fd);
		if (KEY_IN_LIST(erase_keys, c)) {
			/* Drop a whole character, so backspacing over a
			 * multi-byte glyph never leaves half of it in the
			 * search string. */
			if (qlen != 0) {
				qlen = utf8_glyph_start_before(
				    query, qlen, qlen);
				query[qlen] = '\0';
			}
			last_match_row = -1;
			last_match_col = -1;
			find_next = direction;
		} else if (KEY_IN_LIST(isearch_end_keys, c)) {
			if (KEY_IS(c, KEY_BASE_ESC, 0)) {
				wcur()->cx = saved_cx;
				wcur()->cy = saved_cy;
				wcur()->coloff = saved_coloff;
				wcur()->rowoff = saved_rowoff;
			}
			/* Emacs records the query when the search is
			 * accepted, never when it is abandoned. */
			if (KEY_IS(c, KEY_BASE_RET, 0)) {
				minibuf_history_add(hist, query);
			}
			hl_snapshot_restore(&snap);
			editor_set_status_message("");
			return;
		} else if (KEY_IN_LIST(isearch_forward_keys, c)) {
			isearch_recall_last(hist, query, &qlen, &hist_index);
			find_next = 1;
		} else if (KEY_IN_LIST(isearch_backward_keys, c)) {
			isearch_recall_last(hist, query, &qlen, &hist_index);
			find_next = -1;
		} else if (KEY_IN_LIST(isearch_history_keys, c)) {
			int dir = KEY_IS(c, 'p', KEY_MOD_META) ? 1 : -1;
			const char *entry;

			if (dir > 0 && hist_index < 0) {
				memcpy(draft, query, (size_t)qlen + 1);
			}
			entry = minibuf_history_walk(
			    hist, dir, &hist_index, draft);
			if (entry) {
				isearch_set_query(query, &qlen, entry);
				last_match_row = -1;
				last_match_col = -1;
				find_next = direction;
			}
		} else if (c.mods == 0 && ascii_is_print(c.base)) {
			if (qlen < KILO_QUERY_LEN) {
				query[qlen++] = (char)c.base;
				query[qlen] = '\0';
				last_match_row = -1;
				last_match_col = -1;
				find_next = direction;
			}
		} else if (c.mods == 0 && c.base >= 0x80 && c.base <= 0xFF) {
			/* Lead byte of a multi-byte character: read the rest
			 * of the sequence so it joins the query whole. */
			char seq[4];
			int n = editor_read_utf8_seq(fd, (int)c.base, seq);

			if (n > 0 && qlen + n <= KILO_QUERY_LEN) {
				memcpy(query + qlen, seq, (size_t)n);
				qlen += n;
				query[qlen] = '\0';
				last_match_row = -1;
				last_match_col = -1;
				find_next = direction;
			}
		} else if (isearch_handoff_key(fd, c)) {
			/* A handoff ends the search on its match, which in
			 * Emacs commits the query just like Enter does.  The
			 * key may have edited the buffer (C-d), so the
			 * snapshot is only put back if it still fits the row
			 * it came from. */
			minibuf_history_add(hist, query);
			hl_snapshot_restore(&snap);
			return;
		}

		/* Search occurrence. */
		if (find_next) {
			struct kg_regex rx = { 0 };
			if (kind == SEARCH_REGEXP) {
				if (qlen == 0) {
					find_next = 0;
					continue;
				}
				fold = !query_has_upper(query, qlen);
				if (kg_regex_compile(
					&rx, query, fold ? KG_REGEX_ICASE : 0)
				    != KG_REGEX_OK) {
					editor_set_status_message(
					    "Regexp I-search [bad regexp]: %s",
					    query);
					find_next = 0;
					continue;
				}
			}
			{
				int match = 0;
				int current = start_row;
				int col = start_col;
				int match_row = -1;
				int match_col = -1;
				int match_len = 0;
				int point_col;
				int search_dir = find_next;

				fold = !query_has_upper(query, qlen);

				if (last_match_row != -1) {
					current = last_match_row;
					col = last_match_col
					    + (search_dir > 0 ? 1 : 0);
				}
				match = isearch_find_match(current, col,
				    search_dir, kind, &rx, query, qlen, fold,
				    &match_row, &match_col, &match_len);
				find_next = 0;
				too_complex = match < 0;

				/* Highlight */
				hl_snapshot_restore(&snap);

				if (match > 0) {
					last_match_row = match_row;
					last_match_col = match_col;
					if (hl_snapshot_mark(match_row,
						match_col, match_len, &snap)
					    != 0) {
						editor_set_status_message(
						    "Out of memory");
						running = 0;
						return;
					}
					point_col = match_col
					    + (search_dir > 0 ? match_len : 0);
					editor_reveal_position_centered(
					    match_row, point_col);
				}
			}
		}
	}
}

void editor_find(int fd, int direction)
{
	do_isearch(fd, direction, SEARCH_LITERAL);
}

void editor_find_regexp(int fd, int direction)
{
	do_isearch(fd, direction, SEARCH_REGEXP);
}

/* Where a refused match resumes: past the whole glyph at `col`, so a
 * byte offset never lands inside a multi-byte character.  This is the
 * only advance in the literal query-replace loop that is not already a
 * whole match, and it used to be a plain col + 1. */
static int skip_one_glyph(erow *row, int col)
{
	int span = utf8_glyph_span_at(row->chars, row->size, col);

	return col + (span > 0 ? span : 1);
}

/* How a replacement reshapes the row it lands in.  `*nl` rows appear
 * below that row, and `*tail` is how many bytes of the replacement follow
 * its last separator -- which is the column the bytes that came after the
 * match end up at, on the last of the new rows.  With no separator the
 * row count does not move and `*tail` is the whole replacement. */
static void replacement_shape(const char *replace, int rlen, int *nl, int *tail)
{
	int i;

	*nl = 0;
	*tail = rlen;
	for (i = 0; i < rlen; i++) {
		if (replace[i] == '\n') {
			(*nl)++;
			*tail = rlen - i - 1;
		}
	}
}

void editor_query_replace(int fd)
{
	char search[KILO_QUERY_LEN + 1] = { 0 };
	char replace[KILO_QUERY_LEN + 1] = { 0 };
	struct hl_snapshot snap = { 0 };
	int slen, rlen;
	int replace_nl, replace_tail;
	int filerow, match_col;
	int start_row, start_col, end_row, end_col;
	int count = 0, replace_all = 0;

	query_replace_bounds(&start_row, &start_col, &end_row, &end_col);

	if (editor_read_line_with_history(fd, "Query replace: ", search,
		sizeof(search), &query_replace_history)
		< 0
	    || !search[0]) {
		return;
	}
	int fold;

	if (editor_read_line_with_history(fd, "Replace with: ", replace,
		sizeof(replace), &query_replace_history)
	    < 0) {
		return;
	}

	slen = strlen(search);
	rlen = strlen(replace);
	if (slen == 1 && search[0] == '\n') {
		editor_query_replace_newline(
		    fd, replace, rlen, start_row, end_row);
		return;
	}
	filerow = start_row;
	match_col = start_col;
	fold = !query_has_upper(search, slen);
	replacement_shape(replace, rlen, &replace_nl, &replace_tail);

	while (filerow <= end_row && filerow < bcur()->numrows) {
		char *match = case_strstr(
		    bcur()->row[filerow].chars + match_col, search, fold);
		enum replace_answer answer = REPLACE_DO;

		if (!match) {
			filerow++;
			match_col = 0;
			continue;
		}
		match_col = match - bcur()->row[filerow].chars;
		/* A match straddling the region end is outside it, and so is
		 * every later match on this row. */
		if (filerow == end_row && match_col + slen > end_col) {
			break;
		}

		editor_goto_line_direct(filerow + 1, match_col + 1);

		/* Highlight the match.  The span is in chars offsets;
		 * hl_snapshot_mark() converts it, so a tab before the match
		 * on this line does not shift the highlight. */
		hl_snapshot_restore(&snap);
		if (hl_snapshot_mark(filerow, match_col, slen, &snap) != 0) {
			editor_set_status_message("Out of memory");
			running = 0;
			return;
		}

		if (!replace_all) {
			editor_set_status_message(
			    "Query replace %s \"%s\" with \"%s\"? (y/n/!/q)",
			    fold ? "[fold]" : "[case]", search, replace);
			editor_refresh_screen();
			answer = query_replace_answer(editor_read_key(fd));
		}

		if (answer == REPLACE_STOP) {
			break;
		}
		if (answer == REPLACE_ALL) {
			replace_all = 1;
		}

		if (answer != REPLACE_SKIP) {
			/* Where the region's end sits relative to the match,
			 * measured before the row it shares with it moves. */
			int after = end_col - match_col - slen;
			int resume;

			/* Restore the snapshot while it still matches this
			 * row's render size.  A length-changing replacement
			 * regenerates hl, so restoring later would use the old
			 * allocation with the new rsize. */
			hl_snapshot_restore(&snap);

			/* A replacement is one user operation: one row
			 * rebuild, and one C-_ that removes the replacement
			 * span and restores the original match. */
			if (!editor_row_replace_range(filerow, match_col, slen,
				replace, rlen, KG_EDIT_USER)) {
				break;
			}

			/* A replacement holding a separator splits the row, so
			 * both the resume point and the region's end land on a
			 * row that did not exist a moment ago.  Without this
			 * the next search would start past the end of the (now
			 * much shorter) row it thinks it is still on. */
			resume = replace_nl ? replace_tail : match_col + rlen;
			if (filerow == end_row) {
				end_col = resume + after;
			}
			end_row += replace_nl;
			filerow += replace_nl;
			match_col = resume;
			count++;
		} else {
			match_col
			    = skip_one_glyph(&bcur()->row[filerow], match_col);
		}
	}

	hl_snapshot_restore(&snap);
	editor_set_status_message(
	    count ? "Replaced %d occurrence%s." : "No replacements made.",
	    count, count == 1 ? "" : "s");
}

static int replacement_span_len(const struct kg_match *match, int span)
{
	int start, end;

	if (span >= match->nspans) {
		return 0;
	}
	start = match->spans[span].start;
	end = match->spans[span].end;
	if (start < 0 || end < start) {
		return 0;
	}
	return end - start;
}

static int replacement_length(
    const char *replace, const struct kg_match *match, int *out_len)
{
	size_t len = 0;
	int i = 0;

	while (replace[i] != '\0') {
		if (replace[i] == '\\') {
			char next = replace[i + 1];
			if (next == '\0') {
				len++;
				break;
			}
			if (next == '&') {
				len += replacement_span_len(match, 0);
				i += 2;
			} else if (next >= '1' && next <= '9') {
				len += replacement_span_len(match, next - '0');
				i += 2;
			} else {
				len += 1 + (next != '\\');
				i += 2;
			}
		} else {
			len++;
			i++;
		}
		if (len > INT_MAX) {
			return 0;
		}
	}
	*out_len = (int)len;
	return 1;
}

static char *expand_replacement(const char *replace,
    const struct kg_match *match, const char *chars, int *out_len)
{
	char *out;
	int i = 0;
	int len = 0;

	if (!replacement_length(replace, match, out_len)) {
		return NULL;
	}
	out = malloc((size_t)*out_len + 1);
	if (!out) {
		return NULL;
	}
	while (replace[i] != '\0') {
		if (replace[i] == '\\') {
			char next = replace[i + 1];
			if (next == '\0') {
				out[len++] = '\\';
				break;
			}
			if (next == '&' || (next >= '1' && next <= '9')) {
				int span = next == '&' ? 0 : next - '0';
				int span_len
				    = replacement_span_len(match, span);
				if (span_len > 0) {
					memcpy(out + len,
					    chars + match->spans[span].start,
					    span_len);
					len += span_len;
				}
				i += 2;
			} else {
				out[len] = '\\';
				len += next != '\\';
				out[len++] = next;
				i += 2;
			}
		} else {
			out[len++] = replace[i++];
		}
	}
	out[len] = '\0';
	return out;
}

void editor_query_replace_regexp(int fd)
{
	char search[KILO_QUERY_LEN + 1] = { 0 };
	char replace[KILO_QUERY_LEN + 1] = { 0 };
	struct hl_snapshot snap = { 0 };
	int filerow, match_col;
	int start_row, start_col, end_row, end_col;
	int count = 0, replace_all = 0, since_poll = 0;
	int guard_row = -1, guard_left = 0, too_complex = 0;

	query_replace_bounds(&start_row, &start_col, &end_row, &end_col);

	if (editor_read_line_with_history(fd, "Query replace regexp: ", search,
		sizeof(search), &query_replace_history)
		< 0
	    || !search[0]) {
		return;
	}
	if (editor_read_line_with_history(fd, "Replace with: ", replace,
		sizeof(replace), &query_replace_history)
	    < 0) {
		return;
	}

	int fold = !query_has_upper(search, strlen(search));
	struct kg_regex rx;
	int rx_flags = fold ? KG_REGEX_ICASE : 0;
	if (kg_regex_compile(&rx, search, rx_flags) != KG_REGEX_OK) {
		editor_set_status_message("Invalid regular expression");
		return;
	}

	filerow = start_row;
	match_col = start_col;

	while (filerow <= end_row && filerow < bcur()->numrows && running) {
		erow *row = &bcur()->row[filerow];
		struct kg_match match_res;
		enum replace_answer answer = REPLACE_DO;
		int left = row->size - match_col;
		int status;

		/* Emacs' replace loop ends as soon as point reaches the end of
		 * the region -- the end of the buffer when there is none -- so
		 * the empty match sitting exactly there is never replaced, and
		 * a region of two characters takes two replacements rather
		 * than three. */
		if (filerow == end_row && match_col >= end_col) {
			break;
		}
		status = kg_regex_match_forward(
		    &rx, row->chars, match_col, &match_res);

		/* Each step of this loop, accepted or refused, must leave less
		 * of the row ahead of the scan than the step before it did.
		 * Anything else is a progress bug in the arithmetic below, and
		 * continuing would rewrite the buffer without end. */
		if (filerow == guard_row && left >= guard_left) {
			editor_set_status_message(
			    "Internal error: query-replace made no progress");
			break;
		}
		guard_row = filerow;
		guard_left = left;

		if (status == KG_REGEX_TOO_COMPLEX) {
			/* Stop before touching text: the row may well hold a
			 * match this engine could not reach, and pretending it
			 * does not would silently skip it. */
			too_complex = 1;
			break;
		}
		if (status != KG_REGEX_OK) {
			filerow++;
			match_col = 0;
			continue;
		}

		int match_start = match_res.spans[0].start;
		int match_end = match_res.spans[0].end;
		int match_len = match_end - match_start;
		/* Where the scan resumes, taken before the row is edited: an
		 * empty match consumed nothing, so only a whole glyph gets it
		 * past this position, and -1 says the row is exhausted. */
		int next_raw = kg_regex_next_offset(
		    row->chars, row->size, &match_res.spans[0]);

		/* A match straddling the region end is outside it, and so is
		 * every later match on this row. */
		if (filerow == end_row && match_end > end_col) {
			break;
		}

		editor_goto_line_direct(filerow + 1, match_start + 1);

		hl_snapshot_restore(&snap);
		if (hl_snapshot_mark(filerow, match_start, match_len, &snap)
		    != 0) {
			editor_set_status_message("Out of memory");
			running = 0;
			return;
		}

		int expanded_len = 0;
		char *expanded = expand_replacement(
		    replace, &match_res, row->chars, &expanded_len);
		if (!expanded) {
			editor_set_status_message("Out of memory");
			running = 0;
			hl_snapshot_restore(&snap);
			return;
		}

		if (!replace_all) {
			editor_set_status_message(
			    "Query replace regexp %s \"%s\" with \"%s\"? "
			    "(y/n/!/q)",
			    fold ? "[fold]" : "[case]", search, expanded);
			editor_refresh_screen();
			answer = query_replace_answer(editor_read_key(fd));
		} else if (++since_poll >= 256) {
			/* "!" answers for the user, so nothing here ever reads
			 * a key: poll for C-g now and then, or a replacement
			 * over a large region cannot be stopped. */
			since_poll = 0;
			if (editor_check_quit_pending()) {
				answer = REPLACE_STOP;
			}
		}

		if (answer == REPLACE_STOP) {
			free(expanded);
			break;
		}
		if (answer == REPLACE_ALL) {
			replace_all = 1;
		}

		if (answer != REPLACE_SKIP) {
			hl_snapshot_restore(&snap);
			if (!editor_row_replace_range(filerow, match_start,
				match_len, expanded, expanded_len,
				KG_EDIT_USER)) {
				free(expanded);
				break;
			}

			if (filerow == end_row) {
				end_col += expanded_len - match_len;
			}
			/* The bytes after the match moved by the same delta as
			 * the region end did, so the resume point moves with
			 * them. */
			match_col = next_raw < 0
			    ? -1
			    : next_raw + expanded_len - match_len;
			count++;
		} else {
			match_col = next_raw;
		}
		free(expanded);
		if (match_col < 0 || match_col > bcur()->row[filerow].size) {
			filerow++;
			match_col = 0;
		}
	}

	hl_snapshot_restore(&snap);
	if (too_complex) {
		editor_set_status_message(
		    "Regexp too complex; stopped after %d replacement%s.",
		    count, count == 1 ? "" : "s");
		return;
	}
	editor_set_status_message(
	    count ? "Replaced %d occurrence%s." : "No replacements made.",
	    count, count == 1 ? "" : "s");
}
