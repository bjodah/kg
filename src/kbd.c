/* ======================== Keyboard event handling ========================= */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "bufhandle.h"
#include "cmd.h"
#include "cmdstate.h"
#include "def.h"
#include "edit.h"
#include "event.h"
#include "kbd.h"
#include "keyevent.h"
#include "keymap.h"
#include "marker.h"
#include "syntax.h"
#include "yank.h"

#define PREFIX_ARG_MAX 1000

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
	if (n <= 0) {
		return;
	}
	if (n > (int)sizeof(text)) {
		n = (int)sizeof(text);
	}
	memset(text, c, n);
	editor_insert_text_at_point(text, n);
}

/* C-u universal-argument: accumulate a numeric prefix.  Returns 1 if `c`
 * was part of the in-progress prefix (digit, another C-u, or C-g cancel)
 * and the caller should stop processing this key.  Returns 0 if `c` is a
 * real command — by then editor.prefix_pending is cleared and the count
 * is committed in editor.prefix_arg, waiting to be picked up. */
/* M-0..M-9 start a numeric argument by themselves; a plain digit only
 * continues one that C-u or a Meta digit already started, which is why
 * these are two questions. */
static int prefix_meta_digit(struct key_event c)
{
	if ((c.mods & KEY_MOD_META) && c.base >= '0' && c.base <= '9') {
		return (int)(c.base - '0');
	}
	return -1;
}

static int prefix_digit(struct key_event c)
{
	if (c.mods == 0 && c.base >= '0' && c.base <= '9') {
		return (int)(c.base - '0');
	}
	return prefix_meta_digit(c);
}

/* The two spellings of the same argument, told apart by the key that
 * last contributed to it. */
static void prefix_echo(struct key_event c, int value)
{
	if (prefix_meta_digit(c) >= 0) {
		editor_set_status_message("M-%d", value);
		return;
	}
	editor_set_status_message("C-u %d", value);
}

static int handle_pending_universal_arg(struct key_event c, int digit)
{
	if (KEY_IS(c, 'u', KEY_MOD_CTRL)) {
		editor.prefix_raw_kind = PREFIX_RAW_UNIVERSAL;
		editor.prefix_universal_count++;
		editor.prefix_arg = prefix_arg_mul_add(editor.prefix_arg, 4, 0);
		prefix_echo(c, editor.prefix_arg);
		return 1;
	}
	if (digit >= 0) {
		if (editor.prefix_raw_kind == PREFIX_RAW_MINUS) {
			editor.prefix_raw_kind = PREFIX_RAW_INTEGER;
			editor.prefix_arg = -digit;
		} else if (editor.prefix_arg < 0) {
			editor.prefix_arg = -prefix_arg_mul_add(
			    -editor.prefix_arg, 10, digit);
		} else {
			editor.prefix_raw_kind = PREFIX_RAW_INTEGER;
			editor.prefix_arg = editor.prefix_no_digits
			    ? digit
			    : prefix_arg_mul_add(editor.prefix_arg, 10, digit);
		}
		editor.prefix_no_digits = 0;
		prefix_echo(c, editor.prefix_arg);
		return 1;
	}
	if (KEY_IS(c, 'g', KEY_MOD_CTRL)) {
		editor.prefix_pending = 0;
		editor.prefix_supplied = 0;
		editor.prefix_arg = 0;
		editor.prefix_no_digits = 0;
		editor.prefix_raw_kind = PREFIX_RAW_NONE;
		editor.prefix_universal_count = 0;
		editor_set_status_message("");
		return 1;
	}

	/* This key ends the argument and is the command it applies to, so the
	 * accumulated prefix is *committed*, not discarded: the dispatcher
	 * below copies supplied/value/raw_kind into the command prefix and
	 * clears all three there.  Clearing raw_kind here instead made every
	 * Lisp command see a nil raw prefix -- P nil, p 1 -- however the user
	 * spelled the argument. */
	editor.prefix_pending = 0;
	editor.prefix_no_digits = 0;
	return 0;
}

static int start_universal_arg(struct key_event c)
{
	int meta = prefix_meta_digit(c);

	if (!KEY_IS(c, 'u', KEY_MOD_CTRL) && meta < 0
	    && !KEY_IS(c, '-', KEY_MOD_META)) {
		return 0;
	}
	editor.prefix_pending = 1;
	editor.prefix_supplied = 1;
	editor.prefix_raw_kind = KEY_IS(c, '-', KEY_MOD_META)
	    ? PREFIX_RAW_MINUS
	    : (meta < 0 ? PREFIX_RAW_UNIVERSAL : PREFIX_RAW_INTEGER);
	editor.prefix_universal_count
	    = editor.prefix_raw_kind == PREFIX_RAW_UNIVERSAL ? 1 : 0;
	/* Three starts, three effective values: bare M-- is -1, a Meta digit
	 * is that digit, and C-u is 4.  Written as one nested ternary the
	 * M-- arm was unreachable -- `meta` is -1 for M-- too, so the outer
	 * test took the C-u branch and M-- C-f moved four characters
	 * *forward*. */
	if (editor.prefix_raw_kind == PREFIX_RAW_MINUS) {
		editor.prefix_arg = -1;
	} else {
		editor.prefix_arg = meta < 0 ? 4 : meta;
	}
	editor.prefix_no_digits
	    = editor.prefix_raw_kind == PREFIX_RAW_UNIVERSAL;
	if (meta < 0) {
		editor_set_status_message("C-u");
	} else {
		prefix_echo(c, editor.prefix_arg);
	}
	return 1;
}

static int handle_universal_arg(struct key_event c)
{
	int digit = prefix_digit(c);

	if (!editor.prefix_pending) {
		return start_universal_arg(c);
	}

	return handle_pending_universal_arg(c, digit);
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
	struct key_event answer;
	int yes;

	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	vsnprintf(prompt, sizeof(prompt), fmt, ap);
	va_end(ap);
	editor_set_status_message("%s", prompt);
	/* A confirmation reads one key with the echo area committed to the
	 * question, which is exactly the window kg_event_drain_safe() must
	 * defer through -- delivering into it would let a callback repaint
	 * the status line out from under the prompt still waiting for its
	 * answer. */
	kg_event_prompt_enter();
	editor_refresh_screen();
	answer = editor_read_key(fd);
	yes = KEY_IS(answer, 'y', 0) || KEY_IS(answer, 'Y', 0);
	kg_event_prompt_leave();
	return yes;
}

/* One chunk of a `C-u N C-k` kill.  The whole span the loop below walks
 * is one command's forward kill, so only its first chunk consults the
 * coalescing class -- calling kill_ring_kill_forward() (yank.h) on
 * every chunk would re-read cmd_last_kill_class() each time and only
 * ever keep the last chunk, since *this* command's own earlier chunks
 * do not change what that reads until the next keystroke.  Every later
 * chunk just grows what the first one started. */
static void kill_lines_chunk(const char *text, size_t len, int *started)
{
	if (*started) {
		kill_ring_append(text, len);
		return;
	}
	if (cmd_last_kill_class() == KILL_COALESCE_KILL) {
		kill_ring_append(text, len);
	} else {
		kill_ring_set(text, len);
	}
	*started = 1;
}

/* C-u N C-k: kill N logical lines (each = content-to-EOL + newline),
 * matching Emacs.  editor_kill_line() is a half-step primitive, so this
 * counts *newlines* removed (numrows dropped) rather than iterations.  A
 * stalled kill ring tells us we hit EOF and should stop. */
void key_kill_lines(int n)
{
	int start_row = editor_current_filerow_or_eof();
	int start_col = editor_current_filecol();
	int start_pos;
	int end_pos;
	int r, c;
	int newlines_left;
	int started = 0;

	if (start_row >= bcur()->numrows || n <= 0) {
		return;
	}

	start_pos
	    = (int)buffer_row_col_to_position(bcur(), start_row, start_col);
	r = start_row;
	c = start_col;
	newlines_left = n;

	while (newlines_left > 0 && r < bcur()->numrows) {
		erow *row = &bcur()->row[r];
		if (c < row->size) {
			kill_lines_chunk(
			    row->chars + c, (size_t)(row->size - c), &started);
			c = row->size;
		} else {
			if (r + 1 < bcur()->numrows) {
				kill_lines_chunk("\n", 1, &started);
				r++;
				c = 0;
				newlines_left--;
			} else {
				break;
			}
		}
	}

	end_pos = (int)buffer_row_col_to_position(bcur(), r, c);
	if (end_pos > start_pos) {
		struct kg_edit e = kg_edit_user(
		    bcur(), (size_t)start_pos, (size_t)end_pos, "", 0);
		(void)kg_buffer_replace(&e, NULL);
		editor_cursor_goto(start_row, start_col);
		cmd_set_kill_class(KILL_COALESCE_KILL);
	}
}

/* C-u N C-y: batch N yanks under one undo record.  The N copies are one
 * string, so the insertion is one edit and the record is its own.  The
 * caller has already established that the kill ring holds something. */
void key_yank_repeated(int n)
{
	size_t start, total_len;
	char *combined;

	combined = kill_ring_entry_repeated(0, n, &total_len);
	if (!combined) {
		editor_set_status_message("Yank too large");
		return;
	}
	editor_push_mark();
	start = buffer_row_col_to_position(
	    bcur(), editor_current_filerow_or_eof(), editor_current_filecol());
	/* total_len <= KG_YANK_BATCH_MAX, well under INT_MAX. */
	editor_insert_text_at_point(combined, (int)total_len);
	kill_ring_note_yank(start, total_len, n);
	free(combined);
	editor_set_status_message("Yanked");
}

/* Shift translation: the command a shift-selecting key runs is the plain
 * motion, and the shift is what extends the region across it.  A table
 * rather than a switch on purpose: at -Os gcc lowers the switch to a
 * synthesized constant array and then -fanalyzer mis-reads its own index
 * arithmetic, reporting a buffer over-read of "CSWTCH.NN" that has no
 * counterpart in this source.  That false positive is what the
 * __attribute__((optimize("O0"))) on editor_process_keypress used to
 * suppress, back when this mapping was inlined there. */
static const struct {
	struct key_event key;
	const char *command;
} shift_motions[] = {
	{ { KEY_BASE_LEFT, KEY_MOD_SHIFT }, "backward-char" },
	{ { KEY_BASE_RIGHT, KEY_MOD_SHIFT }, "forward-char" },
	{ { KEY_BASE_UP, KEY_MOD_SHIFT }, "previous-line" },
	{ { KEY_BASE_DOWN, KEY_MOD_SHIFT }, "next-line" },
	{ { KEY_BASE_HOME, KEY_MOD_SHIFT }, "move-beginning-of-line" },
	{ { KEY_BASE_END, KEY_MOD_SHIFT }, "move-end-of-line" },
};

/* The command `c` extends the region with, or NULL when `c` is not a
 * shift-selecting key. */
static const char *shift_select_command(struct key_event c)
{
	size_t i;

	for (i = 0; i < sizeof(shift_motions) / sizeof(shift_motions[0]); i++) {
		if (key_event_equal(shift_motions[i].key, c)) {
			return shift_motions[i].command;
		}
	}
	return nullptr;
}

/* The self-insert fallback.
 *
 * It stays outside cmd_invoke() because it batches: a repeated printable
 * character becomes one insertion and one undo record rather than N of
 * each, which is measurable on a paste.  What it must not skip is the
 * policy: cmd_fast_path_begin() applies the same read-only verdict the
 * descriptor would and publishes the same identity, so self-insert is a
 * command that ran as far as read-only buffers, last_command and the
 * goal column are concerned. */
static void key_self_insert(struct key_event c, int n, int fd)
{
	command_id outer;
	char seq[4];
	int seqlen;
	int base;

	/* Any modifier disqualifies self-insert: Ctrl-x is not the letter
	 * x, and an unbound Ctrl or Meta combination that reaches here
	 * (nothing else claimed it) is silently ignored rather than typed
	 * as its bare base character. */
	if (c.mods != 0) {
		return;
	}
	/* TAB's base is the named key, not the byte self-insert writes;
	 * every other accepted base already is the byte to insert. */
	base = c.base == KEY_BASE_TAB ? TAB : (int)c.base;
	if (base != TAB && !ascii_is_print(base)
	    && (base < 0x80 || base > 0xFF)) {
		return;
	}
	if (!cmd_fast_path_begin("self-insert-command", &outer)) {
		return;
	}
	if (base >= 0x80) {
		/* A byte above ASCII is the lead of a multi-byte character
		 * the terminal sends one byte at a time.  Pull in the
		 * continuation bytes and insert the whole glyph as one
		 * unit; a malformed sequence (or a stray continuation
		 * byte) is dropped rather than half inserted, the same
		 * policy the minibuffer uses. */
		seqlen = editor_read_utf8_seq(fd, base, seq);
		while (seqlen > 0 && n-- > 0) {
			editor_self_insert_glyph(seq, seqlen);
		}
	} else if (n > 1 && key_can_batch_literal_insert(base)
	    && !bcur()->overwrite_mode) {
		editor_insert_repeated_literal(base, n);
	} else {
		while (n-- > 0) {
			editor_self_insert_char(base);
		}
	}
	cmd_fast_path_end(outer);
}

/* ---- The global map ----
 *
 * One row per built-in binding that has moved out of the switch below.
 * The rest of the switch is what plan 01 phase 4 still has to migrate,
 * family by family; a key the map does not answer falls through to it,
 * so the two halves coexist while that happens.
 *
 * Sequences are canonical key spellings (see key_format()), and the
 * command names are resolved at dispatch, not here. */
static const struct {
	const char *sequence;
	const char *command;
} global_map_keys[] = {
	{ "C-f", "forward-char" },
	{ "<right>", "forward-char" },
	{ "C-b", "backward-char" },
	{ "<left>", "backward-char" },
	{ "C-n", "next-line" },
	{ "<down>", "next-line" },
	{ "C-p", "previous-line" },
	{ "<up>", "previous-line" },
	{ "C-a", "move-beginning-of-line" },
	{ "<home>", "move-beginning-of-line" },
	{ "C-e", "move-end-of-line" },
	{ "<end>", "move-end-of-line" },
	{ "M-f", "forward-word" },
	{ "C-<right>", "forward-word" },
	{ "M-b", "backward-word" },
	{ "C-<left>", "backward-word" },
	{ "M-}", "forward-paragraph" },
	{ "C-<down>", "forward-paragraph" },
	{ "M-{", "backward-paragraph" },
	{ "C-<up>", "backward-paragraph" },
	{ "M-e", "forward-sentence" },
	{ "M-a", "backward-sentence" },
	{ "M-m", "back-to-indentation" },
	{ "M-<", "beginning-of-buffer" },
	{ "C-<home>", "beginning-of-buffer" },
	{ "M->", "end-of-buffer" },
	{ "C-<end>", "end-of-buffer" },
	{ "C-v", "scroll-up-command" },
	{ "<next>", "scroll-up-command" },
	{ "M-v", "scroll-down-command" },
	{ "<prior>", "scroll-down-command" },
	{ "C-l", "recenter-top-bottom" },
	{ "M-r", "move-to-window-line-top-bottom" },
	/* M-g is a prefix, the way Emacs' M-g map is: both spellings below
	 * reach goto-line, which is what M-g alone used to be bound to
	 * directly.  Neither entry needs a bare "M-g" prefix declaration --
	 * a leaf longer than one key already makes a shorter probe report
	 * KEYMAP_PREFIX (see keymap.c's map_probe()) -- so M-g itself is
	 * never bound to anything. */
	{ "M-g g", "goto-line" },
	{ "M-g M-g", "goto-line" },
	{ "M-g n", "next-error" },
	{ "M-g p", "previous-error" },
	{ "C-SPC", "set-mark-command" },
	{ "M-h", "mark-paragraph" },
	{ "M-@", "mark-word" },
	{ "M-w", "kill-ring-save" },
	{ "C-<insert>", "kill-ring-save" },
	{ "RET", "newline" },
	{ "C-j", "newline-or-eval-print-last-sexp" },
	{ "C-o", "open-line" },
	{ "<insert>", "overwrite-mode" },
	{ "C-d", "delete-char" },
	{ "C-k", "kill-line" },
	{ "C-w", "kill-region" },
	{ "S-<delete>", "kill-region" },
	{ "C-y", "yank" },
	{ "S-<insert>", "yank" },
	{ "M-y", "yank-pop" },
	{ "C-_", "undo" },
	{ "DEL", "delete-backward-char" },
	{ "<delete>", "delete-forward-char" },
	{ "C-q", "quoted-insert" },
	{ "C-s", "isearch-forward" },
	{ "C-r", "isearch-backward" },
	{ "C-M-s", "isearch-forward-regexp" },
	{ "C-M-r", "isearch-backward-regexp" },
	{ "C-t", "transpose-chars" },
	{ "M-t", "transpose-words" },
	{ "M-%", "query-replace" },
	{ "M-d", "kill-word" },
	{ "M-DEL", "backward-kill-word" },
	{ "M-q", "fill-paragraph" },
	{ "M-;", "comment-dwim" },
	{ "M-^", "join-line" },
	{ "M-u", "upcase-word" },
	{ "M-l", "downcase-word" },
	{ "M-c", "capitalize-word" },
	{ "M-\\", "delete-horizontal-space" },
	{ "M-SPC", "just-one-space" },
	{ "M-z", "zap-to-char" },
	{ "C-h", "help" },
	{ "C-z", "suspend-editor" },
	{ "M-:", "eval-expression" },
	{ "M-x", "execute-extended-command" },
	{ "M-!", "shell-command" },
	{ "M-|", "shell-command-on-region" },
	{ "<f3>", "kmacro-start-macro" },
	{ "<f4>", "kmacro-end-or-call-macro" },

	/* ESC is a prefix, not a key that reads another key itself.  The
	 * decoder still merges ESC with a key that follows it inside 100 ms
	 * into one Meta key, so these two sequences are what a *deliberate*
	 * ESC then key produces; the Meta spellings are bound above. */
	{ "ESC %", "query-replace" },
	{ "ESC M-%", "query-replace" },
	{ "ESC @", "mark-word" },
	{ "ESC M-@", "mark-word" },

	{ "C-x C-c", "save-buffers-kill-terminal" },
	{ "C-x C-s", "save-buffer" },
	{ "C-x s", "save-some-buffers" },
	{ "C-x C-f", "find-file" },
	{ "C-x C-r", "find-file-read-only" },
	{ "C-x b", "switch-to-buffer" },
	{ "C-x k", "kill-buffer" },
	{ "C-x C-b", "list-buffers" },
	{ "C-x d", "dired" },
	{ "C-x 2", "split-window-below" },
	{ "C-x 3", "split-window-right" },
	{ "C-x o", "other-window" },
	{ "C-x 0", "delete-window" },
	{ "C-x 1", "delete-other-windows" },
	{ "C-x C-x", "exchange-point-and-mark" },
	{ "C-x C-w", "write-file" },
	{ "C-x i", "insert-file" },
	{ "C-x C-q", "read-only-mode" },
	{ "C-x (", "kmacro-start-macro" },
	{ "C-x )", "kmacro-end-macro" },
	{ "C-x e", "kmacro-end-and-call-macro" },
	{ "C-x C-e", "eval-last-sexp" },
	{ "C-x SPC", "rectangle-mark-mode" },
	{ "C-x #", "server-edit" },

	{ "C-x r k", "kill-rectangle" },
	{ "C-x r C-k", "kill-rectangle" },
	{ "C-x r d", "delete-rectangle" },
	{ "C-x r c", "clear-rectangle" },
	{ "C-x r y", "yank-rectangle" },
	{ "C-x r C-y", "yank-rectangle" },
	{ "C-x r t", "string-rectangle" },
	{ "C-x r SPC", "point-to-register" },
	{ "C-x r j", "jump-to-register" },
	{ "C-x r s", "copy-to-register" },
	{ "C-x r i", "insert-register" },
};

/* Mode maps, and the predicate each is live under.  These are the
 * existing checks -- a syntax pointer, a buffer name, a read-only flag --
 * adapted into layers, not a mode registry: when there is a real one it
 * owns this table and the predicates go with it.
 *
 * They are MAJOR maps, which is one layer, so their predicates have to
 * stay mutually exclusive the way they already were: dired and the
 * buffer list are told apart by the syntax pointer, and the git modes by
 * their own. */
enum mode_map {
	MODE_MAP_DIRED,
	MODE_MAP_BUFFER_LIST,
	MODE_MAP_SPECIAL,
	MODE_MAP_COMPILATION,
	MODE_MAP_GIT_COMMIT,
	MODE_MAP_GIT_REBASE,
	MODE_MAP_COUNT,
};

static const struct {
	enum mode_map map;
	const char *sequence;
	const char *command;
} mode_map_keys[] = {
	{ MODE_MAP_DIRED, "RET", "dired-find-file" },
	{ MODE_MAP_DIRED, "^", "dired-up-directory" },
	{ MODE_MAP_DIRED, "g", "dired-revert" },
	{ MODE_MAP_DIRED, "m", "dired-mark" },
	{ MODE_MAP_DIRED, "d", "dired-flag-file-deletion" },
	{ MODE_MAP_DIRED, "u", "dired-unmark" },
	{ MODE_MAP_DIRED, "x", "dired-do-flagged-delete" },
	{ MODE_MAP_DIRED, "n", "next-line" },
	{ MODE_MAP_DIRED, "p", "previous-line" },
	{ MODE_MAP_DIRED, "q", "quit-window" },
	{ MODE_MAP_BUFFER_LIST, "RET", "ibuffer-visit-buffer" },
	{ MODE_MAP_BUFFER_LIST, "q", "quit-window" },
	{ MODE_MAP_SPECIAL, "q", "quit-window" },
	{ MODE_MAP_COMPILATION, "C-c C-k", "kill-compilation" },
	{ MODE_MAP_GIT_COMMIT, "C-c C-c", "server-edit" },
	{ MODE_MAP_GIT_COMMIT, "C-c C-k", "git-commit-abort" },
	{ MODE_MAP_GIT_REBASE, "C-c C-c", "server-edit" },
	{ MODE_MAP_GIT_REBASE, "C-c C-k", "git-rebase-abort" },
	{ MODE_MAP_GIT_REBASE, "C-c C-p", "git-rebase-pick" },
	{ MODE_MAP_GIT_REBASE, "C-c C-r", "git-rebase-reword" },
	{ MODE_MAP_GIT_REBASE, "C-c C-e", "git-rebase-edit" },
	{ MODE_MAP_GIT_REBASE, "C-c C-s", "git-rebase-squash" },
	{ MODE_MAP_GIT_REBASE, "C-c C-f", "git-rebase-fixup" },
	{ MODE_MAP_GIT_REBASE, "C-c C-d", "git-rebase-drop" },
	{ MODE_MAP_GIT_REBASE, "M-p", "git-rebase-move-line-up" },
	{ MODE_MAP_GIT_REBASE, "M-n", "git-rebase-move-line-down" },
};

static struct keymap *global_map;
static struct keymap *mode_maps[MODE_MAP_COUNT];

/* Whether each mode map is live, asked once per keystroke.  A read-only
 * buffer that is neither dired nor the buffer list still answers q,
 * which is what the special-buffer branch in dispatch used to do. */
static void key_update_mode_maps(void)
{
	const char *name = bcur()->filename;
	int listing = syntax_is_dired();
	int special = name && is_special_buffer(name) && buf_count > 1;

	keymap_set_active(mode_maps[MODE_MAP_DIRED], listing);
	keymap_set_active(
	    mode_maps[MODE_MAP_BUFFER_LIST], bcur()->readonly && !listing);
	keymap_set_active(mode_maps[MODE_MAP_SPECIAL], special && !listing);
	keymap_set_active(mode_maps[MODE_MAP_COMPILATION],
	    name && strcmp(name, "*compilation*") == 0);
	keymap_set_active(
	    mode_maps[MODE_MAP_GIT_COMMIT], syntax_is_git_commit());
	keymap_set_active(
	    mode_maps[MODE_MAP_GIT_REBASE], syntax_is_git_rebase());
}

/* The sequence in progress, and the numeric argument its first key
 * carried.  One buffer at any depth, in place of editor.cx_prefix,
 * editor.cc_prefix and editor.rect_prefix -- three booleans that could
 * only describe three fixed shapes, and that left the argument sitting
 * in an ambient global for the follow-up key to find. */
static struct {
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	int len;
	struct command_prefix prefix;
} pending;

void key_reset_pending_sequence(void) { pending.len = 0; }

/* The sequence so far, in canonical spelling: "C-x r".  The same
 * spelling the describe commands report, because it is the same
 * formatter. */
static void pending_format(char *out, size_t size)
{
	(void)keymap_format_sequence(pending.keys, pending.len, out, size);
}

/* Installed once, on the first keystroke: the editor has no init hook
 * every entry point runs, and the map is the same for every session.
 * A binding that does not install leaves its key to the switch below,
 * which is the behaviour it had before it was listed; test_keymap.c
 * checks that none of them does. */
void key_install_builtin_maps(void)
{
	size_t i;

	global_map = keymap_create("global", KEYMAP_LAYER_GLOBAL);
	for (i = 0; i < sizeof(global_map_keys) / sizeof(*global_map_keys);
	    i++) {
		(void)keymap_bind(global_map, global_map_keys[i].sequence,
		    global_map_keys[i].command);
	}
	mode_maps[MODE_MAP_DIRED] = keymap_create("dired", KEYMAP_LAYER_MAJOR);
	mode_maps[MODE_MAP_BUFFER_LIST]
	    = keymap_create("buffer-list", KEYMAP_LAYER_MAJOR);
	mode_maps[MODE_MAP_SPECIAL]
	    = keymap_create("special", KEYMAP_LAYER_MAJOR);
	mode_maps[MODE_MAP_COMPILATION]
	    = keymap_create("compilation", KEYMAP_LAYER_MAJOR);
	mode_maps[MODE_MAP_GIT_COMMIT]
	    = keymap_create("git-commit", KEYMAP_LAYER_MAJOR);
	mode_maps[MODE_MAP_GIT_REBASE]
	    = keymap_create("git-rebase", KEYMAP_LAYER_MAJOR);
	/* C-c waits for its second key even when nothing is bound under
	 * it, which is what makes it the user's prefix rather than an
	 * undefined key. */
	(void)keymap_bind_prefix(global_map, "C-c");
	for (i = 0; i < sizeof(mode_map_keys) / sizeof(*mode_map_keys); i++) {
		(void)keymap_bind(mode_maps[mode_map_keys[i].map],
		    mode_map_keys[i].sequence, mode_map_keys[i].command);
	}
}

/* Run `c` from the keymaps, one key of a sequence at a time.
 *
 * KEY_DISPATCH_IN_SEQUENCE is a keystroke that was part of a longer
 * sequence -- a prefix, or the key that finished one.  Those return
 * without the per-keystroke teardown, which is what the three prefix
 * helpers did by returning early, and is why a shift-selected region
 * survives a C-x sequence that may be about to use it. */
enum key_dispatch {
	KEY_DISPATCH_UNHANDLED,
	KEY_DISPATCH_DONE,
	KEY_DISPATCH_IN_SEQUENCE,
};

static enum key_dispatch key_dispatch_map(struct key_event event, int fd)
{
	struct key_event quit = { 'g', KEY_MOD_CTRL };
	struct keymap_match match;
	char text[KEYMAP_SEQUENCE_FORMAT_MAX];
	int started = pending.len;

	if (!global_map) {
		key_install_builtin_maps();
	}
	key_update_mode_maps();
	/* C-g gets out of a sequence at any depth, which is why no map may
	 * bind it: a keymap that is wrong must still be escapable. */
	if (started > 0 && key_event_equal(event, quit)) {
		pending.len = 0;
		editor_set_status_message("");
		return KEY_DISPATCH_IN_SEQUENCE;
	}
	if (started == 0) {
		pending.prefix = editor.current_prefix;
	}
	pending.keys[pending.len++] = event;
	keymap_lookup(pending.keys, pending.len, &match);
	pending_format(text, sizeof(text));
	if (match.result == KEYMAP_PREFIX) {
		editor_set_status_message("%s-", text);
		return KEY_DISPATCH_IN_SEQUENCE;
	}
	pending.len = 0;
	if (match.result == KEYMAP_COMMAND) {
		struct command_context ctx
		    = { fd, pending.prefix, CMD_ORIGIN_KEY };

		(void)cmd_invoke(match.command, &ctx);
		return started ? KEY_DISPATCH_IN_SEQUENCE : KEY_DISPATCH_DONE;
	}
	if (match.result == KEYMAP_UNRESOLVED) {
		editor_set_status_message(
		    "%s runs %s, which is not defined", text, match.command);
		return KEY_DISPATCH_IN_SEQUENCE;
	}
	if (match.result == KEYMAP_AMBIGUOUS) {
		editor_set_status_message("%s is bound in two ways", text);
		return KEY_DISPATCH_IN_SEQUENCE;
	}
	/* Nothing has it.  One key falls through to the switch below; a
	 * longer sequence is undefined, and says which sequence. */
	if (started == 0) {
		return KEY_DISPATCH_UNHANDLED;
	}
	editor_set_status_message("%s is undefined", text);
	return KEY_DISPATCH_IN_SEQUENCE;
}

/* Whether what just ran keeps the goal column. */
static int cmd_this_command_keeps_goal_column(void)
{
	const struct named_cmd *cmd
	    = cmd_descriptor_by_id(cmd_state()->this_command);

	return cmd && (cmd->flags & CMD_KEEPS_GOAL_COLUMN);
}

/* Per-keystroke bookkeeping that runs after the command has had its say:
 * region teardown and goal-column invalidation.  `buffer_before` and
 * `generation_before` are the values sampled before dispatch, and
 * `was_shift_select` whether a shift-selected region was live then. */
static void key_finish_keypress(struct kg_buffer_handle buffer_before,
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
	 * during their dispatch.  A shift-translated keystroke keeps the
	 * region alive; a C-x prefix keystroke also keeps it (the follow-up
	 * may consume the region). */
	if (was_shift_select && !cmd_state()->shift_translated) {
		bcur()->shift_select = 0;
		kg_mark_clear(bcur());
		bcur()->rect_mode = 0;
		editor_snap_cx_to_row();
	}

	/* Goal column is only valid between consecutive vertical motions,
	 * and which commands those are is the command's own property --
	 * whichever key, macro or M-x invocation reached it. */
	if (!cmd_this_command_keeps_goal_column()) {
		wcur()->desired_visual_col = -1;
	}
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
void editor_process_keypress(int fd)
{
	struct timeval tv;
	struct key_event c = editor_read_key_idle(fd);
	struct kg_buffer_handle buffer_before = buf_handle(buf_current);
	uint64_t generation_before = bcur()->content_generation;
	int was_shift_select = bcur()->shift_select;
	long elapsed;
	long seconds;
	enum key_dispatch dispatched;
	const char *shift_cmd;
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

	/* A sequence in progress takes the whole keystroke: no numeric
	 * argument, no mode intercepts, no new command state.  The
	 * argument the sequence started with is waiting in `pending`. */
	if (pending.len > 0) {
		(void)key_dispatch_map(c, fd);
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
	prefix.raw_kind = editor.prefix_raw_kind;
	prefix.universal_count = editor.prefix_universal_count;
	editor.current_prefix = prefix;

	if (prefix.supplied) {
		editor.prefix_supplied = 0;
		editor.prefix_arg = 0;
		editor.prefix_pending = 0;
		editor.prefix_raw_kind = PREFIX_RAW_NONE;
		editor.prefix_universal_count = 0;
		editor_set_status_message("");
		n = prefix.value;
	} else {
		n = 1;
	}

	/* End the previous keystroke's command: whatever ran during it
	 * becomes last_command, which is how a command recognises its own
	 * repeat.  The key after a prefix, and the keys of a numeric
	 * argument, return above this point, so a two-key sequence counts as
	 * the one keystroke it is. */
	cmd_state_begin_keystroke();
	cmd_state_set_key(c);

	/* Keys the maps answer.  What they do not answer falls through to
	 * the switch, which is down to the input-layer fast paths. */
	dispatched = key_dispatch_map(c, fd);
	if (dispatched != KEY_DISPATCH_UNHANDLED) {
		if (dispatched == KEY_DISPATCH_DONE) {
			key_finish_keypress(
			    buffer_before, generation_before, was_shift_select);
		}
		return;
	}

	/* Regular key processing */
	if (KEY_IS(c, 'g', KEY_MOD_CTRL)) { /* Keyboard quit / cancel */
		bcur()->mark_highlight = 0;
		bcur()->rect_mode = 0;
		editor_snap_cx_to_row();
		cmd_clear_transient();
		editor_set_status_message("");
	} else if ((shift_cmd = shift_select_command(c)) != nullptr) {
		/* Drop the mark at the current position the first time the user
		 * starts a shift-selected region, so subsequent shift+motion
		 * extends it.  If a region is already on-screen we just extend.
		 */
		if (!bcur()->mark_highlight) {
			editor_set_mark_silent();
			bcur()->shift_select = 1;
		}
		/* The command is the plain motion; what makes this keystroke
		 * different is recorded as dispatch state, which is what the
		 * teardown below reads. */
		cmd_state_set_shift_translated();
		(void)cmd_execute_named(shift_cmd, fd);
	} else {
		/* Printable ASCII, TAB and the lead byte of a multi-byte
		 * character insert themselves, N times when a numeric
		 * argument preceded them; every other control or
		 * non-printable key that no map claimed is ignored. */
		key_self_insert(c, n, fd);
	}

	key_finish_keypress(buffer_before, generation_before, was_shift_select);
}
