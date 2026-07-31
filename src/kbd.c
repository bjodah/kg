/* ======================== Keyboard event handling ========================= */

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "cmd.h"
#include "cmdstate.h"
#include "compile.h"
#include "def.h"
#include "kbd.h"
#include "keybind.h"
#include "lisp.h"
#include "syntax.h"

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

/* Vertical motions, which are the keys that may keep the goal column.
 * The list mirrors every key that eventually reaches
 * editor_move_cursor(ARROW_UP/ARROW_DOWN). */
static const int vertical_motion_keys[] = {
	ARROW_UP,
	ARROW_DOWN,
	SHIFT_ARROW_UP,
	SHIFT_ARROW_DOWN,
	PAGE_UP,
	PAGE_DOWN,
	CTRL_N,
	CTRL_P,
	CTRL_V,
	ALT_V,
};

#define PREFIX_ARG_MAX 1000

/* Whether `c` is one of the `n` keycodes in `keys`.  The key lists in
 * this file are all answered through here rather than spelled out as a
 * run of `c != ...` tests: a list is cheaper to read, cheaper to extend,
 * and does not cost a branch per entry. */
static int key_in_list(const int *keys, size_t n, int c)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (keys[i] == c) {
			return 1;
		}
	}
	return 0;
}

#define KEY_IN(list, c) key_in_list((list), sizeof(list) / sizeof(*(list)), (c))

int key_would_edit_readonly_buffer(int c)
{
	if (c >= 32 && c < 127) {
		return 1;
	}
	/* The lead byte of a typed multi-byte character self-inserts too;
	 * its continuation bytes then arrive as keys of their own and are
	 * refused by the same test. */
	if (c >= 0x80 && c <= 0xFF) {
		return 1;
	}
	return KEY_IN(readonly_blocked_keys, c);
}

static int bottom_buffer_screen_row(void)
{
	int bottom = wcur()->h > 0 ? wcur()->h - 1 : 0;
	int last = bcur()->numrows - wcur()->rowoff - 1;

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
	uint64_t generation_before;
	int i;

	memset(text, c, n);

	start_row = editor_current_filerow_or_eof();
	start_col = editor_current_filecol();
	generation_before = bcur()->content_generation;
	editor_insert_text_raw(text, n);
	if (bcur()->content_generation != generation_before) {
		for (i = 0; i < n; i++) {
			int col
			    = start_col > INT_MAX - i ? INT_MAX : start_col + i;
			undo_push(bcur(), UNDO_INSERT_CHAR, start_row, col, c,
			    NULL, 0);
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
	if (bcur()->dirty && !is_special_buffer(bcur()->filename)) {
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
		int ok = ndirty == 1
		    ? editor_confirm_yn(
			  fd, "Modified buffer, really quit? (y/n) ")
		    : editor_confirm_yn(fd,
			  "%d modified buffers, really quit? (y/n) ", ndirty);
		if (!ok) {
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

/* Ask `fmt` (printf-style, as for the status line) in the echo area and
 * read one key.  Returns 1 only for a literal yes; anything else, C-g
 * included, is a no.  The screen is refreshed first because the question
 * has to be visible before the key that answers it is read.  Every y/n
 * question in the editor goes through here, so they all agree on what a
 * yes is and on when the question is on screen. */
int editor_confirm_yn(int fd, const char *fmt, ...)
{
	char prompt[sizeof(editor.statusmsg)];
	va_list ap;
	int answer;

	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	vsnprintf(prompt, sizeof(prompt), fmt, ap);
	va_end(ap);
	editor_set_status_message("%s", prompt);
	editor_refresh_screen();
	answer = editor_read_key(fd);
	return answer == 'y' || answer == 'Y';
}

/* Abort a git commit or rebase (C-c C-k): quit without saving and with
 * a non-zero exit status so git discards the message / todo list. */
static void editor_git_abort(int fd, const char *prompt)
{
	if (!editor_confirm_yn(fd, prompt)) {
		editor_set_status_message("");
		return;
	}
	kg_exit_status = 1;
	running = 0;
}

/* Action rewritten by a built-in C-c C-<letter> key in git-rebase-todo
 * buffers, mirroring the letters of Emacs' git-rebase-mode. */
static const char *rebase_action_for_key(int c)
{
	switch (c) {
	case CTRL_P:
		return "pick";
	case CTRL_R:
		return "reword";
	case CTRL_E:
		return "edit";
	case CTRL_S:
		return "squash";
	case CTRL_F:
		return "fixup";
	case CTRL_D:
		return "drop";
	default:
		return NULL;
	}
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

/* Handle the key after a C-c prefix: built-in commit/rebase-mode keys
 * first, then user bindings installed with (global-set-key ...).
 * C-c C-c is never user-bindable (see keybind_parse), and in commit
 * and rebase buffers the other built-ins shadow any user binding. */
static void handle_cc_prefix_key(int c, int fd)
{
	const char *rebase_action
	    = syntax_is_git_rebase() ? rebase_action_for_key(c) : NULL;

	if ((syntax_is_git_commit() || syntax_is_git_rebase()) && c == CTRL_C) {
		editor_server_done(fd);
	} else if (syntax_is_git_commit() && c == CTRL_K) {
		editor_git_abort(fd, "Abort commit? (y/n) ");
	} else if (syntax_is_git_rebase() && c == CTRL_K) {
		editor_git_abort(fd, "Abort rebase? (y/n) ");
	} else if (rebase_action) {
		editor_rebase_set_action(rebase_action);
	} else if (bcur()->filename
	    && strcmp(bcur()->filename, "*compilation*") == 0 && c == CTRL_K) {
		editor_kill_compilation(fd);
	} else {
		handle_user_binding(c, fd);
	}
}

/* Handle the key after a C-x r prefix (rectangle operations).  Every op
 * here mutates the buffer, so a read-only buffer rejects them outright;
 * only C-g (cancel) still has any business reaching the switch. */
static void handle_rect_prefix_key(int c, int fd)
{
	if (bcur()->readonly && c != CTRL_G) {
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
}

/* Handle the key after a C-x prefix.
 *
 * The prefix argument typed before the C-x is still in
 * editor.current_prefix: the C-x keystroke set it and returned, and this
 * key is dispatched from the top of editor_process_keypress() before
 * anything reassigns it.  handle_cc_prefix_key() and
 * handle_rect_prefix_key() reach their commands the same way, so C-c and
 * C-x r bindings see the prefix too -- keep that early return ahead of
 * the assignment.  Presence and value are separate questions: C-u 0 is a
 * supplied zero, so never use `value > 0` to ask whether an argument was
 * given. */
static void handle_cx_prefix_key(int c, int fd)
{
	struct command_prefix prefix = editor.current_prefix;

	switch (c) {
	case CTRL_C: /* C-x C-c: Quit */
		if (editor_confirm_quit(fd)) {
			running = 0;
		}
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
	case 'd': /* C-x d: Dired */
		(void)cmd_execute_named("dired", fd);
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
	case ')': /* C-x ): Stop keyboard macro (trim C-x + ')' from buffer) */
		macro_stop(2);
		break;
	case 'e': /* C-x e: Execute keyboard macro; C-u N repeats */
		macro_replay(fd, prefix.supplied ? prefix.value : 1);
		break;
	case CTRL_E: /* C-x C-e: eval-last-sexp; with prefix, insert */
		if (kg_lisp_active()) {
			cmd_eval_last_sexp(prefix.supplied);
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
}

/* Bare keys in a directory listing.  User Lisp bindings are C-c-only by
 * design, so dired's Emacs keys are built in, like the commit and rebase
 * C-c keys; each one runs the command M-x would. */
static const struct {
	int key;
	const char *cmd;
} dired_keys[] = {
	{ ENTER, "dired-find-file" },
	{ '^', "dired-up-directory" },
	{ 'g', "dired-revert" },
	{ 'm', "dired-mark" },
	{ 'd', "dired-flag-file-deletion" },
	{ 'u', "dired-unmark" },
	{ 'x', "dired-do-flagged-delete" },
};

/* Handle `c` as a dired key.  Returns 1 when it was one, so the caller
 * stops before the read-only buffer's other bare keys. */
static int handle_dired_key(int c, int fd)
{
	size_t i;

	if (!syntax_is_dired()) {
		return 0;
	}
	if (c == 'n' || c == 'p') {
		editor_move_cursor(c == 'n' ? ARROW_DOWN : ARROW_UP);
		return 1;
	}
	for (i = 0; i < sizeof(dired_keys) / sizeof(dired_keys[0]); i++) {
		if (dired_keys[i].key == c) {
			(void)cmd_execute_named(dired_keys[i].cmd, fd);
			return 1;
		}
	}
	return 0;
}

/* C-u N C-k: kill N logical lines (each = content-to-EOL + newline),
 * matching Emacs.  editor_kill_line() is a half-step primitive, so this
 * counts *newlines* removed (numrows dropped) rather than iterations.  A
 * stalled kill ring tells us we hit EOF and should stop. */
static void key_kill_lines(int n)
{
	int start_row = editor_current_filerow_or_eof();
	int start_col = editor_current_filecol();
	int prev_kill_len = killring.len;
	int newlines_left = n;
	int killed_len;

	suppress_undo = 1;
	while (newlines_left > 0) {
		int before_numrows = bcur()->numrows;
		int before_ring_len = killring.len;

		editor_kill_line();
		if (killring.len == before_ring_len) {
			break;
		}
		if (bcur()->numrows < before_numrows) {
			newlines_left--;
		}
	}
	suppress_undo = 0;
	killed_len = killring.len - prev_kill_len;
	if (killed_len > 0) {
		undo_push(bcur(), UNDO_KILL_TEXT, start_row, start_col, 0,
		    killring.text + prev_kill_len, killed_len);
	}
}

/* C-u N C-y: batch N yanks under one undo record.  UNDO_YANK_TEXT
 * reverses by deleting len chars forward, so the record only needs the
 * full N-copy byte count.  The caller has already established that the
 * kill ring holds something. */
static void key_yank_repeated(int n)
{
	int start_row = editor_current_filerow_or_eof();
	int start_col = editor_current_filecol();
	int total_len;
	char *combined;
	int i;

	editor_push_mark();
	if (killring.len > INT_MAX / n) {
		editor_set_status_message("Yank too large");
		return;
	}
	total_len = n * killring.len;
	if (total_len > YANK_BATCH_MAX) {
		editor_set_status_message("Yank too large");
		return;
	}
	combined = malloc(total_len);
	if (!combined) {
		editor_set_status_message("Out of memory");
		return;
	}
	for (i = 0; i < n; i++) {
		memcpy(
		    combined + i * killring.len, killring.text, killring.len);
	}
	undo_push(
	    bcur(), UNDO_YANK_TEXT, start_row, start_col, 0, NULL, total_len);
	editor_insert_text_raw(combined, total_len);
	free(combined);
	editor_set_status_message("Yanked");
}

/* The plain motion a shift-selecting key extends the region with.  A
 * table rather than a switch on purpose: at -Os gcc lowers the switch to
 * a synthesized constant array and then -fanalyzer mis-reads its own
 * index arithmetic, reporting a buffer over-read of "CSWTCH.NN" that has
 * no counterpart in this source.  That false positive is what the
 * __attribute__((optimize("O0"))) on editor_process_keypress used to
 * suppress, back when this mapping was inlined there. */
static const struct {
	int key;
	int motion;
} shift_motions[] = {
	{ SHIFT_ARROW_LEFT, ARROW_LEFT },
	{ SHIFT_ARROW_RIGHT, ARROW_RIGHT },
	{ SHIFT_ARROW_UP, ARROW_UP },
	{ SHIFT_ARROW_DOWN, ARROW_DOWN },
	{ SHIFT_HOME, HOME_KEY },
	{ SHIFT_END, END_KEY },
};

/* The motion `c` extends the region with, or 0 when `c` is not a
 * shift-selecting key -- which is also how the teardown below asks
 * whether this keystroke was one of them. */
static int shift_select_motion(int c)
{
	size_t i;

	for (i = 0; i < sizeof(shift_motions) / sizeof(shift_motions[0]); i++) {
		if (shift_motions[i].key == c) {
			return shift_motions[i].motion;
		}
	}
	return 0;
}

/* Per-keystroke bookkeeping that runs after the command has had its say:
 * region teardown and goal-column invalidation.  `buffer_before` and
 * `generation_before` are the values sampled before dispatch, and
 * `was_shift_select` whether a shift-selected region was live then. */
static void key_finish_keypress(int c, struct kg_buffer_handle buffer_before,
    uint64_t generation_before, int was_shift_select)
{
	/* Any command that modified the buffer (insertion, deletion, undo,
	 * etc.) deactivates the visual mark region — matching Emacs'
	 * transient-mark-mode convention.  The identity guard avoids
	 * stomping the highlight that was just restored from a buffer slot
	 * when the user switched buffers (C-x b, C-x C-f).  It has to be the
	 * buffer's identity and not its filename pointer: a killed buffer's
	 * slot can be handed to another file whose name was allocated at the
	 * same address. */
	if (buf_handle_slot(buffer_before) == buf_current
	    && bcur()->content_generation != generation_before) {
		bcur()->mark_highlight = 0;
		bcur()->rect_mode = 0;
		editor_snap_cx_to_row();
	}

	/* Tear down a shift-selected region after the command has had its
	 * say.  Done last so C-w / M-w / C-x C-x can still see the mark
	 * during their dispatch.  Extender keys keep the region alive; a
	 * C-x prefix keystroke also keeps it (the follow-up may consume
	 * the region). */
	if (was_shift_select && !editor.cx_prefix && !shift_select_motion(c)) {
		bcur()->shift_select = 0;
		bcur()->mark_set = 0;
		bcur()->mark_highlight = 0;
		bcur()->rect_mode = 0;
		editor_snap_cx_to_row();
	}

	/* Goal column is only valid between consecutive vertical motions —
	 * any other key invalidates it. */
	if (!KEY_IN(vertical_motion_keys, c)) {
		wcur()->desired_visual_col = -1;
	}
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
void editor_process_keypress(int fd)
{
	struct timeval tv;
	int c = editor_read_key_idle(fd);
	struct kg_buffer_handle buffer_before = buf_handle(buf_current);
	uint64_t generation_before = bcur()->content_generation;
	int was_shift_select = bcur()->shift_select;
	long elapsed;
	long seconds;
	int n;

	/* Paste mode detection: characters arriving less than 30ms apart are
	 * a paste rather than typing, and auto-indent and autocompletion are
	 * suppressed while it lasts.  The gap has to be measured across the
	 * whole timeval: comparing tv_usec only when tv_sec matches misreads
	 * every gap that straddles a second boundary, which during a paste is
	 * one line in every second, and leaves the flag stuck on for slow
	 * keystrokes that happen to share a second with a fast one. */
	gettimeofday(&tv, NULL);
	seconds = (long)(tv.tv_sec - editor.last_char_time.tv_sec);
	if (seconds > 1) {
		editor.paste_mode = 0;
	} else {
		elapsed = seconds * 1000000L
		    + (long)(tv.tv_usec - editor.last_char_time.tv_usec);
		editor.paste_mode = elapsed < 30000;
	}
	editor.last_char_time = tv;

	/* The three prefixes take the whole keystroke: C-x r (rectangle ops),
	 * C-c (mode and user bindings), C-x. */
	if (editor.rect_prefix) {
		editor.rect_prefix = 0;
		handle_rect_prefix_key(c, fd);
		return;
	}

	if (editor.cc_prefix) {
		editor.cc_prefix = 0;
		handle_cc_prefix_key(c, fd);
		return;
	}

	if (editor.cx_prefix) {
		editor.cx_prefix = 0;
		handle_cx_prefix_key(c, fd);
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
	if (c == 'q' && is_special_buffer(bcur()->filename) && buf_count > 1) {
		buf_kill(fd);
		return;
	}

	/* Read-only mode: Enter opens the item at point; editing is blocked. */
	if (bcur()->readonly) {
		/* Dired's bare keys come first: dired and the buffer list are
		 * mutually exclusive by syntax pointer, so the ENTER below is
		 * ibuffer's only when this declines the key. */
		if (handle_dired_key(c, fd)) {
			return;
		}
		if (c == ENTER) {
			buf_ibuffer_select();
			return;
		}
		if (key_would_edit_readonly_buffer(c)) {
			editor_set_status_message("Buffer is read-only");
			return;
		}
	}

	/* End the previous keystroke's command: whatever ran during it
	 * becomes last_command, which is how a command recognises its own
	 * repeat.  The key after a prefix, and the keys of a numeric
	 * argument, return above this point, so a two-key sequence counts as
	 * the one keystroke it is. */
	cmd_state_begin_keystroke();

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
		if (kg_lisp_active() && bcur()->syntax
		    && (strcmp(bcur()->syntax->name, "Lisp Interaction") == 0
			|| strcmp(bcur()->syntax->name, "Lisp") == 0)) {
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
		bcur()->mark_highlight = 0;
		bcur()->rect_mode = 0;
		editor_snap_cx_to_row();
		cmd_clear_transient();
		editor_set_status_message("");
		break;
	case CTRL_K: /* Kill line */
		if (n > 1) {
			key_kill_lines(n);
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
		wcur()->cy = bottom_buffer_screen_row();
		{
			int times = wcur()->h;
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
		editor_set_status_message("C-x-");
		return;
	case CTRL_C: /* C-c prefix: user-defined bindings */
		editor.cc_prefix = 1;
		editor_set_status_message("C-c-");
		return;
	case CTRL_Y: /* Yank (paste) */
	case SHIFT_INSERT: /* CUA paste */
		if (n > 1 && killring.text && killring.len > 0) {
			key_yank_repeated(n);
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
		if (bcur()->mark_set && bcur()->mark_highlight) {
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
		if (c == PAGE_UP && wcur()->cy != 0) {
			wcur()->cy = 0;
		} else if (c == PAGE_DOWN) {
			wcur()->cy = bottom_buffer_screen_row();
		}
		{
			int times = wcur()->h;
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
		/* Never 0: `c` is one of the six labels above. */
		int motion = shift_select_motion(c);

		/* Drop the mark at the current position the first time the user
		 * starts a shift-selected region, so subsequent shift+motion
		 * extends it.  If a region is already on-screen we just extend.
		 */
		if (!bcur()->mark_highlight) {
			editor_set_mark_silent();
			bcur()->shift_select = 1;
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
		(void)cmd_execute_named("move-to-window-line-top-bottom", fd);
		break;
	case ALT_V: /* Page up */
		if (wcur()->cy != 0) {
			wcur()->cy = 0;
		}
		{
			int times = wcur()->h;
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
	case ALT_P: /* M-p / M-n: reorder git-rebase-todo lines */
	case ALT_N:
		if (syntax_is_git_rebase()) {
			while (n--) {
				editor_rebase_move_line(c == ALT_P ? -1 : 1);
			}
		}
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
	case CTRL_L: /* Recenter: cycle center → top → bottom */
		(void)cmd_execute_named("recenter-top-bottom", fd);
		break;
	default:
		/* Filter out control characters and non-printable characters.
		 * Only allow printable ASCII (32-126) and TAB.  (ENTER is
		 * handled as its own case above and would never reach here.)
		 * Repeats N times when a C-u prefix preceded the key. */
		if (c == TAB || (c >= 32 && c < 127)) {
			if (n > 1 && key_can_batch_literal_insert(c)
			    && !bcur()->overwrite_mode) {
				editor_insert_repeated_literal(c, n);
			} else {
				while (n--) {
					editor_self_insert_char(c);
				}
			}
		} else if (c >= 0x80 && c <= 0xFF) {
			/* A byte above ASCII is the lead of a multi-byte
			 * character the terminal sends one byte at a time.
			 * Pull in the continuation bytes and insert the whole
			 * glyph as one unit; a malformed sequence (or a stray
			 * continuation byte) is dropped rather than half
			 * inserted, the same policy the minibuffer uses. */
			char seq[4];
			int seqlen = editor_read_utf8_seq(fd, c, seq);

			if (seqlen > 0) {
				while (n--) {
					editor_self_insert_glyph(seq, seqlen);
				}
			}
		}
		/* Silently ignore all other control/non-printable characters */
		break;
	}

	key_finish_keypress(
	    c, buffer_before, generation_before, was_shift_select);
}
