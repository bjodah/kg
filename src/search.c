/* ============================ Find / Replace ===============================
 */

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "decor.h"
#include "def.h"
#include "edit.h"
#include "keyevent.h"
#include "marker.h"
#include "regex.h"
#include "yank.h"

#define KILO_QUERY_LEN 256

/* Key sets the search prompts ask about; see key_in_list(). */
static const struct key_event erase_keys[]
    = { { KEY_BASE_DELETE, 0 }, { 'h', KEY_MOD_CTRL }, { KEY_BASE_DEL, 0 } };
static const struct key_event replace_stop_keys[]
    = { { KEY_BASE_ESC, 0 }, { 'g', KEY_MOD_CTRL }, { 'q', 0 } };
static const struct key_event replace_do_keys[]
    = { { 'y', 0 }, { KEY_BASE_RET, 0 }, { ' ', 0 } };
static const struct key_event isearch_end_keys[]
    = { { KEY_BASE_ESC, 0 }, { KEY_BASE_RET, 0 }, { 'g', KEY_MOD_CTRL } };
static const struct key_event isearch_forward_keys[]
    = { { KEY_BASE_RIGHT, 0 }, { KEY_BASE_DOWN, 0 }, { 's', KEY_MOD_CTRL } };
static const struct key_event isearch_backward_keys[]
    = { { KEY_BASE_LEFT, 0 }, { KEY_BASE_UP, 0 }, { 'r', KEY_MOD_CTRL } };
static const struct key_event isearch_history_keys[]
    = { { 'p', KEY_MOD_META }, { 'n', KEY_MOD_META } };

/* Emacs keeps literal and regexp searches in separate rings (search-ring
 * and regexp-search-ring) and runs every query-replace prompt — both
 * strings, both the literal and the regexp command — off one shared
 * query-replace-history.  These do the same. */
static struct minibuf_history search_history;
static struct minibuf_history regexp_search_history;
static struct minibuf_history query_replace_history;

/* The current match is one temporary decoration, never a byte painted
 * into row->hl: a fresh find, a query edit, an accepted or cancelled
 * search, or a query-replace answer all retire it the same way, through
 * search_match_decor_drop() below.  There is exactly one such decoration
 * live at a time per caller, so ownership is never ambiguous -- see the
 * comment on search_match_decor() for the gravity choice and on each
 * call site below for why that site is the one deleting it. */

#define KG_SEARCH_MATCH_PRIORITY 0

/* Create the current-match decoration spanning the `len` buffer bytes
 * from `start_pos`.  Byte positions rather than a row and a column
 * because a match may cross a row boundary -- a query carrying a newline
 * is what isearch_match_at() below exists for -- and a row separator is
 * one byte of the same count.  decor.h's documented gravity pair for a
 * match highlight -- KG_MARKER_GRAV_RIGHT for the start,
 * KG_MARKER_GRAV_LEFT for the end -- keeps text typed at either edge
 * outside the match rather than growing it.  Returns a handle that
 * resolves as gone on allocation failure; the caller is expected to
 * treat `id == 0` as out-of-memory, matching kg_marker_create()'s
 * convention elsewhere in this file. */
static struct kg_decor_handle search_match_decor(size_t start_pos, size_t len)
{
	size_t end_pos = start_pos + len;

	return kg_decor_create(bcur(), start_pos, end_pos, KG_MARKER_GRAV_RIGHT,
	    KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, KG_SEARCH_MATCH_PRIORITY,
	    true);
}

/* Delete `*decor` (a no-op if it is already the null handle or already
 * gone) and clear the caller's copy, so a stale handle is never reused
 * by accident. */
static void search_match_decor_drop(struct kg_decor_handle *decor)
{
	kg_decor_delete(*decor);
	*decor = (struct kg_decor_handle) { 0 };
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
    char *s, const char *query, int limit, int qlen, int fold)
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

static int case_equal(char a, char b, int fold)
{
	if (!fold) {
		return a == b;
	}
	return tolower((unsigned char)a) == tolower((unsigned char)b);
}

/* Does the literal `query` match starting at (row, col)?  A '\n' in the
 * query matches the separator that ends a row, so a search string
 * carrying one -- put there by C-q, isearch-quote-char, or carried in by
 * a yanked multi-line kill -- spans rows.  The regexp kind has no
 * equivalent: kg_regex_match_forward() is handed one row at a time.
 * kg's last row has no separator after it, the way a file's
 * final newline is a terminator rather than a row of its own, so a query
 * can never match past the end of the buffer.  Everything here is a
 * chars byte offset. */
static int isearch_match_at(int row, int col, const char *query, int fold)
{
	const struct editor_buffer *b = bcur();

	for (; *query != '\0'; query++) {
		const erow *r = &b->row[row];

		if (*query == '\n') {
			if (col != r->size || row + 1 >= b->numrows) {
				return 0;
			}
			row++;
			col = 0;
			continue;
		}
		if (col >= r->size
		    || !case_equal(r->chars[col], *query, fold)) {
			return 0;
		}
		col++;
	}
	return 1;
}

/* Where a newline-bearing query matches in `row`, or -1.  `origin` says
 * this is the row the search started from, whose `col` is point: a
 * backward match found there would end on a later row, i.e. after point,
 * so there is nothing to find.  Every other row is scanned whole, which
 * is the same rule the single-row backward path (isearch_find_last_before)
 * already applies to a wrapped row. */
static int isearch_multiline_col(
    int row, int col, int direction, const char *query, int fold, int origin)
{
	const erow *r = &bcur()->row[row];
	int i, best = -1;

	if (direction < 0 && origin) {
		return -1;
	}
	for (i = direction > 0 ? col : 0; i <= r->size; i++) {
		if (!isearch_match_at(row, i, query, fold)) {
			continue;
		}
		if (direction > 0) {
			return i;
		}
		best = i;
	}
	return best;
}

/* The literal query's match column in `row`, or -1.  A query without a
 * newline never leaves its row, so it keeps the plain strstr scan; one
 * with a newline goes through the cross-row matcher above. */
static int isearch_literal_col(int row, int col, int direction,
    const char *query, int qlen, int fold, int origin)
{
	const erow *r = &bcur()->row[row];
	char *match;

	if (memchr(query, '\n', (size_t)qlen) != NULL) {
		return isearch_multiline_col(
		    row, col, direction, query, fold, origin);
	}
	if (direction > 0) {
		match = case_strstr(r->chars + col, query, fold);
	} else {
		match = isearch_find_last_before(
		    r->chars, query, col, qlen, fold);
	}
	return match != NULL ? (int)(match - r->chars) : -1;
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
			int found = isearch_literal_col(
			    current, col, direction, query, qlen, fold, i == 0);

			if (found >= 0) {
				*match_row = current;
				*match_col = found;
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
	if (kg_mark_is_set(bcur()) && bcur()->mark_highlight
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

/* C-y and M-y in the search prompt, measured against Emacs 31
 * (isearch-yank-kill / isearch-yank-pop-only): C-y appends the kill
 * ring's newest entry to the query; M-y directly after a yank replaces
 * the span that yank appended with the next-older entry, wrapping past
 * the oldest; and M-y with NO yank before it appends the newest entry
 * exactly as C-y does -- measured, Emacs rotates nothing there.
 * `*yank_start` is the appended span's start, -1 while the previous
 * keystroke was not a yank (the caller resets it on every other key),
 * and since yanks only ever append, the span's end is always `*qlen`.
 * Returns 1 when the key was one of the two: the caller re-searches,
 * except when `*ring_note` was raised instead because the ring is
 * empty.  The query is a bounded NUL-terminated line: the span stops
 * at an embedded NUL and is clamped bytewise to the query's capacity
 * (Emacs, yanking into a real buffer, has neither bound). */
static int isearch_yank(struct key_event c, char *query, int *qlen,
    int *yank_start, int *yank_index, int *ring_note)
{
	char *text;
	const char *nul;
	size_t elen = 0;
	int n, next, pop;

	if (!KEY_IS(c, 'y', KEY_MOD_CTRL) && !KEY_IS(c, 'y', KEY_MOD_META)) {
		return 0;
	}
	pop = KEY_IS(c, 'y', KEY_MOD_META) && *yank_start >= 0
	    && killring.count > 0;
	next = pop ? (*yank_index + 1) % killring.count : 0;
	/* Fetch before touching the query, so an empty ring or a failed
	 * allocation leaves the previously yanked span in place. */
	text = kill_ring_entry_repeated(next, 1, &elen);
	if (text == NULL) {
		*yank_start = -1;
		*ring_note = 1;
		return 1;
	}
	if (pop) {
		*qlen = *yank_start;
	} else {
		*yank_start = *qlen;
	}
	nul = memchr(text, '\0', elen);
	n = (int)(nul != NULL ? (size_t)(nul - text) : elen);
	if (n > KILO_QUERY_LEN - *qlen) {
		n = KILO_QUERY_LEN - *qlen;
	}
	memcpy(query + *qlen, text, (size_t)n);
	*qlen += n;
	query[*qlen] = '\0';
	*yank_index = next;
	free(text);
	return 1;
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

/* Append `n` bytes to the query, or nothing at all when they do not fit:
 * a half-appended character would be neither what was typed nor a
 * search string.  Returns 1 when the query grew. */
static int isearch_append(char *query, int *qlen, const char *bytes, int n)
{
	if (n <= 0 || n > KILO_QUERY_LEN - *qlen) {
		return 0;
	}
	memcpy(query + *qlen, bytes, (size_t)n);
	*qlen += n;
	query[*qlen] = '\0';
	return 1;
}

/* The keys that put text into the search string, and the only ones that
 * do: a printable ASCII byte, the lead byte of a multi-byte character
 * the terminal is sending one byte at a time, and C-q, Emacs'
 * isearch-quote-char, which takes the very next byte whatever it is --
 * C-q C-j searches for a newline, C-q TAB for a tab.  The next byte is
 * data, not a key, so it is read here rather than by the decoder,
 * exactly as cmd_quoted_insert() reads it for the buffer.  Returns 1
 * when the query grew, so the caller re-searches. */
static int isearch_insert_key(
    int fd, struct key_event c, char *query, int *qlen)
{
	char seq[4];

	if (KEY_IS(c, 'q', KEY_MOD_CTRL)) {
		seq[0] = (char)editor_read_raw_byte(fd);
		return running ? isearch_append(query, qlen, seq, 1) : 0;
	}
	if (c.mods != 0) {
		return 0;
	}
	if (ascii_is_print(c.base)) {
		seq[0] = (char)c.base;
		return isearch_append(query, qlen, seq, 1);
	}
	if (c.base >= 0x80 && c.base <= 0xFF) {
		return isearch_append(query, qlen, seq,
		    editor_read_utf8_seq(fd, (int)c.base, seq));
	}
	return 0;
}

static void isearch_drop_marker(struct kg_marker_handle *marker)
{
	kg_marker_delete(*marker);
	*marker = (struct kg_marker_handle) { 0 };
}

static int isearch_set_marker(struct kg_marker_handle *marker, size_t position)
{
	if (kg_marker_set_position(*marker, position) == KG_MARKER_OK) {
		return 1;
	}
	*marker = kg_marker_create(bcur(), position, KG_MARKER_GRAV_LEFT);
	return marker->id != 0;
}

/* Where an interactive search started: the four view registers ESC has to
 * put back, since abandoning a search abandons the scrolling it did. */
struct isearch_origin {
	int cx, cy, coloff, rowoff;
};

/* What the search has found so far: the marker sitting at the current
 * match, the decoration showing it, where to search from while there is
 * no match yet, and whether the last attempt ran out of engine budget. */
struct isearch_match {
	struct kg_marker_handle marker;
	struct kg_decor_handle decor;
	int start_row;
	int start_col;
	int too_complex;
};

enum isearch_step {
	ISEARCH_STEP_OK,
	ISEARCH_STEP_NOMEM,
};

/* Put point and the viewport back where the search began.  The marker is
 * the truth when it survived the session's edits; the plain saved
 * registers are the fallback when it did not. */
static void isearch_restore_point(
    struct kg_marker_handle saved_point, const struct isearch_origin *origin)
{
	int row, col;

	if (!kg_marker_get_row_col(saved_point, &row, &col)) {
		wcur()->cx = origin->cx;
		wcur()->cy = origin->cy;
		wcur()->coloff = origin->coloff;
		wcur()->rowoff = origin->rowoff;
		return;
	}
	wcur()->coloff = origin->coloff;
	wcur()->rowoff = origin->rowoff;
	wcur()->cy = row - origin->rowoff;
	wcur()->cx = col - origin->coloff;
	if (wcur()->cy < 0 || wcur()->cy >= wcur()->h || wcur()->cx < 0
	    || wcur()->cx >= wcur()->w) {
		editor_reveal_position_centered(row, col);
	}
}

/* One search step: find the next occurrence in `search_dir` from wherever
 * the last match left off, move the match marker and its decoration onto
 * it, and reveal it.  Everything here is chars byte offsets.  Returns
 * NOMEM when the caller must give up; anything else -- an empty regexp
 * query, a pattern that will not compile, no match -- is a normal step
 * that simply leaves the match where it was. */
static enum isearch_step isearch_advance(struct isearch_match *m,
    enum search_kind kind, char *query, int qlen, int search_dir)
{
	struct kg_regex rx = { 0 };
	int current = m->start_row;
	int col = m->start_col;
	int match_row = -1, match_col = -1, match_len = 0;
	int fold = !query_has_upper(query, qlen);
	size_t start_pos;
	int match;

	if (kind == SEARCH_REGEXP) {
		if (qlen == 0) {
			return ISEARCH_STEP_OK;
		}
		if (kg_regex_compile(&rx, query, fold ? KG_REGEX_ICASE : 0)
		    != KG_REGEX_OK) {
			editor_set_status_message(
			    "Regexp I-search [bad regexp]: %s", query);
			return ISEARCH_STEP_OK;
		}
	}

	if (kg_marker_get_row_col(m->marker, &current, &col)) {
		col += search_dir > 0 ? 1 : 0;
	}
	match = isearch_find_match(current, col, search_dir, kind, &rx, query,
	    qlen, fold, &match_row, &match_col, &match_len);
	m->too_complex = match < 0;

	/* Drop the previous match's decoration unconditionally -- a new
	 * match, a query edit, or no match at all all retire it the same
	 * way -- before deciding whether there is a new one to show. */
	search_match_decor_drop(&m->decor);
	if (match <= 0) {
		return ISEARCH_STEP_OK;
	}

	start_pos = buffer_row_col_to_position(bcur(), match_row, match_col);
	if (!isearch_set_marker(&m->marker, start_pos)) {
		return ISEARCH_STEP_NOMEM;
	}
	m->decor = search_match_decor(start_pos, (size_t)match_len);
	if (m->decor.id == 0) {
		return ISEARCH_STEP_NOMEM;
	}
	/* Point lands at the far end of the match in the search's own
	 * direction, which for a match carrying a newline is on a later
	 * row -- so the end is named as a byte position and converted
	 * back, never as this row's column plus a length. */
	if (search_dir > 0) {
		buffer_position_to_row_col(bcur(),
		    start_pos + (size_t)match_len, &match_row, &match_col);
	}
	editor_reveal_position_centered(match_row, match_col);
	return ISEARCH_STEP_OK;
}

static void do_isearch(int fd, int direction, enum search_kind kind)
{
	char query[KILO_QUERY_LEN + 1] = { 0 };
	char draft[KILO_QUERY_LEN + 1] = { 0 };
	struct minibuf_history *hist
	    = kind == SEARCH_REGEXP ? &regexp_search_history : &search_history;
	int hist_index = -1; /* -1 == the query being typed, not a recall */
	struct isearch_origin origin
	    = { wcur()->cx, wcur()->cy, wcur()->coloff, wcur()->rowoff };
	struct isearch_match m = { .start_row = wcur()->rowoff + wcur()->cy };
	struct kg_marker_handle saved_point = { 0 };
	int find_next = 0; /* if 1 search next, if -1 search prev. */
	int qlen = 0;
	int yank_start = -1; /* see isearch_yank(); -1: no yank to pop */
	int yank_index = 0;
	int ring_note = 0; /* one-shot "[kill ring empty]" for the prompt */

	/* Everything in this function is a chars byte offset: the search
	 * reads row->chars, so a TAB is one character and never the eight
	 * columns it is drawn as.  The match decoration below is anchored by
	 * flat buffer-byte markers instead; converting to render space for
	 * display is the renderer's job now, not this file's. */
	m.start_col = wcur()->coloff + wcur()->cx;
	saved_point = kg_marker_create(bcur(),
	    buffer_row_col_to_position(bcur(), m.start_row, m.start_col),
	    KG_MARKER_GRAV_LEFT);
	if (saved_point.id == 0) {
		editor_set_status_message("Out of memory");
		running = 0;
		return;
	}

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
		    ring_note		? "[kill ring empty]"
			: !rx_valid	? "[bad regexp]"
			: m.too_complex ? "[too complex]"
			: fold		? "[fold]"
					: "[case]",
		    query);
		editor_refresh_screen();

		c = editor_read_key(fd);
		ring_note = 0;
		/* Yank-pop eligibility is "the key before this one was a
		 * yank", exactly the minibuffer's rule. */
		if (!KEY_IS(c, 'y', KEY_MOD_CTRL)
		    && !KEY_IS(c, 'y', KEY_MOD_META)) {
			yank_start = -1;
		}
		if (KEY_IN_LIST(erase_keys, c)) {
			/* Drop a whole character, so backspacing over a
			 * multi-byte glyph never leaves half of it in the
			 * search string. */
			if (qlen != 0) {
				qlen = utf8_glyph_start_before(
				    query, qlen, qlen);
				query[qlen] = '\0';
			}
			isearch_drop_marker(&m.marker);
			find_next = direction;
		} else if (KEY_IN_LIST(isearch_end_keys, c)) {
			if (KEY_IS(c, KEY_BASE_ESC, 0)) {
				isearch_restore_point(saved_point, &origin);
			}
			/* Emacs records the query when the search is
			 * accepted, never when it is abandoned. */
			if (KEY_IS(c, KEY_BASE_RET, 0)) {
				minibuf_history_add(hist, query);
			}
			/* Accept or cancel: the search is over, so its match
			 * decoration goes with it. */
			search_match_decor_drop(&m.decor);
			isearch_drop_marker(&m.marker);
			isearch_drop_marker(&saved_point);
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
				isearch_drop_marker(&m.marker);
				find_next = direction;
			}
		} else if (isearch_yank(c, query, &qlen, &yank_start,
			       &yank_index, &ring_note)) {
			/* Append (or pop) and re-search immediately, as
			 * Emacs does; an empty ring changed nothing, so
			 * only the note repaints. */
			if (!ring_note) {
				isearch_drop_marker(&m.marker);
				find_next = direction;
			}
		} else if (isearch_insert_key(fd, c, query, &qlen)) {
			isearch_drop_marker(&m.marker);
			find_next = direction;
		} else if (isearch_handoff_key(fd, c)) {
			/* A handoff ends the search on its match, which in
			 * Emacs commits the query just like Enter does.  The
			 * key may have edited the buffer (C-d): its markers
			 * relocated the decoration through that edit before
			 * this ever runs, so there is nothing left to
			 * reconcile here -- this call is the search's one
			 * and only owner retiring it, whatever the edit did
			 * to its span. */
			minibuf_history_add(hist, query);
			search_match_decor_drop(&m.decor);
			isearch_drop_marker(&m.marker);
			isearch_drop_marker(&saved_point);
			return;
		}

		/* Search occurrence. */
		if (find_next) {
			int search_dir = find_next;

			find_next = 0;
			if (isearch_advance(&m, kind, query, qlen, search_dir)
			    == ISEARCH_STEP_NOMEM) {
				editor_set_status_message("Out of memory");
				running = 0;
				isearch_drop_marker(&m.marker);
				isearch_drop_marker(&saved_point);
				return;
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
	struct kg_decor_handle match_decor = { 0 };
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

		/* Highlight the match: one temporary decoration, replacing
		 * whatever the previous iteration's answer left behind. */
		search_match_decor_drop(&match_decor);
		match_decor = search_match_decor(
		    buffer_row_col_to_position(bcur(), filerow, match_col),
		    (size_t)slen);
		if (match_decor.id == 0) {
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

			/* The match is about to be edited; retire its
			 * decoration first.  Ownership stays with this loop
			 * the whole time -- it is never handed to the edit,
			 * so there is only ever one place that deletes it. */
			search_match_decor_drop(&match_decor);

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

	search_match_decor_drop(&match_decor);
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
	struct kg_decor_handle match_decor = { 0 };
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

		/* Highlight the match: one temporary decoration, replacing
		 * whatever the previous iteration's answer left behind. */
		search_match_decor_drop(&match_decor);
		match_decor = search_match_decor(
		    buffer_row_col_to_position(bcur(), filerow, match_start),
		    (size_t)match_len);
		if (match_decor.id == 0) {
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
			search_match_decor_drop(&match_decor);
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
			/* The match is about to be edited; retire its
			 * decoration first -- this loop is its only owner,
			 * start to finish. */
			search_match_decor_drop(&match_decor);
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

	search_match_decor_drop(&match_decor);
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
