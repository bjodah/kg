/* ============================ Find / Replace ===============================
 */

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "localvars.h"
#include "regex.h"

#define KILO_QUERY_LEN 256

#define RESTORE_HL                                                             \
	do {                                                                   \
		if (saved_hl) {                                                \
			memcpy(editor.row[saved_hl_line].hl, saved_hl,         \
			    editor.row[saved_hl_line].rsize);                  \
			free(saved_hl);                                        \
			saved_hl = NULL;                                       \
		}                                                              \
	} while (0)

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

static int isearch_find_match(int start_row, int start_col, int direction,
    enum search_kind kind, const struct kg_regex *rx, char *query, int qlen,
    int fold, int *match_row, int *match_col, int *match_len)
{
	int current, i;

	if (editor.numrows == 0 || qlen == 0) {
		return 0;
	}

	if (start_row < 0) {
		start_row = 0;
	} else if (start_row >= editor.numrows) {
		start_row = editor.numrows - 1;
	}

	current = start_row;
	for (i = 0; i < editor.numrows; i++) {
		erow *row = &editor.row[current];
		int col
		    = (i == 0) ? start_col : (direction > 0 ? 0 : row->rsize);

		if (col < 0) {
			col = 0;
		} else if (col > row->rsize) {
			col = row->rsize;
		}

		if (kind == SEARCH_REGEXP) {
			struct kg_match match_res;
			int status;
			if (direction > 0) {
				status = kg_regex_match_forward(
				    rx, row->render, col, &match_res);
			} else {
				status = kg_regex_match_backward(
				    rx, row->render, col, &match_res);
			}
			if (status == KG_REGEX_OK) {
				*match_row = current;
				*match_col = match_res.spans[0].start;
				*match_len = match_res.spans[0].end
				    - match_res.spans[0].start;
				return 1;
			}
		} else {
			char *match;
			if (direction > 0) {
				match = case_strstr(
				    row->render + col, query, fold);
			} else {
				match = isearch_find_last_before(
				    row->render, query, col, qlen, fold);
			}

			if (match) {
				*match_row = current;
				*match_col = match - row->render;
				*match_len = qlen;
				return 1;
			}
		}

		current += direction;
		if (current < 0) {
			current = editor.numrows - 1;
		} else if (current == editor.numrows) {
			current = 0;
		}
	}
	return 0;
}

static void query_replace_newline_at(int rowidx, char *replace, int rlen)
{
	erow *row = &editor.row[rowidx];
	int join_col = row->size;
	int i;

	undo_push(UNDO_KILL_TEXT, rowidx, join_col, 0, "\n", 1);
	undo_push(UNDO_YANK_TEXT, rowidx, join_col, 0, replace, rlen);

	suppress_undo = 1;
	for (i = 0; i < rlen; i++) {
		editor_row_insert_char(
		    row, join_col + i, (unsigned char)replace[i]);
	}
	editor_row_append_string(
	    row, editor.row[rowidx + 1].chars, editor.row[rowidx + 1].size);
	editor_del_row(rowidx + 1);
	suppress_undo = 0;
	editor_update_row(row);
	editor.dirty++;
}

/* Emacs restricts query-replace to the region when one is active; otherwise
 * it runs from point to the end of the buffer.  The bounds are returned as
 * an inclusive row range with a column on each end. */
static void query_replace_bounds(
    int *start_row, int *start_col, int *end_row, int *end_col)
{
	if (editor.mark_set && editor.mark_highlight
	    && editor_region_bounds(start_row, start_col, end_row, end_col)) {
		return;
	}
	*start_row = editor.rowoff + editor.cy;
	*start_col = editor.coloff + editor.cx;
	*end_row = editor.numrows > 0 ? editor.numrows - 1 : 0;
	*end_col = editor.numrows > 0 ? editor.row[*end_row].size : 0;
}

static void editor_query_replace_newline(
    int fd, char *replace, int rlen, int start_row, int end_row)
{
	int filerow = start_row;
	int count = 0, replace_all = 0;

	/* The newline ending row R is inside the region only while R is above
	 * the last region row. */
	while (filerow < end_row && filerow < editor.numrows - 1) {
		int c;

		editor_cursor_goto(filerow, editor.row[filerow].size);
		if (!replace_all) {
			editor_set_status_message(
			    "Replace \"^J\" with \"%s\"? (y/n/!/q)", replace);
			editor_refresh_screen();
			c = editor_read_key(fd);
		} else {
			c = 'y';
		}

		if (c == ESC || c == CTRL_G || c == 'q') {
			break;
		}
		if (c == '!') {
			replace_all = 1;
			c = 'y';
		}

		if (c == 'y' || c == ENTER || c == ' ') {
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

static int isearch_handoff_key(int fd, int c)
{
	switch (c) {
	case KEY_NULL:
		editor_set_mark();
		return 1;
	case CTRL_A:
		editor_set_status_message("");
		editor_move_cursor(HOME_KEY);
		return 1;
	case CTRL_B:
		editor_set_status_message("");
		editor_move_cursor(ARROW_LEFT);
		return 1;
	case CTRL_D:
		if (editor.readonly) {
			editor_set_status_message("Buffer is read-only");
		} else {
			editor_set_status_message("");
			editor_del_forward_char();
		}
		return 1;
	case CTRL_E:
		editor_set_status_message("");
		editor_move_cursor(END_KEY);
		return 1;
	case CTRL_F:
		editor_set_status_message("");
		editor_move_cursor(ARROW_RIGHT);
		return 1;
	case CTRL_N:
		editor_set_status_message("");
		editor_move_cursor(ARROW_DOWN);
		return 1;
	case CTRL_P:
		editor_set_status_message("");
		editor_move_cursor(ARROW_UP);
		return 1;
	case ARROW_LEFT:
	case ARROW_RIGHT:
	case ARROW_UP:
	case ARROW_DOWN:
	case HOME_KEY:
	case END_KEY:
		editor_set_status_message("");
		editor_move_cursor(c);
		return 1;
	case CTRL_HOME:
	case ALT_LT:
		editor_set_status_message("");
		editor_move_to_beginning();
		return 1;
	case CTRL_END:
	case ALT_GT:
		editor_set_status_message("");
		editor_move_to_end();
		return 1;
	case ALT_B:
	case CTRL_ARROW_LEFT:
		editor_set_status_message("");
		editor_move_word_backward();
		return 1;
	case ALT_F:
	case CTRL_ARROW_RIGHT:
		editor_set_status_message("");
		editor_move_word_forward();
		return 1;
	case ALT_M:
		editor_set_status_message("");
		editor_move_to_indentation();
		return 1;
	case ALT_A:
		editor_set_status_message("");
		editor_move_sentence_backward();
		return 1;
	case ALT_E:
		editor_set_status_message("");
		editor_move_sentence_forward();
		return 1;
	case ALT_LBRACE:
	case CTRL_ARROW_UP:
		editor_set_status_message("");
		editor_move_paragraph_backward();
		return 1;
	case ALT_RBRACE:
	case CTRL_ARROW_DOWN:
		editor_set_status_message("");
		editor_move_paragraph_forward();
		return 1;
	default:
		(void)fd;
		return 0;
	}
}

static void do_isearch(int fd, int direction, enum search_kind kind)
{
	char query[KILO_QUERY_LEN + 1] = { 0 };
	int saved_cx = editor.cx, saved_cy = editor.cy;
	int saved_coloff = editor.coloff, saved_rowoff = editor.rowoff;
	int start_row = editor.rowoff + editor.cy;
	int start_col = 0;
	int last_match_row = -1;
	int last_match_col = -1;
	int saved_hl_line = -1; /* No saved HL */
	int find_next = 0; /* if 1 search next, if -1 search prev. */
	char *saved_hl = NULL;
	int qlen = 0;

	/* isearch_find_match() indexes row->render, so the starting point
	 * has to be a render BYTE offset — not a display column.  The two
	 * only coincide on rows made of single-byte, single-width glyphs. */
	if (start_row >= 0 && start_row < editor.numrows) {
		start_col = chars_to_render_col(
		    &editor.row[start_row], editor.coloff + editor.cx);
	}

	while (1) {
		int c;
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

		if (kind == SEARCH_REGEXP) {
			if (!rx_valid) {
				editor_set_status_message(
				    "Regexp I-search [bad regexp]: %s", query);
			} else {
				editor_set_status_message(
				    "Regexp I-search %s: %s",
				    fold ? "[fold]" : "[case]", query);
			}
		} else {
			editor_set_status_message("I-search %s: %s",
			    fold ? "[fold]" : "[case]", query);
		}
		editor_refresh_screen();

		c = editor_read_key(fd);
		if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
			if (qlen != 0) {
				query[--qlen] = '\0';
			}
			last_match_row = -1;
			last_match_col = -1;
			find_next = direction;
		} else if (c == ESC || c == ENTER || c == CTRL_G) {
			if (c == ESC) {
				editor.cx = saved_cx;
				editor.cy = saved_cy;
				editor.coloff = saved_coloff;
				editor.rowoff = saved_rowoff;
			}
			RESTORE_HL;
			editor_set_status_message("");
			return;
		} else if (c == ARROW_RIGHT || c == ARROW_DOWN || c == CTRL_S) {
			find_next = 1;
		} else if (c == ARROW_LEFT || c == ARROW_UP || c == CTRL_R) {
			find_next = -1;
		} else if (isprint(c)) {
			if (qlen < KILO_QUERY_LEN) {
				query[qlen++] = c;
				query[qlen] = '\0';
				last_match_row = -1;
				last_match_col = -1;
				find_next = direction;
			}
		} else if (isearch_handoff_key(fd, c)) {
			RESTORE_HL;
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

				/* Highlight */
				RESTORE_HL;

				if (match) {
					erow *row = &editor.row[match_row];
					last_match_row = match_row;
					last_match_col = match_col;
					if (row->hl) {
						saved_hl_line = match_row;
						saved_hl = malloc(row->rsize);
						if (!saved_hl) {
							editor_set_status_message(
							    "Out of memory");
							running = 0;
							RESTORE_HL;
							return;
						}
						memcpy(saved_hl, row->hl,
						    row->rsize);
						memset(row->hl + match_col,
						    HL_MATCH, match_len);
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

void editor_query_replace(int fd)
{
	char search[KILO_QUERY_LEN + 1] = { 0 };
	char replace[KILO_QUERY_LEN + 1] = { 0 };
	char *saved_hl = NULL;
	int saved_hl_line = -1;
	int slen, rlen;
	int filerow, match_col;
	int start_row, start_col, end_row, end_col;
	int count = 0, replace_all = 0;

	query_replace_bounds(&start_row, &start_col, &end_row, &end_col);

	if (editor_read_line(fd, "Query replace: ", search, sizeof(search)) < 0
	    || !search[0]) {
		return;
	}
	int fold;

	if (editor_read_line(fd, "Replace with: ", replace, sizeof(replace))
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

	while (filerow <= end_row && filerow < editor.numrows) {
		char *match = case_strstr(
		    editor.row[filerow].chars + match_col, search, fold);
		int c;

		if (!match) {
			filerow++;
			match_col = 0;
			continue;
		}
		match_col = match - editor.row[filerow].chars;
		/* A match straddling the region end is outside it, and so is
		 * every later match on this row. */
		if (filerow == end_row && match_col + slen > end_col) {
			break;
		}

		editor_goto_line_direct(filerow + 1, match_col + 1);

		/* Highlight the match.  Convert the chars offset to a render
		 * offset so the highlight lands correctly even when tabs
		 * precede the match on the line. */
		RESTORE_HL;
		{
			erow *row = &editor.row[filerow];
			if (row->hl) {
				int rcol = chars_to_render_col(row, match_col);
				saved_hl_line = filerow;
				saved_hl = malloc(row->rsize);
				if (!saved_hl) {
					editor_set_status_message(
					    "Out of memory");
					running = 0;
					RESTORE_HL;
					return;
				}
				memcpy(saved_hl, row->hl, row->rsize);
				if (rcol + slen <= row->rsize) {
					memset(row->hl + rcol, HL_MATCH, slen);
				}
			}
		}

		if (!replace_all) {
			editor_set_status_message(
			    "Query replace %s \"%s\" with \"%s\"? (y/n/!/q)",
			    fold ? "[fold]" : "[case]", search, replace);
			editor_refresh_screen();
			c = editor_read_key(fd);
		} else {
			c = 'y';
		}

		if (c == ESC || c == CTRL_G || c == 'q') {
			break;
		}
		if (c == '!') {
			replace_all = 1;
			c = 'y';
		}

		if (c == 'y' || c == ENTER || c == ' ') {
			erow *row = &editor.row[filerow];
			int i;

			/* Restore the snapshot while it still matches this
			 * row's render size.  A length-changing replacement
			 * regenerates hl, so restoring later would use the old
			 * allocation with the new rsize. */
			RESTORE_HL;

			/* A replacement is one user operation: one C-_ removes
			 * the replacement span and restores the original match.
			 */
			undo_push(UNDO_REPLACE_TEXT, filerow, match_col, rlen,
			    row->chars + match_col, slen);

			suppress_undo = 1;
			for (i = 0; i < slen; i++) {
				editor_row_del_char(row, match_col);
			}
			for (i = 0; i < rlen; i++) {
				editor_row_insert_char(row, match_col + i,
				    (unsigned char)replace[i]);
			}
			suppress_undo = 0;

			if (filerow == end_row) {
				end_col += rlen - slen;
			}
			match_col += rlen;
			count++;
		} else {
			match_col++;
		}
	}

	RESTORE_HL;
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
	char *saved_hl = NULL;
	int saved_hl_line = -1;
	int filerow, match_col;
	int start_row, start_col, end_row, end_col;
	int count = 0, replace_all = 0;

	query_replace_bounds(&start_row, &start_col, &end_row, &end_col);

	if (editor_read_line(
		fd, "Query replace regexp: ", search, sizeof(search))
		< 0
	    || !search[0]) {
		return;
	}
	if (editor_read_line(fd, "Replace with: ", replace, sizeof(replace))
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

	while (filerow <= end_row && filerow < editor.numrows) {
		erow *row = &editor.row[filerow];
		struct kg_match match_res;
		int status = kg_regex_match_forward(
		    &rx, row->chars, match_col, &match_res);
		int c;

		if (status != KG_REGEX_OK) {
			filerow++;
			match_col = 0;
			continue;
		}

		int match_start = match_res.spans[0].start;
		int match_end = match_res.spans[0].end;
		int match_len = match_end - match_start;

		/* A match straddling the region end is outside it, and so is
		 * every later match on this row. */
		if (filerow == end_row && match_end > end_col) {
			break;
		}

		editor_goto_line_direct(filerow + 1, match_start + 1);

		RESTORE_HL;
		{
			if (row->hl) {
				int rcol_start
				    = chars_to_render_col(row, match_start);
				int rcol_end
				    = chars_to_render_col(row, match_end);
				int rcol_len = rcol_end - rcol_start;
				saved_hl_line = filerow;
				saved_hl = malloc(row->rsize);
				if (!saved_hl) {
					editor_set_status_message(
					    "Out of memory");
					running = 0;
					RESTORE_HL;
					return;
				}
				memcpy(saved_hl, row->hl, row->rsize);
				if (rcol_start + rcol_len <= row->rsize) {
					memset(row->hl + rcol_start, HL_MATCH,
					    rcol_len);
				}
			}
		}

		int expanded_len = 0;
		char *expanded = expand_replacement(
		    replace, &match_res, row->chars, &expanded_len);
		if (!expanded) {
			editor_set_status_message("Out of memory");
			running = 0;
			RESTORE_HL;
			return;
		}

		if (!replace_all) {
			editor_set_status_message(
			    "Query replace regexp %s \"%s\" with \"%s\"? "
			    "(y/n/!/q)",
			    fold ? "[fold]" : "[case]", search, expanded);
			editor_refresh_screen();
			c = editor_read_key(fd);
		} else {
			c = 'y';
		}

		if (c == ESC || c == CTRL_G || c == 'q') {
			free(expanded);
			break;
		}
		if (c == '!') {
			replace_all = 1;
			c = 'y';
		}

		if (c == 'y' || c == ENTER || c == ' ') {
			RESTORE_HL;
			undo_push(UNDO_REPLACE_TEXT, filerow, match_start,
			    expanded_len, row->chars + match_start, match_len);

			suppress_undo = 1;
			for (int i = 0; i < match_len; i++) {
				editor_row_del_char(row, match_start);
			}
			for (int i = 0; i < expanded_len; i++) {
				editor_row_insert_char(row, match_start + i,
				    (unsigned char)expanded[i]);
			}
			suppress_undo = 0;

			if (filerow == end_row) {
				end_col += expanded_len - match_len;
			}
			match_col = match_start + expanded_len
			    + (match_len == 0 ? 1 : 0);
			count++;
		} else {
			match_col
			    = match_start + (match_len == 0 ? 1 : match_len);
		}
		free(expanded);
	}

	RESTORE_HL;
	editor_set_status_message(
	    count ? "Replaced %d occurrence%s." : "No replacements made.",
	    count, count == 1 ? "" : "s");
}
