/* ======================== Keyboard event handling ========================= */

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "compile.h"
#include "def.h"
#include "lisp.h"

#define YANK_BATCH_MAX (8 * 1024 * 1024)

static const int readonly_blocked_keys[] = {
	BACKSPACE,
	DEL_KEY,
	CTRL_D,
	CTRL_K,
	CTRL_W,
	CTRL_Y,
	CTRL_Q,
	CTRL_T,
	CTRL_J,
	SHIFT_DELETE,
	SHIFT_INSERT,
	CTRL_UNDERSCORE,
	ALT_BACKSLASH,
	ALT_SPACE,
	ALT_Z,
	TAB,
};

#define PREFIX_ARG_MAX 1000

static int key_would_edit_readonly_buffer(int c)
{
	size_t i;

	if (c >= 32 && c < 127) {
		return 1;
	}
	for (i = 0; i
	    < sizeof(readonly_blocked_keys) / sizeof(readonly_blocked_keys[0]);
	    i++) {
		if (readonly_blocked_keys[i] == c) {
			return 1;
		}
	}
	return 0;
}

static int bottom_buffer_screen_row(void)
{
	int bottom = editor.screenrows > 0 ? editor.screenrows - 1 : 0;
	int last = editor.numrows - editor.rowoff - 1;

	if (last < 0) {
		last = 0;
	}
	if (last > bottom) {
		last = bottom;
	}
	return last;
}

static int prefix_arg_mul_add(int value, int mul, int add)
{
	if (value > (PREFIX_ARG_MAX - add) / mul) {
		return PREFIX_ARG_MAX;
	}
	return value * mul + add;
}

static int key_can_batch_literal_insert(int c)
{
	return c == TAB || ((c >= 32 && c < 127) && !editor_find_close_char(c));
}

static void editor_insert_repeated_literal(int c, int n)
{
	char text[PREFIX_ARG_MAX];
	int start_row, start_col;
	int dirty_before;
	int i;

	memset(text, c, n);

	start_row = editor_current_filerow_or_eof();
	start_col = editor_current_filecol();
	dirty_before = editor.dirty;
	editor_insert_text_raw(text, n);
	if (editor.dirty != dirty_before) {
		for (i = 0; i < n; i++) {
			int col
			    = start_col > INT_MAX - i ? INT_MAX : start_col + i;
			undo_push(UNDO_INSERT_CHAR, start_row, col, c, NULL, 0);
		}
	}
}

/* C-u universal-argument: accumulate a numeric prefix.  Returns 1 if `c`
 * was part of the in-progress prefix (digit, another C-u, or C-g cancel)
 * and the caller should stop processing this key.  Returns 0 if `c` is a
 * real command — by then editor.prefix_pending is cleared and the count
 * is committed in editor.prefix_arg, waiting to be picked up. */
static int handle_universal_arg(int c)
{
	int meta_digit = (c >= ALT_0 && c <= ALT_9) ? c - ALT_0 : -1;

	if (!editor.prefix_pending) {
		if (c == CTRL_U) {
			editor.prefix_pending = 1;
			editor.prefix_supplied = 1;
			editor.prefix_arg = 4;
			editor.prefix_no_digits = 1;
			editor_set_status_message("C-u");
			return 1;
		}
		if (meta_digit >= 0) {
			editor.prefix_pending = 1;
			editor.prefix_supplied = 1;
			editor.prefix_arg = meta_digit;
			editor.prefix_no_digits = 0;
			editor_set_status_message("M-%d", editor.prefix_arg);
			return 1;
		}
		return 0;
	}

	if (c == CTRL_U) {
		editor.prefix_arg = prefix_arg_mul_add(editor.prefix_arg, 4, 0);
		editor_set_status_message("C-u %d", editor.prefix_arg);
		return 1;
	}
	if (c >= '0' && c <= '9') {
		int digit = c - '0';
		if (editor.prefix_no_digits) {
			editor.prefix_arg = digit;
			editor.prefix_no_digits = 0;
		} else {
			editor.prefix_arg
			    = prefix_arg_mul_add(editor.prefix_arg, 10, digit);
		}
		editor_set_status_message("C-u %d", editor.prefix_arg);
		return 1;
	}
	if (meta_digit >= 0) {
		if (editor.prefix_no_digits) {
			editor.prefix_arg = meta_digit;
			editor.prefix_no_digits = 0;
		} else {
			editor.prefix_arg = prefix_arg_mul_add(
			    editor.prefix_arg, 10, meta_digit);
		}
		editor_set_status_message("M-%d", editor.prefix_arg);
		return 1;
	}
	if (c == CTRL_G) {
		editor.prefix_pending = 0;
		editor.prefix_supplied = 0;
		editor.prefix_arg = 0;
		editor.prefix_no_digits = 0;
		editor_set_status_message("");
		return 1;
	}

	editor.prefix_pending = 0;
	editor.prefix_no_digits = 0;
	return 0;
}

/* Quit confirmation for C-x C-c: prompt when modified real-file buffers
 * exist.  Returns 1 when quitting may proceed. */
static int editor_confirm_quit(int fd)
{
	int i, ndirty = 0;

	/* Count modified real-file buffers (exclude *special* ones). */
	if (editor.dirty && !is_special_buffer(editor.filename)) {
		ndirty++;
	}
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active || i == buf_current) {
			continue;
		}
		if (!buflist[i].dirty) {
			continue;
		}
		if (is_special_buffer(buflist[i].filename)) {
			continue;
		}
		ndirty++;
	}
	if (ndirty) {
		int answer;
		editor_set_status_message(ndirty == 1
			? "Modified buffer, really quit? (y/n) "
			: "%d modified buffers, really quit? "
			  "(y/n) ",
		    ndirty);
		editor_refresh_screen();
		answer = editor_read_key(fd);
		if (answer != 'y' && answer != 'Y') {
			editor_set_status_message("");
			return 0;
		}
	}
	return 1;
}

/* Finish an EDITOR-server session (C-c C-c in git-commit buffers,
 * C-x # anywhere): save the current buffer, then quit with status 0.
 * A failed or cancelled save keeps the session running. */
static void editor_server_done(int fd)
{
	if (editor_save(fd) != 0) {
		return;
	}
	if (!editor_confirm_quit(fd)) {
		return;
	}
	running = 0;
}

/* Abort a git commit (C-c C-k): quit without saving and with a
 * non-zero exit status so git discards the commit. */
static void editor_commit_abort(int fd)
{
	int answer;

	editor_set_status_message("Abort commit? (y/n) ");
	editor_refresh_screen();
	answer = editor_read_key(fd);
	if (answer != 'y' && answer != 'Y') {
		editor_set_status_message("");
		return;
	}
	kg_exit_status = 1;
	running = 0;
}

/* Run the command bound to C-c <key>, or report an undefined key.  The
 * binding table maps the second key to a named command, so a bound key
 * runs exactly what M-x would. */
static void handle_user_binding(int c, int fd)
{
	const char *bound = keybind_lookup(c);

	if (bound) {
		(void)cmd_execute_named(bound, fd);
	} else if (c == CTRL_G) {
		editor_set_status_message("");
	} else if (c >= 32 && c < 127) {
		editor_set_status_message("C-c %c is undefined", c);
	} else if (c > 0 && c < 27) {
		editor_set_status_message("C-c C-%c is undefined", 'a' + c - 1);
	} else {
		editor_set_status_message("C-c key is undefined");
	}
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-out-of-bounds"
__attribute__((optimize("O0")))
#endif
void editor_process_keypress(int fd)
{
	struct timeval tv;
	int c = editor_read_key_idle(fd);
	char *fname_before = editor.filename;
	int dirty_before = editor.dirty;
	int was_shift_select = editor.shift_select;
	long elapsed;
	int n;

	/* Paste mode detection: if characters arrive very quickly (< 30ms
	 * apart), we're likely in a paste operation, so disable autocompletion
	 */
	gettimeofday(&tv, NULL);
	if (tv.tv_sec == editor.last_char_time.tv_sec) {
		elapsed = (tv.tv_usec - editor.last_char_time.tv_usec);
		if (elapsed < 30000) {
			editor.paste_mode = 1; /* 30ms threshold */
		}
	} else if (tv.tv_sec - editor.last_char_time.tv_sec > 0) {
		editor.paste_mode = 0;
	}
	editor.last_char_time = tv;

	/* Handle C-x r rectangle ops (second key after C-x r).  Every op
	 * here mutates the buffer, so a read-only buffer rejects them
	 * outright; only C-g (cancel) still has any business reaching
	 * the inner switch. */
	if (editor.rect_prefix) {
		editor.rect_prefix = 0;
		if (editor.readonly && c != CTRL_G) {
			editor_set_status_message("Buffer is read-only");
			return;
		}
		switch (c) {
		case 'k':
		case CTRL_K:
			editor_kill_rect();
			break;
		case 'd':
			editor_delete_rect();
			break;
		case 'c':
			editor_clear_rect();
			break;
		case 'y':
		case CTRL_Y:
			editor_yank_rect();
			break;
		case 't':
			editor_string_rect(fd);
			break;
		case CTRL_G:
			editor_set_status_message("");
			break;
		default:
			editor_set_status_message("C-x r %c is undefined", c);
			break;
		}
		return;
	}

	/* Handle C-c <key>: built-in commit-mode keys first, then user
	 * bindings installed with (global-set-key ...).  C-c C-c is never
	 * user-bindable (see keybind_parse), and in commit buffers the
	 * built-in C-c C-k shadows any user binding. */
	if (editor.cc_prefix) {
		editor.cc_prefix = 0;
		if (syntax_is_git_commit() && c == CTRL_C) {
			editor_server_done(fd);
		} else if (syntax_is_git_commit() && c == CTRL_K) {
			editor_commit_abort(fd);
		} else if (editor.filename
		    && strcmp(editor.filename, "*compilation*") == 0
		    && c == CTRL_K) {
			editor_kill_compilation(fd);
		} else {
			handle_user_binding(c, fd);
		}
		return;
	}

	/* Handle C-x prefix commands */
	if (editor.cx_prefix) {
		editor.cx_prefix = 0;
		switch (c) {
		case CTRL_C: /* C-x C-c: Quit */
			if (!editor_confirm_quit(fd)) {
				return;
			}
			running = 0;
			break;
		case CTRL_S: /* C-x C-s: Save */
			editor_save(fd);
			break;
		case 's': /* C-x s: Save all modified buffers */
			buf_save_all(fd);
			break;
		case CTRL_F: /* C-x C-f: Open file in new buffer */
			buf_open_file(fd);
			break;
		case CTRL_R: /* C-x C-r: Open file read-only */
			buf_open_file_read_only(fd);
			break;
		case 'b': /* C-x b: Interactive buffer select */
			buf_select_interactive(fd);
			break;
		case 'k': /* C-x k: Kill current buffer */
			buf_kill(fd);
			break;
		case CTRL_B: /* C-x C-b: Open buffer list */
			buf_open_list();
			break;
		case '2': /* C-x 2: Split window horizontally */
			win_split_horizontal();
			break;
		case '3': /* C-x 3: Split window vertically */
			win_split_vertical();
			break;
		case 'o': /* C-x o: Other window */
			win_cycle_next();
			break;
		case '0': /* C-x 0: Delete current window */
			win_delete_current();
			break;
		case '1': /* C-x 1: Delete other windows */
			win_delete_others();
			break;
		case CTRL_X: /* C-x C-x: Exchange point and mark */
			editor_exchange_point_and_mark();
			break;
		case CTRL_W: /* C-x C-w: Write file (save as) */
			editor_write_file(fd);
			break;
		case 'i': /* C-x i: Insert file at point */
			editor_insert_file(fd);
			break;
		case CTRL_Q: /* C-x C-q: read-only-mode */
			(void)cmd_execute_named("read-only-mode", fd);
			break;
		case '(': /* C-x (: Start keyboard macro */
			macro_start();
			break;
		case ')': /* C-x ): Stop keyboard macro (trim C-x + ')' from
			     buffer) */
			macro_stop(2);
			break;
		case 'e': /* C-x e: Execute keyboard macro; C-u N repeats */
			macro_replay(fd,
			    editor.cx_prefix_arg > 0 ? editor.cx_prefix_arg
						     : 1);
			break;
		case CTRL_E: /* C-x C-e: eval-last-sexp; with prefix, insert */
			if (kg_lisp_active()) {
				cmd_eval_last_sexp(editor.cx_prefix_arg > 0);
			} else {
				editor_set_status_message("Lisp not available");
			}
			break;
		case ' ': /* C-x SPC: rectangle-mark-mode */
			editor_rect_mode_toggle();
			break;
		case 'r': /* C-x r-: rectangle operation prefix */
			editor.rect_prefix = 1;
			editor_set_status_message("C-x r-");
			break;
		case CTRL_G: /* C-x C-g: Cancel C-x prefix */
			editor_set_status_message("");
			break;
		case '#': /* C-x #: save and exit 0 (EDITOR-server done) */
			editor_server_done(fd);
			break;
		default:
			editor_set_status_message("C-x %c is undefined", c);
			break;
		}
		return;
	}

	if (handle_universal_arg(c)) {
		return;
	}

	/* Consume the pending C-u count now, before any of the early-return
	 * filters below have a chance to drop the key.  Whichever branch ends
	 * up handling this keystroke either uses `n` or implicitly discards
	 * it; either way the next keypress starts fresh. */
	struct command_prefix prefix;
	prefix.supplied = editor.prefix_supplied;
	prefix.value = editor.prefix_arg;
	editor.current_prefix = prefix;

	if (prefix.supplied) {
		editor.prefix_supplied = 0;
		editor.prefix_arg = 0;
		editor.prefix_pending = 0;
		editor_set_status_message("");
		n = prefix.value;
	} else {
		n = 1;
	}

	/* q closes special *...* buffers, but only if another buffer exists */
	if (c == 'q' && is_special_buffer(editor.filename) && buf_count > 1) {
		buf_kill(fd);
		return;
	}

	/* Read-only mode: Enter opens the item at point; editing is blocked. */
	if (editor.readonly) {
		if (c == ENTER) {
			buf_ibuffer_select();
			return;
		}
		if (key_would_edit_readonly_buffer(c)) {
			editor_set_status_message("Buffer is read-only");
			return;
		}
	}

	/* Reset cycle states if the previous key wasn't the cycling command. */
	if (editor.last_key != CTRL_L) {
		editor.recenter_state = 0;
	}
	if (editor.last_key != ALT_R) {
		editor.window_line_state = 0;
	}
	editor.last_key = c;

	/* Regular key processing */
	switch (c) {
	case ESC:
		c = editor_read_key(fd);
		if (c == '%' || c == ALT_PCT) {
			editor_query_replace(fd);
		} else if (c == '@' || c == ALT_AT) {
			editor_mark_word(n);
		} else if (c != CTRL_G) {
			editor_set_status_message("ESC %c is undefined", c);
		}
		break;
	case KEY_NULL: /* Ctrl+Space: set mark; with C-u, pop the mark ring */
		if (prefix.supplied) {
			editor_pop_to_mark();
		} else {
			editor_set_mark();
		}
		break;
	case ENTER: /* Enter */
		while (n--) {
			editor_insert_newline();
		}
		break;
	case CTRL_J: /* Ctrl-j: eval sexp in Lisp/Scratch if Lisp active, else
			newline */
		if (kg_lisp_active() && editor.syntax
		    && (strcmp(editor.syntax->name, "Lisp Interaction") == 0
			|| strcmp(editor.syntax->name, "Lisp") == 0)) {
			cmd_eval_print_last_sexp();
		} else {
			while (n--) {
				editor_insert_newline();
			}
		}
		break;
	case CTRL_A: /* Beginning of line */
		editor_move_cursor(HOME_KEY);
		break;
	case CTRL_B: /* Backward char */
		while (n--) {
			editor_move_cursor(ARROW_LEFT);
		}
		break;
	case CTRL_D: /* Delete char forward */
		while (n--) {
			editor_del_forward_char();
		}
		break;
	case CTRL_E: /* End of line */
		editor_move_cursor(END_KEY);
		break;
	case CTRL_F: /* Forward char */
		while (n--) {
			editor_move_cursor(ARROW_RIGHT);
		}
		break;
	case CTRL_G: /* Keyboard quit / cancel */
		editor.mark_highlight = 0;
		editor.rect_mode = 0;
		editor_snap_cx_to_row();
		editor_set_status_message("");
		break;
	case CTRL_K: /* Kill line */
		if (n > 1) {
			/* Kill N logical lines (each = content-to-EOL +
			 * newline), matching Emacs' C-u N C-k.
			 * editor_kill_line() is a half- step primitive, so we
			 * count *newlines* removed (numrows dropped) rather
			 * than iterations.  A stalled kill_ring tells us we hit
			 * EOF and should stop. */
			int start_row = editor_current_filerow_or_eof();
			int start_col = editor_current_filecol();
			int prev_kill_len = killring.len;
			int newlines_left = n;
			int killed_len;
			suppress_undo = 1;
			while (newlines_left > 0) {
				int before_numrows = editor.numrows;
				int before_ring_len = killring.len;
				editor_kill_line();
				if (killring.len == before_ring_len) {
					break;
				}
				if (editor.numrows < before_numrows) {
					newlines_left--;
				}
			}
			suppress_undo = 0;
			killed_len = killring.len - prev_kill_len;
			if (killed_len > 0) {
				undo_push(UNDO_KILL_TEXT, start_row, start_col,
				    0, killring.text + prev_kill_len,
				    killed_len);
			}
		} else {
			editor_kill_line();
		}
		break;
	case CTRL_O: /* Open line */
		while (n--) {
			editor_open_line();
		}
		break;
	case CTRL_N: /* Next line */
		while (n--) {
			editor_move_cursor(ARROW_DOWN);
		}
		break;
	case CTRL_P: /* Previous line */
		while (n--) {
			editor_move_cursor(ARROW_UP);
		}
		break;
	case CTRL_S: /* Incremental search */
		editor_find(fd, 1);
		break;
	case CTRL_T: /* Transpose chars */
		while (n--) {
			editor_transpose_chars();
		}
		break;
	case CTRL_R:
		editor_find(fd, -1);
		break;
	case CTRL_Q: {
		int key = editor_read_raw_byte(fd);
		if (running) {
			editor_insert_char(key);
		}
		break;
	}
	case CTRL_V: /* Page down */
		editor.cy = bottom_buffer_screen_row();
		{
			int times = editor.screenrows;
			while (times--) {
				editor_move_cursor(ARROW_DOWN);
			}
		}
		break;
	case CTRL_W: /* Kill region (cut) */
	case SHIFT_DELETE: /* CUA cut */
		editor_kill_region();
		break;
	case CTRL_X: /* C-x prefix */
		editor.cx_prefix = 1;
		editor.cx_prefix_arg = prefix.supplied ? prefix.value : 0;
		editor_set_status_message("C-x-");
		return;
	case CTRL_C: /* C-c prefix: user-defined bindings */
		editor.cc_prefix = 1;
		editor_set_status_message("C-c-");
		return;
	case CTRL_Y: /* Yank (paste) */
	case SHIFT_INSERT: /* CUA paste */
		if (n > 1 && killring.text && killring.len > 0) {
			/* Batch N yanks under one undo: UNDO_YANK_TEXT reverses
			 * by deleting len chars forward, so the record only
			 * needs the full N-copy byte count. */
			int start_row = editor_current_filerow_or_eof();
			int start_col = editor_current_filecol();
			editor_push_mark();
			if (killring.len <= INT_MAX / n) {
				int total_len = n * killring.len;
				char *combined;
				int i;

				if (total_len > YANK_BATCH_MAX) {
					editor_set_status_message(
					    "Yank too large");
					break;
				}
				combined = malloc(total_len);
				if (!combined) {
					editor_set_status_message(
					    "Out of memory");
					break;
				}
				for (i = 0; i < n; i++) {
					memcpy(combined + i * killring.len,
					    killring.text, killring.len);
				}
				undo_push(UNDO_YANK_TEXT, start_row, start_col,
				    0, NULL, total_len);
				editor_insert_text_raw(combined, total_len);
				free(combined);
				editor_set_status_message("Yanked");
			} else {
				editor_set_status_message("Yank too large");
			}
		} else {
			editor_yank();
		}
		break;
	case CTRL_UNDERSCORE: /* Undo (C-_ or C-/) */
		while (n--) {
			editor_undo();
		}
		break;
	case CTRL_H: /* Help */
		buf_open_help();
		break;
	case CTRL_Z: /* Suspend to shell */
		editor_suspend();
		break;
	case BACKSPACE: /* Backspace */
		while (n--) {
			editor_del_char();
		}
		break;
	case DEL_KEY: /* Forward delete; consumes an active region first. */
		if (editor.mark_set && editor.mark_highlight) {
			editor_delete_region_or_char();
		} else {
			while (n--) {
				editor_del_forward_char();
			}
		}
		break;
	case INSERT_KEY:
		editor_toggle_overwrite_mode();
		break;
	case PAGE_UP:
	case PAGE_DOWN:
		if (c == PAGE_UP && editor.cy != 0) {
			editor.cy = 0;
		} else if (c == PAGE_DOWN) {
			editor.cy = bottom_buffer_screen_row();
		}
		{
			int times = editor.screenrows;
			while (times--) {
				editor_move_cursor(
				    c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
		}
		break;

	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		while (n--) {
			editor_move_cursor(c);
		}
		break;
	case HOME_KEY:
	case END_KEY:
		editor_move_cursor(c);
		break;
	case SHIFT_ARROW_LEFT:
	case SHIFT_ARROW_RIGHT:
	case SHIFT_ARROW_UP:
	case SHIFT_ARROW_DOWN:
	case SHIFT_HOME:
	case SHIFT_END: {
		int motion;
		/* Drop the mark at the current position the first time the user
		 * starts a shift-selected region, so subsequent shift+motion
		 * extends it.  If a region is already on-screen we just extend.
		 */
		if (!editor.mark_highlight) {
			editor_set_mark_silent();
			editor.shift_select = 1;
		}
		if (c == SHIFT_ARROW_LEFT) {
			motion = ARROW_LEFT;
		} else if (c == SHIFT_ARROW_RIGHT) {
			motion = ARROW_RIGHT;
		} else if (c == SHIFT_ARROW_UP) {
			motion = ARROW_UP;
		} else if (c == SHIFT_ARROW_DOWN) {
			motion = ARROW_DOWN;
		} else if (c == SHIFT_HOME) {
			motion = HOME_KEY;
		} else {
			motion = END_KEY;
		}
		while (n--) {
			editor_move_cursor(motion);
		}
		break;
	}
	case CTRL_HOME:
	case ALT_LT:
		editor_push_mark();
		editor_move_to_beginning();
		break;
	case CTRL_END:
	case ALT_GT:
		editor_push_mark();
		editor_move_to_end();
		break;
	case ALT_G: /* Goto line */
		editor_goto_line(fd);
		break;
	case ALT_H: /* Mark paragraph */
		editor_mark_paragraph();
		break;
	case ALT_AT: /* Mark word */
		editor_mark_word(n);
		break;
	case CTRL_ARROW_LEFT:
	case ALT_B:
		while (n--) {
			editor_move_word_backward();
		}
		break;
	case CTRL_ARROW_RIGHT:
	case ALT_F:
		while (n--) {
			editor_move_word_forward();
		}
		break;
	case ALT_D: /* Kill word forward */
		while (n--) {
			editor_kill_word_forward();
		}
		break;
	case ALT_BACKSPACE: /* Kill word backward */
		while (n--) {
			editor_kill_word_backward();
		}
		break;
	case CTRL_ARROW_UP:
	case ALT_LBRACE:
		while (n--) {
			editor_move_paragraph_backward();
		}
		break;
	case CTRL_ARROW_DOWN:
	case ALT_RBRACE:
		while (n--) {
			editor_move_paragraph_forward();
		}
		break;
	case ALT_M: /* M-m: back-to-indentation */
		editor_move_to_indentation();
		break;
	case ALT_BACKSLASH: /* Delete horizontal space */
		editor_delete_horizontal_space();
		break;
	case ALT_SPACE: /* Just one space */
		editor_just_one_space();
		break;
	case ALT_Z: /* Zap to char */
		editor_zap_to_char(fd, n);
		break;
	case ALT_A: /* M-a: backward sentence */
		while (n--) {
			editor_move_sentence_backward();
		}
		break;
	case ALT_E: /* M-e: forward sentence */
		while (n--) {
			editor_move_sentence_forward();
		}
		break;
	case ALT_R: /* M-r: top/middle/bottom of window cycle */
		editor_move_to_window_line();
		break;
	case ALT_V: /* Page up */
		if (editor.cy != 0) {
			editor.cy = 0;
		}
		{
			int times = editor.screenrows;
			while (times--) {
				editor_move_cursor(ARROW_UP);
			}
		}
		break;
	case ALT_W: /* Copy region */
	case CTRL_INSERT: /* CUA copy */
		editor_copy_region();
		break;
	case ALT_Q: /* Reflow paragraph */
		editor_reflow_paragraph();
		break;
	case ALT_PCT: /* Query replace */
		editor_query_replace(fd);
		break;
	case ALT_SEMICOLON: /* Toggle line comment */
		editor_comment_dwim();
		break;
	case ALT_COLON: /* Eval expression */
		(void)cmd_execute_named("eval-expression", fd);
		break;
	case ALT_X: /* Named command */
		editor_named_command(fd);
		break;
	case ALT_CTRL_S:
		editor_find_regexp(fd, 1);
		break;
	case ALT_CTRL_R:
		editor_find_regexp(fd, -1);
		break;
	case ALT_CARET: /* Join current line with previous */
		while (n--) {
			editor_join_line();
		}
		break;
	case ALT_U: /* Upcase word forward */
		while (n--) {
			editor_upcase_word();
		}
		break;
	case ALT_L: /* Downcase word forward */
		while (n--) {
			editor_downcase_word();
		}
		break;
	case ALT_C: /* Capitalize word forward */
		while (n--) {
			editor_capitalize_word();
		}
		break;
	case ALT_BANG: /* Shell command */
		editor_shell_command(fd, prefix.supplied);
		break;
	case ALT_PIPE: /* Shell command on region */
		editor_shell_command_on_region(fd, prefix.supplied);
		break;
	case KEY_F3: /* F3: Start keyboard macro */
		macro_start();
		break;
	case KEY_F4: /* F4: Stop if recording, else execute; C-u N repeats */
		if (macro_is_recording()) {
			macro_stop(1);
		} else {
			macro_replay(fd, n);
		}
		break;
	case CTRL_L: { /* Recenter: cycle center → top → bottom */
		int filerow = editor_current_filerow_or_eof();
		switch (editor.recenter_state) {
		case 0: /* center */
			editor.rowoff = filerow - editor.screenrows / 2;
			break;
		case 1: /* top */
			editor.rowoff = filerow;
			break;
		default: /* bottom */
			editor.rowoff = filerow - (editor.screenrows - 1);
			break;
		}
		if (editor.rowoff < 0) {
			editor.rowoff = 0;
		}
		editor.cy = filerow - editor.rowoff;
		editor.recenter_state = (editor.recenter_state + 1) % 3;
		probe_window_size();
		tty_write("\x1b[2J", 4);
		editor_refresh_screen();
		break;
	}
	default:
		/* Filter out control characters and non-printable characters.
		 * Only allow printable ASCII (32-126) and TAB.  (ENTER is
		 * handled as its own case above and would never reach here.)
		 * Repeats N times when a C-u prefix preceded the key. */
		if (c == TAB || (c >= 32 && c < 127)) {
			if (n > 1 && key_can_batch_literal_insert(c)
			    && !editor.overwrite_mode) {
				editor_insert_repeated_literal(c, n);
			} else {
				while (n--) {
					editor_self_insert_char(c);
				}
			}
		}
		/* Silently ignore all other control/non-printable characters */
		break;
	}

	/* Any command that modified the buffer (insertion, deletion, undo,
	 * etc.) deactivates the visual mark region — matching Emacs'
	 * transient-mark-mode convention.  The filename guard avoids
	 * stomping the highlight that was just restored from a buffer slot
	 * when the user switched buffers (C-x b, C-x C-f). */
	if (editor.filename == fname_before && editor.dirty != dirty_before) {
		editor.mark_highlight = 0;
		editor.rect_mode = 0;
		editor_snap_cx_to_row();
	}

	/* Tear down a shift-selected region after the command has had its
	 * say.  Done last so C-w / M-w / C-x C-x can still see the mark
	 * during their dispatch.  Extender keys keep the region alive; a
	 * C-x prefix keystroke also keeps it (the follow-up may consume
	 * the region). */
	if (was_shift_select && !editor.cx_prefix && c != SHIFT_ARROW_LEFT
	    && c != SHIFT_ARROW_RIGHT && c != SHIFT_ARROW_UP
	    && c != SHIFT_ARROW_DOWN && c != SHIFT_HOME && c != SHIFT_END) {
		editor.shift_select = 0;
		editor.mark_set = 0;
		editor.mark_highlight = 0;
		editor.rect_mode = 0;
		editor_snap_cx_to_row();
	}

	/* Goal column is only valid between consecutive vertical motions —
	 * any other key invalidates it.  Keep-list mirrors every key that
	 * eventually routes through editor_move_cursor(ARROW_UP/DOWN). */
	if (c != ARROW_UP && c != ARROW_DOWN && c != SHIFT_ARROW_UP
	    && c != SHIFT_ARROW_DOWN && c != PAGE_UP && c != PAGE_DOWN
	    && c != CTRL_N && c != CTRL_P && c != CTRL_V && c != ALT_V) {
		editor.desired_visual_col = -1;
	}
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
