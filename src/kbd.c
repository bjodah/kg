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
#include "def.h"
#include "edit.h"
#include "kbd.h"
#include "keyevent.h"
#include "keymap.h"
#include "syntax.h"

#define YANK_BATCH_MAX (8 * 1024 * 1024)

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
/* M-0..M-9 start a numeric argument by themselves; a plain digit only
 * continues one that C-u or a Meta digit already started, which is why
 * these are two questions. */
static int prefix_meta_digit(int c)
{
	if (c >= ALT_0 && c <= ALT_9) {
		return c - ALT_0;
	}
	return -1;
}

static int prefix_digit(int c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	return prefix_meta_digit(c);
}

/* The two spellings of the same argument, told apart by the key that
 * last contributed to it. */
static void prefix_echo(int c, int value)
{
	if (prefix_meta_digit(c) >= 0) {
		editor_set_status_message("M-%d", value);
		return;
	}
	editor_set_status_message("C-u %d", value);
}

static int handle_universal_arg(int c)
{
	int digit = prefix_digit(c);

	if (!editor.prefix_pending) {
		int meta = prefix_meta_digit(c);

		if (c != CTRL_U && meta < 0) {
			return 0;
		}
		editor.prefix_pending = 1;
		editor.prefix_supplied = 1;
		editor.prefix_arg = meta < 0 ? 4 : meta;
		editor.prefix_no_digits = meta < 0;
		if (meta < 0) {
			editor_set_status_message("C-u");
		} else {
			prefix_echo(c, editor.prefix_arg);
		}
		return 1;
	}

	if (c == CTRL_U) {
		editor.prefix_arg = prefix_arg_mul_add(editor.prefix_arg, 4, 0);
		prefix_echo(c, editor.prefix_arg);
		return 1;
	}
	if (digit >= 0) {
		editor.prefix_arg = editor.prefix_no_digits
		    ? digit
		    : prefix_arg_mul_add(editor.prefix_arg, 10, digit);
		editor.prefix_no_digits = 0;
		prefix_echo(c, editor.prefix_arg);
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

/* C-u N C-k: kill N logical lines (each = content-to-EOL + newline),
 * matching Emacs.  editor_kill_line() is a half-step primitive, so this
 * counts *newlines* removed (numrows dropped) rather than iterations.  A
 * stalled kill ring tells us we hit EOF and should stop. */
void key_kill_lines(int n)
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

/* C-u N C-y: batch N yanks under one undo record.  The N copies are one
 * string, so the insertion is one edit and the record is its own.  The
 * caller has already established that the kill ring holds something. */
void key_yank_repeated(int n)
{
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
	editor_insert_text_at_point(combined, total_len);
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
	int key;
	const char *command;
} shift_motions[] = {
	{ SHIFT_ARROW_LEFT, "backward-char" },
	{ SHIFT_ARROW_RIGHT, "forward-char" },
	{ SHIFT_ARROW_UP, "previous-line" },
	{ SHIFT_ARROW_DOWN, "next-line" },
	{ SHIFT_HOME, "move-beginning-of-line" },
	{ SHIFT_END, "move-end-of-line" },
};

/* The command `c` extends the region with, or NULL when `c` is not a
 * shift-selecting key. */
static const char *shift_select_command(int c)
{
	size_t i;

	for (i = 0; i < sizeof(shift_motions) / sizeof(shift_motions[0]); i++) {
		if (shift_motions[i].key == c) {
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
static void key_self_insert(int c, int n, int fd)
{
	command_id outer;
	char seq[4];
	int seqlen;

	if (c != TAB && !ascii_is_print(c) && (c < 0x80 || c > 0xFF)) {
		return;
	}
	if (!cmd_fast_path_begin("self-insert-command", &outer)) {
		return;
	}
	if (c >= 0x80) {
		/* A byte above ASCII is the lead of a multi-byte character
		 * the terminal sends one byte at a time.  Pull in the
		 * continuation bytes and insert the whole glyph as one
		 * unit; a malformed sequence (or a stray continuation
		 * byte) is dropped rather than half inserted, the same
		 * policy the minibuffer uses. */
		seqlen = editor_read_utf8_seq(fd, c, seq);
		while (seqlen > 0 && n-- > 0) {
			editor_self_insert_glyph(seq, seqlen);
		}
	} else if (n > 1 && key_can_batch_literal_insert(c)
	    && !bcur()->overwrite_mode) {
		editor_insert_repeated_literal(c, n);
	} else {
		while (n-- > 0) {
			editor_self_insert_char(c);
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
	{ "M-g", "goto-line" },
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
	{ "C-_", "undo" },
	{ "DEL", "delete-backward-char" },
	{ "<delete>", "delete-forward-char" },
	{ "C-q", "quoted-insert" },
	{ "C-s", "isearch-forward" },
	{ "C-r", "isearch-backward" },
	{ "C-M-s", "isearch-forward-regexp" },
	{ "C-M-r", "isearch-backward-regexp" },
	{ "C-t", "transpose-chars" },
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

/* The sequence so far, in canonical spelling: "C-x r". */
static void pending_format(char *out, size_t size)
{
	int i;
	size_t used = 0;

	out[0] = '\0';
	for (i = 0; i < pending.len; i++) {
		char text[KEY_FORMAT_MAX];

		if (key_format(pending.keys[i], text, sizeof(text)) != 0) {
			continue;
		}
		used += (size_t)snprintf(
		    out + used, size - used, "%s%s", used ? " " : "", text);
		if (used >= size) {
			return;
		}
	}
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

static enum key_dispatch key_dispatch_map(int c, int fd)
{
	struct key_event event = key_event_from_legacy(c);
	struct key_event quit = { 'g', KEY_MOD_CTRL };
	struct keymap_match match;
	char text[KEYMAP_SEQUENCE_MAX * (KEY_FORMAT_MAX + 1)];
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
		bcur()->mark_set = 0;
		bcur()->mark_highlight = 0;
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
	int c = editor_read_key_idle(fd);
	struct kg_buffer_handle buffer_before = buf_handle(buf_current);
	uint64_t generation_before = bcur()->content_generation;
	int was_shift_select = bcur()->shift_select;
	long elapsed;
	long seconds;
	enum key_dispatch dispatched;
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

	/* End the previous keystroke's command: whatever ran during it
	 * becomes last_command, which is how a command recognises its own
	 * repeat.  The key after a prefix, and the keys of a numeric
	 * argument, return above this point, so a two-key sequence counts as
	 * the one keystroke it is. */
	cmd_state_begin_keystroke();
	cmd_state_set_key(key_event_from_legacy(c));

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
	switch (c) {
	case CTRL_G: /* Keyboard quit / cancel */
		bcur()->mark_highlight = 0;
		bcur()->rect_mode = 0;
		editor_snap_cx_to_row();
		cmd_clear_transient();
		editor_set_status_message("");
		break;
	case SHIFT_ARROW_LEFT:
	case SHIFT_ARROW_RIGHT:
	case SHIFT_ARROW_UP:
	case SHIFT_ARROW_DOWN:
	case SHIFT_HOME:
	case SHIFT_END:
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
		 * teardown below reads.  Never NULL: `c` is one of the six
		 * labels above. */
		cmd_state_set_shift_translated();
		(void)cmd_execute_named(shift_select_command(c), fd);
		break;
	default:
		/* Printable ASCII, TAB and the lead byte of a multi-byte
		 * character insert themselves, N times when a numeric
		 * argument preceded them; every other control or
		 * non-printable key that no map claimed is ignored. */
		key_self_insert(c, n, fd);
		break;
	}

	key_finish_keypress(buffer_before, generation_before, was_shift_select);
}
