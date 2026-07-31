/* test_keys.c -- the inventory of built-in key bindings.
 *
 * Plan 01 phase 0 asks for one row per current built-in binding, checked
 * rather than written in prose: sequence, current handler, proposed
 * command name, read-only policy, kind of leaf, and which layer it is
 * live in.  The rows below are that inventory, and the checks are what
 * keeps them true:
 *
 *  - the global table is a partition of the key domain: every keycode the
 *    editor can be handed (0..31, DEL, and every soft key in
 *    enum KEY_ACTION) has exactly one row, so a new enumerator that
 *    nobody classified fails here rather than being quietly unbound;
 *  - the recorded read-only verdict has to agree with kbd.c's
 *    key_would_edit_readonly_buffer(), the second read-only opinion this
 *    plan deletes in phase 4.  When that list goes, this check is the
 *    proof that its verdicts moved to the command table;
 *  - a row whose command already exists in cmdtable has to agree with it
 *    about CMD_EDITS_BUFFER, so a key and M-x cannot end up with two
 *    different verdicts for one command;
 *  - a row marked as already dispatched by name has to name a command
 *    that resolves, which is what kbd.c's cmd_execute_named() calls need
 *    at runtime and nothing else checks;
 *  - a row that names a PTY case has to name one that exists.
 *
 * The proposed names are Emacs' where Emacs has one.  They are a
 * proposal, not a binding: the commit that moves a key adds the
 * descriptor and flips `dispatched_by_name`.
 *
 * This binary links every editor translation unit except main.c, the way
 * test_cmd does, because reaching kbd.c and cmdtable means linking most
 * of the editor.  It calls no handler. */

#include "../src/cmd.h"
#include "../src/def.h"
#include "../src/keyevent.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

/* What kind of leaf a key is.  The plan classifies the switch into
 * complete commands, prefix nodes, input operations that legitimately
 * stay outside ordinary keymaps, the self-insert fallback, and the
 * emergency quit. */
enum leaf_kind {
	K_CMD, /* a complete action */
	K_PREFIX, /* the next key completes the sequence */
	K_INPUT, /* input-layer operation, outside ordinary maps */
	K_SELF, /* the self-insert fallback */
	K_QUIT, /* emergency quit */
	K_NONE, /* reaches dispatch and is deliberately ignored */
};

/* Which map the binding is live in.  Only the global map and the
 * existing special-mode predicates exist today; the layer column records
 * which predicate guards the branch. */
enum binding_layer {
	L_GLOBAL,
	L_SPECIAL, /* any *special* buffer */
	L_READONLY, /* any read-only buffer */
	L_DIRED,
	L_GIT_COMMIT,
	L_GIT_REBASE,
	L_COMPILATION,
	L_USER, /* keybind.c's C-c bindings */
	L_MINIBUF, /* consumed by a prompt, never by dispatch */
};

/* What refuses this binding in a read-only buffer. */
enum readonly_policy {
	RO_FREE, /* does not edit; allowed */
	/* refused by its own descriptor, through cmd_invoke() or through
	 * cmd_fast_path_begin(): where every editing key now is */
	RO_COMMAND,
	RO_INTERCEPTED, /* the read-only branch takes the key first */
	RO_HANDLER, /* edits; its own handler refuses */
	RO_PREFIX, /* edits; the prefix helper refuses before dispatch */
	RO_UNGUARDED, /* edits, and nothing refuses it: a hole */
	RO_UNCHECKED, /* reached past dispatch; no verdict recorded */
};

struct binding {
	int key; /* keycode, or -1 for a row that covers a range */
	const char *seq;
	const char *handler; /* what kbd.c calls today */
	const char *command; /* proposed command name */
	unsigned flags; /* intended CMD_EDITS_BUFFER verdict */
	enum leaf_kind kind;
	enum binding_layer layer;
	enum readonly_policy readonly;
	int dispatched_by_name; /* already goes through cmd_invoke() */
	const char *pty_case; /* test/pty/<name>.yaml, or NULL */
	const char *note;
};

#define EDITS CMD_EDITS_BUFFER

/* One row per keycode the global dispatch in editor_process_keypress()
 * can be handed.  Printable ASCII and the bytes above it are covered by
 * the two range rows at the end. */
static const struct binding global_bindings[] = {
	{ KEY_NULL, "C-SPC", "the global map -> cmd_invoke()",
	    "set-mark-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "C-u C-SPC pops the mark ring instead" },
	{ CTRL_A, "C-a", "the global map -> cmd_invoke()",
	    "move-beginning-of-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
	{ CTRL_B, "C-b", "the global map -> cmd_invoke()", "backward-char", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_C, "C-c", "editor.cc_prefix = 1", "kg-c-c-prefix", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, "mode keys first, then user bindings" },
	{ CTRL_D, "C-d", "the global map -> cmd_invoke()", "delete-char", EDITS,
	    K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ CTRL_E, "C-e", "the global map -> cmd_invoke()", "move-end-of-line",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_F, "C-f", "the global map -> cmd_invoke()", "forward-char", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_G, "C-g", "inline: drops the region, clears the echo area",
	    "keyboard-quit", 0, K_QUIT, L_GLOBAL, RO_FREE, 0, NULL,
	    "stays an explicit fast path: it must work when a map does not" },
	{ CTRL_H, "C-h", "the global map -> cmd_invoke()", "help", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ TAB, "TAB", "the self-insert fast path", "self-insert-command", EDITS,
	    K_SELF, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "the one non-printable key the self-insert fallback accepts" },
	{ CTRL_J, "C-j", "the global map -> cmd_invoke()",
	    "newline-or-eval-print-last-sexp", EDITS, K_CMD, L_GLOBAL,
	    RO_COMMAND, 1, NULL,
	    "newline outside Lisp and Lisp Interaction buffers" },
	{ CTRL_K, "C-k", "the global map -> cmd_invoke()", "kill-line", EDITS,
	    K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ CTRL_L, "C-l", "the global map -> cmd_invoke()",
	    "recenter-top-bottom", 0, K_CMD, L_GLOBAL, RO_FREE, 1,
	    "recenter-cycle-centre-top-bottom",
	    "cycles centre/top/bottom while it is the last command" },
	{ ENTER, "RET", "the global map -> cmd_invoke()", "newline", EDITS,
	    K_CMD, L_GLOBAL, RO_INTERCEPTED, 1, NULL,
	    "a read-only buffer runs buf_ibuffer_select() on RET first" },
	{ CTRL_N, "C-n", "the global map -> cmd_invoke()", "next-line", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_O, "C-o", "the global map -> cmd_invoke()", "open-line", EDITS,
	    K_CMD, L_GLOBAL, RO_COMMAND, 1, "read-only-refuses-editing-keys",
	    "edited a read-only buffer until it became a command" },
	{ CTRL_P, "C-p", "the global map -> cmd_invoke()", "previous-line", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_Q, "C-q", "the global map -> cmd_invoke()", "quoted-insert",
	    EDITS, K_INPUT, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "the next byte is data, not a key: stays outside the map" },
	{ CTRL_R, "C-r", "the global map -> cmd_invoke()", "isearch-backward",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_S, "C-s", "the global map -> cmd_invoke()", "isearch-forward", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_T, "C-t", "the global map -> cmd_invoke()", "transpose-chars",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ CTRL_U, "C-u", "handle_universal_arg", "universal-argument", 0,
	    K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL,
	    "collects the numeric argument; explicit zero is a value" },
	{ CTRL_V, "C-v", "the global map -> cmd_invoke()", "scroll-up-command",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_W, "C-w", "the global map -> cmd_invoke()", "kill-region", EDITS,
	    K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ CTRL_X, "C-x", "editor.cx_prefix = 1", "kg-c-x-prefix", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ CTRL_Y, "C-y", "the global map -> cmd_invoke()", "yank", EDITS, K_CMD,
	    L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ CTRL_Z, "C-z", "the global map -> cmd_invoke()", "suspend-editor", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ESC, "ESC", "reads one more key", "kg-esc-prefix", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL,
	    "only ESC % and ESC @ are defined; see prefix_bindings" },
	{ 28, "C-\\", "default: ignored", "", 0, K_NONE, L_GLOBAL, RO_FREE, 0,
	    NULL, NULL },
	{ 29, "C-]", "default: ignored", "", 0, K_NONE, L_GLOBAL, RO_FREE, 0,
	    NULL, NULL },
	{ 30, "C-^", "default: ignored", "", 0, K_NONE, L_GLOBAL, RO_FREE, 0,
	    NULL, NULL },
	{ CTRL_UNDERSCORE, "C-_", "the global map -> cmd_invoke()", "undo",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "C-/ arrives as the same code" },
	{ BACKSPACE, "DEL", "the global map -> cmd_invoke()",
	    "delete-backward-char", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    NULL },

	{ ARROW_LEFT, "<left>", "the global map -> cmd_invoke()",
	    "backward-char", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ARROW_RIGHT, "<right>", "the global map -> cmd_invoke()",
	    "forward-char", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ARROW_UP, "<up>", "the global map -> cmd_invoke()", "previous-line",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ARROW_DOWN, "<down>", "the global map -> cmd_invoke()", "next-line",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ DEL_KEY, "<delete>", "the global map -> cmd_invoke()",
	    "delete-forward-char", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "consumes an active region first" },
	{ HOME_KEY, "<home>", "the global map -> cmd_invoke()",
	    "move-beginning-of-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
	{ END_KEY, "<end>", "the global map -> cmd_invoke()",
	    "move-end-of-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ PAGE_UP, "<prior>", "the global map -> cmd_invoke()",
	    "scroll-down-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ PAGE_DOWN, "<next>", "the global map -> cmd_invoke()",
	    "scroll-up-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_ARROW_LEFT, "C-<left>", "the global map -> cmd_invoke()",
	    "backward-word", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_ARROW_RIGHT, "C-<right>", "the global map -> cmd_invoke()",
	    "forward-word", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_ARROW_UP, "C-<up>", "the global map -> cmd_invoke()",
	    "backward-paragraph", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_ARROW_DOWN, "C-<down>", "the global map -> cmd_invoke()",
	    "forward-paragraph", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_HOME, "C-<home>", "the global map -> cmd_invoke()",
	    "beginning-of-buffer", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ CTRL_END, "C-<end>", "the global map -> cmd_invoke()",
	    "end-of-buffer", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ SHIFT_ARROW_LEFT, "S-<left>", "shift_select_motion + move",
	    "backward-char", 0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ SHIFT_ARROW_RIGHT, "S-<right>", "shift_select_motion + move",
	    "forward-char", 0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ SHIFT_ARROW_UP, "S-<up>", "shift_select_motion + move",
	    "previous-line", 0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ SHIFT_ARROW_DOWN, "S-<down>", "shift_select_motion + move",
	    "next-line", 0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ SHIFT_HOME, "S-<home>", "shift_select_motion + move",
	    "move-beginning-of-line", 0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ SHIFT_END, "S-<end>", "shift_select_motion + move",
	    "move-end-of-line", 0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ INSERT_KEY, "<insert>", "the global map -> cmd_invoke()",
	    "overwrite-mode", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ SHIFT_INSERT, "S-<insert>", "the global map -> cmd_invoke()", "yank",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, "CUA paste" },
	{ SHIFT_DELETE, "S-<delete>", "the global map -> cmd_invoke()",
	    "kill-region", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "CUA cut" },
	{ CTRL_INSERT, "C-<insert>", "the global map -> cmd_invoke()",
	    "kill-ring-save", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "CUA copy" },
	{ ALT_F, "M-f", "the global map -> cmd_invoke()", "forward-word", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_B, "M-b", "the global map -> cmd_invoke()", "backward-word", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_D, "M-d", "the global map -> cmd_invoke()", "kill-word", EDITS,
	    K_CMD, L_GLOBAL, RO_COMMAND, 1, "read-only-refuses-editing-keys",
	    NULL },
	{ ALT_G, "M-g", "the global map -> cmd_invoke()", "goto-line", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_H, "M-h", "the global map -> cmd_invoke()", "mark-paragraph", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_V, "M-v", "the global map -> cmd_invoke()", "scroll-down-command",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_W, "M-w", "the global map -> cmd_invoke()", "kill-ring-save", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_Q, "M-q", "the global map -> cmd_invoke()", "fill-paragraph",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1,
	    "read-only-refuses-editing-keys", NULL },
	{ ALT_BACKSPACE, "M-DEL", "the global map -> cmd_invoke()",
	    "backward-kill-word", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "no terminal-independent key token, so no PTY case of its own" },
	{ ALT_PCT, "M-%", "the global map -> cmd_invoke()", "query-replace",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ ALT_AT, "M-@", "the global map -> cmd_invoke()", "mark-word", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_SEMICOLON, "M-;", "the global map -> cmd_invoke()",
	    "comment-dwim", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "only visible in a buffer whose mode has a comment syntax" },
	{ ALT_COLON, "M-:", "the global map -> cmd_invoke()", "eval-expression",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_X, "M-x", "the global map -> cmd_invoke()",
	    "execute-extended-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "the picker itself is a prompt; it invokes by name" },
	{ ALT_CARET, "M-^", "the global map -> cmd_invoke()", "join-line",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1,
	    "read-only-refuses-editing-keys", NULL },
	{ ALT_U, "M-u", "the global map -> cmd_invoke()", "upcase-word", EDITS,
	    K_CMD, L_GLOBAL, RO_COMMAND, 1, "read-only-refuses-editing-keys",
	    NULL },
	{ ALT_L, "M-l", "the global map -> cmd_invoke()", "downcase-word",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, "same hole as M-u" },
	{ ALT_C, "M-c", "the global map -> cmd_invoke()", "capitalize-word",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, "same hole as M-u" },
	{ ALT_BANG, "M-!", "the global map -> cmd_invoke()", "shell-command", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "C-u M-! inserts the output; cmdtable still calls it non-editing" },
	{ ALT_PIPE, "M-|", "the global map -> cmd_invoke()",
	    "shell-command-on-region", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "C-u M-| replaces the region; cmdtable calls it non-editing" },
	{ ALT_LT, "M-<", "the global map -> cmd_invoke()",
	    "beginning-of-buffer", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_GT, "M->", "the global map -> cmd_invoke()", "end-of-buffer", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_LBRACE, "M-{", "the global map -> cmd_invoke()",
	    "backward-paragraph", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_RBRACE, "M-}", "the global map -> cmd_invoke()",
	    "forward-paragraph", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_M, "M-m", "the global map -> cmd_invoke()", "back-to-indentation",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_A, "M-a", "the global map -> cmd_invoke()", "backward-sentence",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_E, "M-e", "the global map -> cmd_invoke()", "forward-sentence", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ ALT_R, "M-r", "the global map -> cmd_invoke()",
	    "move-to-window-line-top-bottom", 0, K_CMD, L_GLOBAL, RO_FREE, 1,
	    "window-line-cycle-top-middle-bottom",
	    "cycles top/middle/bottom while it is the last command" },
	{ ALT_BACKSLASH, "M-\\", "the global map -> cmd_invoke()",
	    "delete-horizontal-space", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1,
	    NULL, NULL },
	{ ALT_SPACE, "M-SPC", "the global map -> cmd_invoke()",
	    "just-one-space", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    NULL },
	{ ALT_Z, "M-z", "the global map -> cmd_invoke()", "zap-to-char", EDITS,
	    K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ ALT_P, "M-p", "editor_rebase_move_line(-1)",
	    "git-rebase-move-line-up", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER,
	    0, NULL, "does nothing outside a git-rebase-todo buffer" },
	{ ALT_N, "M-n", "editor_rebase_move_line(1)",
	    "git-rebase-move-line-down", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER,
	    0, NULL, "does nothing outside a git-rebase-todo buffer" },
	{ ALT_ENTER, "M-RET", "not reached: prompts read it themselves", "", 0,
	    K_INPUT, L_MINIBUF, RO_FREE, 0, NULL,
	    "the path prompt's \"accept what I typed\" key" },
	{ ALT_0, "M-0", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, "explicit zero is a supplied value" },
	{ ALT_1, "M-1", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ ALT_2, "M-2", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ ALT_3, "M-3", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ ALT_4, "M-4", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ ALT_5, "M-5", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ ALT_6, "M-6", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ ALT_7, "M-7", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ ALT_8, "M-8", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ ALT_9, "M-9", "handle_universal_arg", "digit-argument", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ ALT_CTRL_S, "C-M-s", "the global map -> cmd_invoke()",
	    "isearch-forward-regexp", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
	{ ALT_CTRL_R, "C-M-r", "the global map -> cmd_invoke()",
	    "isearch-backward-regexp", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
	{ KEY_F3, "<f3>", "the global map -> cmd_invoke()",
	    "kmacro-start-macro", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ KEY_F4, "<f4>", "the global map -> cmd_invoke()",
	    "kmacro-end-or-call-macro", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
};

/* The two fallback ranges the switch's default branch owns.  They are
 * ranges rather than rows so the inventory does not carry 95 identical
 * lines; the checks below walk them keycode by keycode all the same. */
static const struct {
	int lo;
	int hi;
	const struct binding row;
} fallback_ranges[] = {
	{ 32, 126,
	    { -1, "<printable>",
		"editor_self_insert_char / editor_insert_repeated_literal",
		"self-insert-command", EDITS, K_SELF, L_GLOBAL, RO_COMMAND, 1,
		NULL, "a measured fast path, through cmd_fast_path_begin()" } },
	{ 0x80, 0xFF,
	    { -1, "<utf-8 lead>",
		"editor_read_utf8_seq + editor_self_insert_glyph",
		"self-insert-command", EDITS, K_SELF, L_GLOBAL, RO_COMMAND, 1,
		NULL,
		"one glyph at a time; a malformed sequence is dropped" } },
};

/* Bindings reached after a prefix key, and the bare keys the special-mode
 * predicates claim before the global map sees them.  Sequences here are
 * not keycodes of their own, so they are checked for uniqueness and for
 * naming a resolvable command, not for covering a domain. */
static const struct binding prefix_bindings[] = {
	{ CTRL_C, "C-x C-c", "editor_confirm_quit + running = 0",
	    "save-buffers-kill-terminal", 0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL,
	    NULL },
	{ CTRL_S, "C-x C-s", "editor_save", "save-buffer", 0, K_CMD, L_GLOBAL,
	    RO_FREE, 0, NULL, NULL },
	{ 's', "C-x s", "buf_save_all", "save-some-buffers", 0, K_CMD, L_GLOBAL,
	    RO_FREE, 0, NULL, NULL },
	{ CTRL_F, "C-x C-f", "buf_open_file", "find-file", 0, K_CMD, L_GLOBAL,
	    RO_FREE, 0, NULL, NULL },
	{ CTRL_R, "C-x C-r", "buf_open_file_read_only", "find-file-read-only",
	    0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ 'b', "C-x b", "buf_select_interactive", "switch-to-buffer", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ 'k', "C-x k", "buf_kill", "kill-buffer", 0, K_CMD, L_GLOBAL, RO_FREE,
	    0, NULL, NULL },
	{ CTRL_B, "C-x C-b", "buf_open_list", "list-buffers", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ 'd', "C-x d", "cmd_execute_named(\"dired\")", "dired", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ '2', "C-x 2", "win_split_horizontal", "split-window-below", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ '3', "C-x 3", "win_split_vertical", "split-window-right", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ 'o', "C-x o", "win_cycle_next", "other-window", 0, K_CMD, L_GLOBAL,
	    RO_FREE, 0, NULL, NULL },
	{ '0', "C-x 0", "win_delete_current", "delete-window", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ '1', "C-x 1", "win_delete_others", "delete-other-windows", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ CTRL_X, "C-x C-x", "editor_exchange_point_and_mark",
	    "exchange-point-and-mark", 0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL,
	    NULL },
	{ CTRL_W, "C-x C-w", "editor_write_file", "write-file", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ 'i', "C-x i", "editor_insert_file", "insert-file", EDITS, K_CMD,
	    L_GLOBAL, RO_UNCHECKED, 0, NULL,
	    "the prefix path never consults the key filter" },
	{ CTRL_Q, "C-x C-q", "cmd_execute_named(\"read-only-mode\")",
	    "read-only-mode", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ '(', "C-x (", "macro_start", "kmacro-start-macro", 0, K_CMD, L_GLOBAL,
	    RO_FREE, 0, NULL, NULL },
	{ ')', "C-x )", "macro_stop(2)", "kmacro-end-macro", 0, K_CMD, L_GLOBAL,
	    RO_FREE, 0, NULL, NULL },
	{ 'e', "C-x e", "macro_replay", "kmacro-end-and-call-macro", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, "C-u N e repeats N times" },
	{ CTRL_E, "C-x C-e", "cmd_eval_last_sexp", "eval-last-sexp", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 0, NULL, "with a prefix, inserts the result" },
	{ ' ', "C-x SPC", "editor_rect_mode_toggle", "rectangle-mark-mode", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ 'r', "C-x r", "editor.rect_prefix = 1", "kg-c-x-r-prefix", 0,
	    K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ CTRL_G, "C-x C-g", "editor_set_status_message(\"\")", "keyboard-quit",
	    0, K_QUIT, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ '#', "C-x #", "editor_server_done", "server-edit", 0, K_CMD, L_GLOBAL,
	    RO_FREE, 0, NULL, NULL },

	{ 'k', "C-x r k", "editor_kill_rect", "kill-rectangle", EDITS, K_CMD,
	    L_GLOBAL, RO_PREFIX, 0, NULL, "C-x r C-k is the same leaf" },
	{ 'd', "C-x r d", "editor_delete_rect", "delete-rectangle", EDITS,
	    K_CMD, L_GLOBAL, RO_PREFIX, 0, NULL, NULL },
	{ 'c', "C-x r c", "editor_clear_rect", "clear-rectangle", EDITS, K_CMD,
	    L_GLOBAL, RO_PREFIX, 0, NULL, NULL },
	{ 'y', "C-x r y", "editor_yank_rect", "yank-rectangle", EDITS, K_CMD,
	    L_GLOBAL, RO_PREFIX, 0, NULL, "C-x r C-y is the same leaf" },
	{ 't', "C-x r t", "editor_string_rect", "string-rectangle", EDITS,
	    K_CMD, L_GLOBAL, RO_PREFIX, 0, NULL, NULL },
	{ CTRL_G, "C-x r C-g", "editor_set_status_message(\"\")",
	    "keyboard-quit", 0, K_QUIT, L_GLOBAL, RO_FREE, 0, NULL, NULL },

	{ CTRL_C, "C-c C-c", "editor_server_done", "server-edit", 0, K_CMD,
	    L_GIT_COMMIT, RO_FREE, 0, NULL,
	    "git-rebase-todo buffers bind it the same way" },
	{ CTRL_K, "C-c C-k", "editor_git_abort", "git-commit-abort", 0, K_CMD,
	    L_GIT_COMMIT, RO_FREE, 0, NULL, NULL },
	{ CTRL_K, "C-c C-k [rebase]", "editor_git_abort", "git-rebase-abort", 0,
	    K_CMD, L_GIT_REBASE, RO_FREE, 0, NULL, NULL },
	{ CTRL_P, "C-c C-p", "editor_rebase_set_action(\"pick\")",
	    "git-rebase-pick", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 0, NULL,
	    NULL },
	{ CTRL_R, "C-c C-r", "editor_rebase_set_action(\"reword\")",
	    "git-rebase-reword", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 0,
	    NULL, NULL },
	{ CTRL_E, "C-c C-e", "editor_rebase_set_action(\"edit\")",
	    "git-rebase-edit", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 0, NULL,
	    NULL },
	{ CTRL_S, "C-c C-s", "editor_rebase_set_action(\"squash\")",
	    "git-rebase-squash", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 0,
	    NULL, NULL },
	{ CTRL_F, "C-c C-f", "editor_rebase_set_action(\"fixup\")",
	    "git-rebase-fixup", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 0, NULL,
	    NULL },
	{ CTRL_D, "C-c C-d", "editor_rebase_set_action(\"drop\")",
	    "git-rebase-drop", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 0, NULL,
	    NULL },
	{ CTRL_K, "C-c C-k [compilation]", "editor_kill_compilation",
	    "kill-compilation", 0, K_CMD, L_COMPILATION, RO_FREE, 0, NULL,
	    NULL },
	{ -1, "C-c <key>", "keybind_lookup + cmd_execute_named", "", 0, K_CMD,
	    L_USER, RO_UNCHECKED, 1, NULL,
	    "whatever (global-set-key ...) bound; the name resolves at "
	    "dispatch" },

	{ '%', "ESC %", "cmd_execute_named(\"query-replace\")", "query-replace",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "the ESC branch reaches the same command M-% does" },
	{ '@', "ESC @", "cmd_execute_named(\"mark-word\")", "mark-word", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },

	{ ENTER, "RET [dired]", "cmd_execute_named(\"dired-find-file\")",
	    "dired-find-file", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ '^', "^ [dired]", "cmd_execute_named(\"dired-up-directory\")",
	    "dired-up-directory", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ 'g', "g [dired]", "cmd_execute_named(\"dired-revert\")",
	    "dired-revert", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ 'm', "m [dired]", "cmd_execute_named(\"dired-mark\")", "dired-mark",
	    0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ 'd', "d [dired]", "cmd_execute_named(\"dired-flag-file-deletion\")",
	    "dired-flag-file-deletion", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL,
	    NULL },
	{ 'u', "u [dired]", "cmd_execute_named(\"dired-unmark\")",
	    "dired-unmark", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ 'x', "x [dired]", "cmd_execute_named(\"dired-do-flagged-delete\")",
	    "dired-do-flagged-delete", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL,
	    NULL },
	{ 'n', "n [dired]", "editor_move_cursor(ARROW_DOWN)", "next-line", 0,
	    K_CMD, L_DIRED, RO_FREE, 0, NULL, NULL },
	{ 'p', "p [dired]", "editor_move_cursor(ARROW_UP)", "previous-line", 0,
	    K_CMD, L_DIRED, RO_FREE, 0, NULL, NULL },

	{ ENTER, "RET [read-only]", "buf_ibuffer_select",
	    "ibuffer-visit-buffer", 0, K_CMD, L_READONLY, RO_FREE, 0, NULL,
	    "claims RET in every read-only buffer, not just the list" },
	{ 'q', "q [special]", "buf_kill", "quit-window", 0, K_CMD, L_SPECIAL,
	    RO_FREE, 0, NULL, "only while another buffer exists" },
};

static int global_count(void)
{
	return (int)(sizeof(global_bindings) / sizeof(global_bindings[0]));
}

static int prefix_count(void)
{
	return (int)(sizeof(prefix_bindings) / sizeof(prefix_bindings[0]));
}

static int range_count(void)
{
	return (int)(sizeof(fallback_ranges) / sizeof(fallback_ranges[0]));
}

/* The keycodes global dispatch can be handed: the control range, DEL, and
 * every soft key.  The printable range and the bytes above ASCII belong
 * to the fallback ranges instead. */
static int key_is_in_domain(int c)
{
	return (c >= 0 && c <= 31) || c == BACKSPACE
	    || (c >= ARROW_LEFT && c <= KEY_F4);
}

static const struct binding *global_row_for(int c)
{
	int i;

	for (i = 0; i < global_count(); i++) {
		if (global_bindings[i].key == c) {
			return &global_bindings[i];
		}
	}
	return NULL;
}

/* Every keycode in the domain is classified exactly once.  A new
 * enumerator in enum KEY_ACTION fails here until someone says what it
 * does. */
static void test_global_table_covers_the_key_domain(void)
{
	int c, i;

	for (c = 0; c <= KEY_F4; c++) {
		int seen = 0;

		if (!key_is_in_domain(c)) {
			continue;
		}
		for (i = 0; i < global_count(); i++) {
			if (global_bindings[i].key == c) {
				seen++;
			}
		}
		CHECKF(seen == 1, "keycode %d has %d rows, want 1", c, seen);
	}
	for (i = 0; i < global_count(); i++) {
		CHECKF(key_is_in_domain(global_bindings[i].key),
		    "%s: keycode %d is outside the global key domain",
		    global_bindings[i].seq, global_bindings[i].key);
	}
}

/* Every editing key is refused by the command it resolves to.
 *
 * This replaced kbd.c's readonly_blocked_keys[], which judged keycodes:
 * it said nothing about the same command reached by M-x or from Lisp,
 * and it named only the editing keys someone remembered to list.  The
 * check is now that a row which edits reaches a descriptor that says so,
 * and that nothing else claims to. */
static void test_every_editing_key_is_refused_by_its_command(void)
{
	int c, i, k;

	for (i = 0; i < global_count(); i++) {
		const struct binding *b = &global_bindings[i];
		const struct named_cmd *cmd = cmd_lookup(b->command);
		int edits = (b->flags & CMD_EDITS_BUFFER) != 0;

		if (b->readonly == RO_COMMAND) {
			CHECKF(b->dispatched_by_name && cmd
				&& (cmd->flags & CMD_EDITS_BUFFER),
			    "%s: nothing refuses it in a read-only buffer",
			    b->seq);
			continue;
		}
		CHECKF(!edits || b->readonly != RO_FREE,
		    "%s edits and nothing refuses it", b->seq);
	}
	for (k = 0; k < range_count(); k++) {
		const struct binding *row = &fallback_ranges[k].row;
		const struct named_cmd *cmd = cmd_lookup(row->command);

		CHECKF(row->readonly == RO_COMMAND && cmd
			&& (cmd->flags & CMD_EDITS_BUFFER),
		    "%s: the self-insert fallback is not refused", row->seq);
		for (c = fallback_ranges[k].lo; c <= fallback_ranges[k].hi;
		    c++) {
			CHECKF(global_row_for(c) == NULL,
			    "keycode %d is both a fallback and a row", c);
		}
	}
}

/* A row that edits says so, and a row that says so edits.  RO_FREE is
 * the only policy for a leaf without CMD_EDITS_BUFFER. */
static void test_edit_verdict_and_readonly_policy_agree(void)
{
	const struct binding *tables[2] = { global_bindings, prefix_bindings };
	int counts[2] = { global_count(), prefix_count() };
	int t, i;

	for (t = 0; t < 2; t++) {
		for (i = 0; i < counts[t]; i++) {
			const struct binding *b = &tables[t][i];
			int edits = (b->flags & CMD_EDITS_BUFFER) != 0;
			int refused = b->readonly != RO_FREE;

			if (b->readonly == RO_UNCHECKED) {
				continue;
			}
			CHECKF(edits == refused,
			    "%s: edits=%d but its read-only policy is %d",
			    b->seq, edits, (int)b->readonly);
		}
	}
}

/* The sequence column is the key's canonical spelling, and the spelling
 * of the keycode the row names -- so the inventory, the decoder adapter
 * and the parser cannot drift apart.  ESC C-s and ESC C-r are written
 * "C-M-s" and "C-M-r": the terminal sends them as two bytes, but they
 * are one key with two modifiers, which is what the map is keyed by. */
static void test_sequences_are_the_keycode_spelled_canonically(void)
{
	int i;

	for (i = 0; i < global_count(); i++) {
		const struct binding *b = &global_bindings[i];
		struct key_event parsed;
		char text[KEY_FORMAT_MAX];

		CHECKF(key_parse(b->seq, &parsed) == 0, "%s does not parse",
		    b->seq);
		CHECKF(key_event_equal(parsed, key_event_from_legacy(b->key)),
		    "%s is not how keycode %d is spelled", b->seq, b->key);
		CHECK(key_format(parsed, text, sizeof(text)) == 0);
		CHECKF(strcmp(text, b->seq) == 0,
		    "%s is spelled %s canonically", b->seq, text);
	}
}

/* One command, one verdict: a row whose command already has a descriptor
 * must record the descriptor's CMD_EDITS_BUFFER bit. */
static void test_named_commands_agree_with_the_command_table(void)
{
	const struct binding *tables[2] = { global_bindings, prefix_bindings };
	int counts[2] = { global_count(), prefix_count() };
	int t, i;

	for (t = 0; t < 2; t++) {
		for (i = 0; i < counts[t]; i++) {
			const struct binding *b = &tables[t][i];
			const struct named_cmd *cmd;

			if (!b->command[0]) {
				continue;
			}
			cmd = cmd_lookup(b->command);
			if (b->dispatched_by_name) {
				CHECKF(cmd != NULL,
				    "%s: dispatch calls %s by name, and no "
				    "such command exists",
				    b->seq, b->command);
			}
			if (!cmd) {
				continue;
			}
			CHECKF((cmd->flags & CMD_EDITS_BUFFER)
				== (b->flags & CMD_EDITS_BUFFER),
			    "%s: %s disagrees with cmdtable about "
			    "CMD_EDITS_BUFFER",
			    b->seq, b->command);
		}
	}
}

/* Proposed names are lower-case Lisp-style identifiers, and one sequence
 * appears once. */
static void test_rows_are_well_formed(void)
{
	const struct binding *tables[2] = { global_bindings, prefix_bindings };
	int counts[2] = { global_count(), prefix_count() };
	int t, i, j;

	for (t = 0; t < 2; t++) {
		for (i = 0; i < counts[t]; i++) {
			const struct binding *b = &tables[t][i];
			const char *p;

			CHECKF(
			    b->seq && b->seq[0], "row %d has no sequence", i);
			CHECKF(b->handler && b->handler[0], "%s has no handler",
			    b->seq);
			for (p = b->command; *p; p++) {
				CHECKF(*p == '-' || (*p >= 'a' && *p <= 'z')
					|| (*p >= '0' && *p <= '9'),
				    "%s: %s is not a command name", b->seq,
				    b->command);
			}
			for (j = i + 1; j < counts[t]; j++) {
				CHECKF(strcmp(b->seq, tables[t][j].seq) != 0,
				    "%s is listed twice", b->seq);
			}
		}
	}
}

/* Both places the case directory can be from here: `make check` runs the
 * unit binaries from the repository root, but running one by hand from
 * test/ should not manufacture a failure. */
static int pty_case_exists(const char *name)
{
	static const char *const dirs[] = { "test/pty", "pty" };
	size_t i;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		char path[256];
		FILE *f;

		snprintf(path, sizeof(path), "%s/%s.yaml", dirs[i], name);
		f = fopen(path, "r");
		if (f) {
			(void)fclose(f);
			return 1;
		}
	}
	return 0;
}

/* A named PTY case has to exist.  The stateful branches this plan moves
 * first -- the C-l and M-r cycles -- are the rows that name one. */
static void test_named_pty_cases_exist(void)
{
	const struct binding *tables[2] = { global_bindings, prefix_bindings };
	int counts[2] = { global_count(), prefix_count() };
	int t, i;

	for (t = 0; t < 2; t++) {
		for (i = 0; i < counts[t]; i++) {
			const struct binding *b = &tables[t][i];

			if (!b->pty_case) {
				continue;
			}
			CHECKF(pty_case_exists(b->pty_case),
			    "%s: no PTY case %s", b->seq, b->pty_case);
		}
	}
}

/* The goal column is kept by the commands the vertical-motion keys run,
 * and by nothing else.  key_finish_keypress() reads that flag instead of
 * the keycode list it used to keep, so this is what pins the two
 * together: shift-translated Up and Down reach the same commands, which
 * is why they are in the list. */
static void test_vertical_motions_keep_the_goal_column(void)
{
	static const char *const vertical[] = { "C-n", "C-p", "<up>", "<down>",
		"C-v", "M-v", "<prior>", "<next>", "S-<up>", "S-<down>" };
	int i;
	size_t k;

	for (k = 0; k < sizeof(vertical) / sizeof(*vertical); k++) {
		const struct named_cmd *cmd = NULL;

		for (i = 0; i < global_count(); i++) {
			if (strcmp(global_bindings[i].seq, vertical[k]) == 0) {
				cmd = cmd_lookup(global_bindings[i].command);
			}
		}
		CHECKF(cmd && (cmd->flags & CMD_KEEPS_GOAL_COLUMN),
		    "%s runs a command that drops the goal column",
		    vertical[k]);
	}
	for (i = 0; i < global_count(); i++) {
		const struct binding *b = &global_bindings[i];
		const struct named_cmd *cmd = cmd_lookup(b->command);
		int expected = 0;

		for (k = 0; k < sizeof(vertical) / sizeof(*vertical); k++) {
			if (strcmp(b->seq, vertical[k]) == 0) {
				expected = 1;
			}
		}
		if (!cmd || expected) {
			continue;
		}
		CHECKF(!(cmd->flags & CMD_KEEPS_GOAL_COLUMN),
		    "%s is not a vertical motion but keeps the goal column",
		    b->seq);
	}
}

int main(void)
{
	RUN(test_global_table_covers_the_key_domain);
	RUN(test_every_editing_key_is_refused_by_its_command);
	RUN(test_edit_verdict_and_readonly_policy_agree);
	RUN(test_named_commands_agree_with_the_command_table);
	RUN(test_rows_are_well_formed);
	RUN(test_named_pty_cases_exist);
	RUN(test_sequences_are_the_keycode_spelled_canonically);
	RUN(test_vertical_motions_keep_the_goal_column);
	return test_summary();
}
