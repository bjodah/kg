/* ============================ Find / Replace ===============================
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"

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

static char *isearch_find_last_before(char *s, char *query, int limit, int qlen)
{
	char *best = NULL;
	char *match = s;

	while ((match = strstr(match, query)) != NULL) {
		if (match - s + qlen > limit) {
			break;
		}
		best = match;
		match++;
	}
	return best;
}

static int isearch_find_match(int start_row, int start_col, int direction,
    char *query, int qlen, int *match_row, int *match_col)
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
		char *match;

		if (col < 0) {
			col = 0;
		} else if (col > row->rsize) {
			col = row->rsize;
		}

		if (direction > 0) {
			match = strstr(row->render + col, query);
		} else {
			match = isearch_find_last_before(
			    row->render, query, col, qlen);
		}

		if (match) {
			*match_row = current;
			*match_col = match - row->render;
			return 1;
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

static void editor_query_replace_newline(int fd, char *replace, int rlen)
{
	int filerow = editor.rowoff + editor.cy;
	int count = 0, replace_all = 0;

	while (filerow < editor.numrows - 1) {
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

void editor_find(int fd, int direction)
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

	if (start_row >= 0 && start_row < editor.numrows) {
		start_col = editor_visual_col(
		    &editor.row[start_row], editor.coloff + editor.cx);
	}

	while (1) {
		int c;

		editor_set_status_message("I-search: %s", query);
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
			int match = 0;
			int current = start_row;
			int col = start_col;
			int match_row = -1;
			int match_col = -1;
			int point_col;
			int search_dir = find_next;

			if (last_match_row != -1) {
				current = last_match_row;
				col = last_match_col + (search_dir > 0 ? 1 : 0);
			}
			match = isearch_find_match(current, col, search_dir,
			    query, qlen, &match_row, &match_col);
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
					memcpy(saved_hl, row->hl, row->rsize);
					memset(row->hl + match_col, HL_MATCH,
					    qlen);
				}
				point_col
				    = match_col + (search_dir > 0 ? qlen : 0);
				editor_reveal_position_centered(
				    match_row, point_col);
			}
		}
	}
}

void editor_query_replace(int fd)
{
	char search[KILO_QUERY_LEN + 1] = { 0 };
	char replace[KILO_QUERY_LEN + 1] = { 0 };
	char *saved_hl = NULL;
	int saved_hl_line = -1;
	int slen, rlen;
	int filerow, match_col;
	int count = 0, replace_all = 0;

	if (editor_read_line(fd, "Query replace: ", search, sizeof(search)) < 0
	    || !search[0]) {
		return;
	}
	if (editor_read_line(fd, "Replace with: ", replace, sizeof(replace))
	    < 0) {
		return;
	}

	slen = strlen(search);
	rlen = strlen(replace);
	if (slen == 1 && search[0] == '\n') {
		editor_query_replace_newline(fd, replace, rlen);
		return;
	}
	filerow = editor.rowoff + editor.cy;
	match_col = editor.coloff + editor.cx;

	while (filerow < editor.numrows) {
		char *match
		    = strstr(editor.row[filerow].chars + match_col, search);
		int c;

		if (!match) {
			filerow++;
			match_col = 0;
			continue;
		}
		match_col = match - editor.row[filerow].chars;

		editor_goto_line_direct(filerow + 1, match_col + 1);

		/* Highlight the match.  Convert the chars offset to a render
		 * offset so the highlight lands correctly even when tabs
		 * precede the match on the line. */
		RESTORE_HL;
		{
			erow *row = &editor.row[filerow];
			if (row->hl) {
				int i, rcol = 0;
				for (i = 0; i < match_col; i++) {
					rcol += (row->chars[i] == '\t')
					    ? (8 - rcol % 8)
					    : 1;
				}
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
			    "Replace \"%s\" with \"%s\"? (y/n/!/q)", search,
			    replace);
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

			/* Push two undo entries so C-_ fully reverses the
			 * replacement: YANK_TEXT is popped first and deletes
			 * the inserted replacement; KILL_TEXT is popped second
			 * and reinserts the original search text. */
			undo_push(UNDO_KILL_TEXT, filerow, match_col, 0, search,
			    slen);
			undo_push(UNDO_YANK_TEXT, filerow, match_col, 0,
			    replace, rlen);

			suppress_undo = 1;
			for (i = 0; i < slen; i++) {
				editor_row_del_char(row, match_col);
			}
			for (i = 0; i < rlen; i++) {
				editor_row_insert_char(row, match_col + i,
				    (unsigned char)replace[i]);
			}
			suppress_undo = 0;

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
