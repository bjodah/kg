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
	struct key_event key; /* base -1 for a row that covers a range */
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
	{ { ' ', KEY_MOD_CTRL }, "C-SPC", "the global map -> cmd_invoke()",
	    "set-mark-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "C-u C-SPC pops the mark ring instead" },
	{ { 'a', KEY_MOD_CTRL }, "C-a", "the global map -> cmd_invoke()",
	    "move-beginning-of-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
	{ { 'b', KEY_MOD_CTRL }, "C-b", "the global map -> cmd_invoke()",
	    "backward-char", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'c', KEY_MOD_CTRL }, "C-c", "keymap prefix traversal", "", 0,
	    K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL,
	    "declared a prefix so it waits even with nothing bound under "
	    "it" },
	{ { 'd', KEY_MOD_CTRL }, "C-d", "the global map -> cmd_invoke()",
	    "delete-char", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ { 'e', KEY_MOD_CTRL }, "C-e", "the global map -> cmd_invoke()",
	    "move-end-of-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'f', KEY_MOD_CTRL }, "C-f", "the global map -> cmd_invoke()",
	    "forward-char", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'g', KEY_MOD_CTRL }, "C-g",
	    "inline: drops the region, clears the echo area", "keyboard-quit",
	    0, K_QUIT, L_GLOBAL, RO_FREE, 0, NULL,
	    "stays an explicit fast path: it must work when a map does not" },
	{ { 'h', KEY_MOD_CTRL }, "C-h", "the global map -> cmd_invoke()",
	    "help", 0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { KEY_BASE_TAB, 0 }, "TAB", "the self-insert fast path",
	    "self-insert-command", EDITS, K_SELF, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "the one non-printable key the self-insert fallback accepts" },
	{ { 'j', KEY_MOD_CTRL }, "C-j", "the global map -> cmd_invoke()",
	    "newline-or-eval-print-last-sexp", EDITS, K_CMD, L_GLOBAL,
	    RO_COMMAND, 1, NULL,
	    "newline outside Lisp and Lisp Interaction buffers" },
	{ { 'k', KEY_MOD_CTRL }, "C-k", "the global map -> cmd_invoke()",
	    "kill-line", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ { 'l', KEY_MOD_CTRL }, "C-l", "the global map -> cmd_invoke()",
	    "recenter-top-bottom", 0, K_CMD, L_GLOBAL, RO_FREE, 1,
	    "recenter-cycle-centre-top-bottom",
	    "cycles centre/top/bottom while it is the last command" },
	{ { KEY_BASE_RET, 0 }, "RET", "the global map -> cmd_invoke()",
	    "newline", EDITS, K_CMD, L_GLOBAL, RO_INTERCEPTED, 1, NULL,
	    "a read-only buffer runs buf_ibuffer_select() on RET first" },
	{ { 'n', KEY_MOD_CTRL }, "C-n", "the global map -> cmd_invoke()",
	    "next-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'o', KEY_MOD_CTRL }, "C-o", "the global map -> cmd_invoke()",
	    "open-line", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1,
	    "read-only-refuses-editing-keys",
	    "edited a read-only buffer until it became a command" },
	{ { 'p', KEY_MOD_CTRL }, "C-p", "the global map -> cmd_invoke()",
	    "previous-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'q', KEY_MOD_CTRL }, "C-q", "the global map -> cmd_invoke()",
	    "quoted-insert", EDITS, K_INPUT, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "the next byte is data, not a key: stays outside the map" },
	{ { 'r', KEY_MOD_CTRL }, "C-r", "the global map -> cmd_invoke()",
	    "isearch-backward", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 's', KEY_MOD_CTRL }, "C-s", "the global map -> cmd_invoke()",
	    "isearch-forward", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 't', KEY_MOD_CTRL }, "C-t", "the global map -> cmd_invoke()",
	    "transpose-chars", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    NULL },
	{ { 'u', KEY_MOD_CTRL }, "C-u", "handle_universal_arg",
	    "universal-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL,
	    "collects the numeric argument; explicit zero is a value" },
	{ { 'v', KEY_MOD_CTRL }, "C-v", "the global map -> cmd_invoke()",
	    "scroll-up-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'w', KEY_MOD_CTRL }, "C-w", "the global map -> cmd_invoke()",
	    "kill-region", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ { 'x', KEY_MOD_CTRL }, "C-x", "keymap prefix traversal", "", 0,
	    K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL,
	    "a prefix in the global map, at any depth" },
	{ { 'y', KEY_MOD_CTRL }, "C-y", "the global map -> cmd_invoke()",
	    "yank", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ { 'z', KEY_MOD_CTRL }, "C-z", "the global map -> cmd_invoke()",
	    "suspend-editor", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_ESC, 0 }, "ESC", "keymap prefix traversal", "", 0,
	    K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL,
	    "only ESC % and ESC @ are defined; see prefix_bindings" },
	{ { '\\', KEY_MOD_CTRL }, "C-\\", "default: ignored", "", 0, K_NONE,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { ']', KEY_MOD_CTRL }, "C-]", "default: ignored", "", 0, K_NONE,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '^', KEY_MOD_CTRL }, "C-^", "default: ignored", "", 0, K_NONE,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '_', KEY_MOD_CTRL }, "C-_", "the global map -> cmd_invoke()",
	    "undo", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "C-/ arrives as the same code" },
	{ { KEY_BASE_DEL, 0 }, "DEL", "the global map -> cmd_invoke()",
	    "delete-backward-char", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    NULL },

	{ { KEY_BASE_LEFT, 0 }, "<left>", "the global map -> cmd_invoke()",
	    "backward-char", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_RIGHT, 0 }, "<right>", "the global map -> cmd_invoke()",
	    "forward-char", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_UP, 0 }, "<up>", "the global map -> cmd_invoke()",
	    "previous-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_DOWN, 0 }, "<down>", "the global map -> cmd_invoke()",
	    "next-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_DELETE, 0 }, "<delete>", "the global map -> cmd_invoke()",
	    "delete-forward-char", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "consumes an active region first" },
	{ { KEY_BASE_HOME, 0 }, "<home>", "the global map -> cmd_invoke()",
	    "move-beginning-of-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
	{ { KEY_BASE_END, 0 }, "<end>", "the global map -> cmd_invoke()",
	    "move-end-of-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_PRIOR, 0 }, "<prior>", "the global map -> cmd_invoke()",
	    "scroll-down-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_NEXT, 0 }, "<next>", "the global map -> cmd_invoke()",
	    "scroll-up-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_LEFT, KEY_MOD_CTRL }, "C-<left>",
	    "the global map -> cmd_invoke()", "backward-word", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_RIGHT, KEY_MOD_CTRL }, "C-<right>",
	    "the global map -> cmd_invoke()", "forward-word", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_UP, KEY_MOD_CTRL }, "C-<up>",
	    "the global map -> cmd_invoke()", "backward-paragraph", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_DOWN, KEY_MOD_CTRL }, "C-<down>",
	    "the global map -> cmd_invoke()", "forward-paragraph", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_HOME, KEY_MOD_CTRL }, "C-<home>",
	    "the global map -> cmd_invoke()", "beginning-of-buffer", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_END, KEY_MOD_CTRL }, "C-<end>",
	    "the global map -> cmd_invoke()", "end-of-buffer", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_LEFT, KEY_MOD_SHIFT }, "S-<left>",
	    "shift translation -> cmd_invoke()", "backward-char", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ { KEY_BASE_RIGHT, KEY_MOD_SHIFT }, "S-<right>",
	    "shift translation -> cmd_invoke()", "forward-char", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ { KEY_BASE_UP, KEY_MOD_SHIFT }, "S-<up>",
	    "shift translation -> cmd_invoke()", "previous-line", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ { KEY_BASE_DOWN, KEY_MOD_SHIFT }, "S-<down>",
	    "shift translation -> cmd_invoke()", "next-line", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ { KEY_BASE_HOME, KEY_MOD_SHIFT }, "S-<home>",
	    "shift translation -> cmd_invoke()", "move-beginning-of-line", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ { KEY_BASE_END, KEY_MOD_SHIFT }, "S-<end>",
	    "shift translation -> cmd_invoke()", "move-end-of-line", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL,
	    "shift translation: sets the mark, then the plain motion" },
	{ { KEY_BASE_INSERT, 0 }, "<insert>", "the global map -> cmd_invoke()",
	    "overwrite-mode", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_INSERT, KEY_MOD_SHIFT }, "S-<insert>",
	    "the global map -> cmd_invoke()", "yank", EDITS, K_CMD, L_GLOBAL,
	    RO_COMMAND, 1, NULL, "CUA paste" },
	{ { KEY_BASE_DELETE, KEY_MOD_SHIFT }, "S-<delete>",
	    "the global map -> cmd_invoke()", "kill-region", EDITS, K_CMD,
	    L_GLOBAL, RO_COMMAND, 1, NULL, "CUA cut" },
	{ { KEY_BASE_INSERT, KEY_MOD_CTRL }, "C-<insert>",
	    "the global map -> cmd_invoke()", "kill-ring-save", 0, K_CMD,
	    L_GLOBAL, RO_FREE, 1, NULL, "CUA copy" },
	{ { 'f', KEY_MOD_META }, "M-f", "the global map -> cmd_invoke()",
	    "forward-word", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'b', KEY_MOD_META }, "M-b", "the global map -> cmd_invoke()",
	    "backward-word", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'd', KEY_MOD_META }, "M-d", "the global map -> cmd_invoke()",
	    "kill-word", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1,
	    "read-only-refuses-editing-keys", NULL },
	{ { 'g', KEY_MOD_META }, "M-g", "the global map -> cmd_invoke()",
	    "goto-line", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'h', KEY_MOD_META }, "M-h", "the global map -> cmd_invoke()",
	    "mark-paragraph", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'v', KEY_MOD_META }, "M-v", "the global map -> cmd_invoke()",
	    "scroll-down-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'w', KEY_MOD_META }, "M-w", "the global map -> cmd_invoke()",
	    "kill-ring-save", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'q', KEY_MOD_META }, "M-q", "the global map -> cmd_invoke()",
	    "fill-paragraph", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1,
	    "read-only-refuses-editing-keys", NULL },
	{ { KEY_BASE_DEL, KEY_MOD_META }, "M-DEL",
	    "the global map -> cmd_invoke()", "backward-kill-word", EDITS,
	    K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "no terminal-independent key token, so no PTY case of its own" },
	{ { '%', KEY_MOD_META }, "M-%", "the global map -> cmd_invoke()",
	    "query-replace", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    NULL },
	{ { '@', KEY_MOD_META }, "M-@", "the global map -> cmd_invoke()",
	    "mark-word", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { ';', KEY_MOD_META }, "M-;", "the global map -> cmd_invoke()",
	    "comment-dwim", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "only visible in a buffer whose mode has a comment syntax" },
	{ { ':', KEY_MOD_META }, "M-:", "the global map -> cmd_invoke()",
	    "eval-expression", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'x', KEY_MOD_META }, "M-x", "the global map -> cmd_invoke()",
	    "execute-extended-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "the picker itself is a prompt; it invokes by name" },
	{ { '^', KEY_MOD_META }, "M-^", "the global map -> cmd_invoke()",
	    "join-line", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1,
	    "read-only-refuses-editing-keys", NULL },
	{ { 'u', KEY_MOD_META }, "M-u", "the global map -> cmd_invoke()",
	    "upcase-word", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1,
	    "read-only-refuses-editing-keys", NULL },
	{ { 'l', KEY_MOD_META }, "M-l", "the global map -> cmd_invoke()",
	    "downcase-word", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "same hole as M-u" },
	{ { 'c', KEY_MOD_META }, "M-c", "the global map -> cmd_invoke()",
	    "capitalize-word", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "same hole as M-u" },
	{ { '!', KEY_MOD_META }, "M-!", "the global map -> cmd_invoke()",
	    "shell-command", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "C-u M-! inserts the output; cmdtable still calls it non-editing" },
	{ { '|', KEY_MOD_META }, "M-|", "the global map -> cmd_invoke()",
	    "shell-command-on-region", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "C-u M-| replaces the region; cmdtable calls it non-editing" },
	{ { '<', KEY_MOD_META }, "M-<", "the global map -> cmd_invoke()",
	    "beginning-of-buffer", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { '>', KEY_MOD_META }, "M->", "the global map -> cmd_invoke()",
	    "end-of-buffer", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { '{', KEY_MOD_META }, "M-{", "the global map -> cmd_invoke()",
	    "backward-paragraph", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { '}', KEY_MOD_META }, "M-}", "the global map -> cmd_invoke()",
	    "forward-paragraph", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'm', KEY_MOD_META }, "M-m", "the global map -> cmd_invoke()",
	    "back-to-indentation", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'a', KEY_MOD_META }, "M-a", "the global map -> cmd_invoke()",
	    "backward-sentence", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'e', KEY_MOD_META }, "M-e", "the global map -> cmd_invoke()",
	    "forward-sentence", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'r', KEY_MOD_META }, "M-r", "the global map -> cmd_invoke()",
	    "move-to-window-line-top-bottom", 0, K_CMD, L_GLOBAL, RO_FREE, 1,
	    "window-line-cycle-top-middle-bottom",
	    "cycles top/middle/bottom while it is the last command" },
	{ { '\\', KEY_MOD_META }, "M-\\", "the global map -> cmd_invoke()",
	    "delete-horizontal-space", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1,
	    NULL, NULL },
	{ { ' ', KEY_MOD_META }, "M-SPC", "the global map -> cmd_invoke()",
	    "just-one-space", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    NULL },
	{ { 'z', KEY_MOD_META }, "M-z", "the global map -> cmd_invoke()",
	    "zap-to-char", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL, NULL },
	{ { 'p', KEY_MOD_META }, "M-p", "the git-rebase map -> cmd_invoke()",
	    "git-rebase-move-line-up", EDITS, K_CMD, L_GIT_REBASE, RO_COMMAND,
	    1, NULL, "the map is live only in a git-rebase-todo buffer" },
	{ { 'n', KEY_MOD_META }, "M-n", "the git-rebase map -> cmd_invoke()",
	    "git-rebase-move-line-down", EDITS, K_CMD, L_GIT_REBASE, RO_COMMAND,
	    1, NULL, "the map is live only in a git-rebase-todo buffer" },
	{ { KEY_BASE_RET, KEY_MOD_META }, "M-RET",
	    "not reached: prompts read it themselves", "", 0, K_INPUT,
	    L_MINIBUF, RO_FREE, 0, NULL,
	    "the path prompt's \"accept what I typed\" key" },
	{ { '0', KEY_MOD_META }, "M-0", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL,
	    "explicit zero is a supplied value" },
	{ { '1', KEY_MOD_META }, "M-1", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '2', KEY_MOD_META }, "M-2", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '3', KEY_MOD_META }, "M-3", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '4', KEY_MOD_META }, "M-4", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '5', KEY_MOD_META }, "M-5", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '6', KEY_MOD_META }, "M-6", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '7', KEY_MOD_META }, "M-7", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '8', KEY_MOD_META }, "M-8", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { '9', KEY_MOD_META }, "M-9", "handle_universal_arg",
	    "digit-argument", 0, K_PREFIX, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { 's', KEY_MOD_CTRL | KEY_MOD_META }, "C-M-s",
	    "the global map -> cmd_invoke()", "isearch-forward-regexp", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'r', KEY_MOD_CTRL | KEY_MOD_META }, "C-M-r",
	    "the global map -> cmd_invoke()", "isearch-backward-regexp", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_F3, 0 }, "<f3>", "the global map -> cmd_invoke()",
	    "kmacro-start-macro", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { KEY_BASE_F4, 0 }, "<f4>", "the global map -> cmd_invoke()",
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
	    { { -1, 0 }, "<printable>",
		"editor_self_insert_char / editor_insert_repeated_literal",
		"self-insert-command", EDITS, K_SELF, L_GLOBAL, RO_COMMAND, 1,
		NULL, "a measured fast path, through cmd_fast_path_begin()" } },
	{ 0x80, 0xFF,
	    { { -1, 0 }, "<utf-8 lead>",
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
	{ { 'c', KEY_MOD_CTRL }, "C-x C-c", "the global map -> cmd_invoke()",
	    "save-buffers-kill-terminal", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
	{ { 's', KEY_MOD_CTRL }, "C-x C-s", "the global map -> cmd_invoke()",
	    "save-buffer", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 's', 0 }, "C-x s", "the global map -> cmd_invoke()",
	    "save-some-buffers", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'f', KEY_MOD_CTRL }, "C-x C-f", "the global map -> cmd_invoke()",
	    "find-file", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'r', KEY_MOD_CTRL }, "C-x C-r", "the global map -> cmd_invoke()",
	    "find-file-read-only", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'b', 0 }, "C-x b", "the global map -> cmd_invoke()",
	    "switch-to-buffer", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'k', 0 }, "C-x k", "the global map -> cmd_invoke()", "kill-buffer",
	    0, K_CMD, L_GLOBAL, RO_FREE, 0, NULL, NULL },
	{ { 'b', KEY_MOD_CTRL }, "C-x C-b", "the global map -> cmd_invoke()",
	    "list-buffers", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'd', 0 }, "C-x d", "the global map -> cmd_invoke()", "dired", 0,
	    K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { '2', 0 }, "C-x 2", "the global map -> cmd_invoke()",
	    "split-window-below", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { '3', 0 }, "C-x 3", "the global map -> cmd_invoke()",
	    "split-window-right", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'o', 0 }, "C-x o", "the global map -> cmd_invoke()", "other-window",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { '0', 0 }, "C-x 0", "the global map -> cmd_invoke()",
	    "delete-window", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { '1', 0 }, "C-x 1", "the global map -> cmd_invoke()",
	    "delete-other-windows", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
	{ { 'x', KEY_MOD_CTRL }, "C-x C-x", "the global map -> cmd_invoke()",
	    "exchange-point-and-mark", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    NULL },
	{ { 'w', KEY_MOD_CTRL }, "C-x C-w", "the global map -> cmd_invoke()",
	    "write-file", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'i', 0 }, "C-x i", "the global map -> cmd_invoke()", "insert-file",
	    EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "the prefix path never consults the key filter" },
	{ { 'q', KEY_MOD_CTRL }, "C-x C-q", "the global map -> cmd_invoke()",
	    "read-only-mode", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { '(', 0 }, "C-x (", "the global map -> cmd_invoke()",
	    "kmacro-start-macro", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { ')', 0 }, "C-x )", "the global map -> cmd_invoke()",
	    "kmacro-end-macro", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'e', 0 }, "C-x e", "the global map -> cmd_invoke()",
	    "kmacro-end-and-call-macro", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "C-u N e repeats N times" },
	{ { 'e', KEY_MOD_CTRL }, "C-x C-e", "the global map -> cmd_invoke()",
	    "eval-last-sexp", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL,
	    "with a prefix, inserts the result" },
	{ { ' ', 0 }, "C-x SPC", "the global map -> cmd_invoke()",
	    "rectangle-mark-mode", 0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },
	{ { 'r', 0 }, "C-x r", "keymap prefix traversal", "", 0, K_PREFIX,
	    L_GLOBAL, RO_FREE, 0, NULL, "a prefix because leaves hang off it" },
	{ { 'g', KEY_MOD_CTRL }, "C-x C-g",
	    "prefix traversal cancels the sequence", "keyboard-quit", 0, K_QUIT,
	    L_GLOBAL, RO_FREE, 0, NULL,
	    "C-g gets out at any depth, which is why no map may bind it" },
	{ { '#', 0 }, "C-x #", "the global map -> cmd_invoke()", "server-edit",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },

	{ { 'k', 0 }, "C-x r k", "the global map -> cmd_invoke()",
	    "kill-rectangle", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "C-x r C-k is the same leaf" },
	{ { 'd', 0 }, "C-x r d", "the global map -> cmd_invoke()",
	    "delete-rectangle", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    NULL },
	{ { 'c', 0 }, "C-x r c", "the global map -> cmd_invoke()",
	    "clear-rectangle", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    NULL },
	{ { 'y', 0 }, "C-x r y", "the global map -> cmd_invoke()",
	    "yank-rectangle", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "C-x r C-y is the same leaf" },
	{ { 't', 0 }, "C-x r t", "the global map -> cmd_invoke()",
	    "string-rectangle", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    NULL },
	{ { 'g', KEY_MOD_CTRL }, "C-x r C-g",
	    "prefix traversal cancels the sequence", "keyboard-quit", 0, K_QUIT,
	    L_GLOBAL, RO_FREE, 0, NULL, NULL },

	{ { 'c', KEY_MOD_CTRL }, "C-c C-c", "a mode map -> cmd_invoke()",
	    "server-edit", 0, K_CMD, L_GIT_COMMIT, RO_FREE, 1, NULL,
	    "git-rebase-todo buffers bind it the same way" },
	{ { 'k', KEY_MOD_CTRL }, "C-c C-k", "a mode map -> cmd_invoke()",
	    "git-commit-abort", 0, K_CMD, L_GIT_COMMIT, RO_FREE, 1, NULL,
	    NULL },
	{ { 'k', KEY_MOD_CTRL }, "C-c C-k [rebase]",
	    "a mode map -> cmd_invoke()", "git-rebase-abort", 0, K_CMD,
	    L_GIT_REBASE, RO_FREE, 1, NULL, NULL },
	{ { 'p', KEY_MOD_CTRL }, "C-c C-p", "a mode map -> cmd_invoke()",
	    "git-rebase-pick", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 1, NULL,
	    NULL },
	{ { 'r', KEY_MOD_CTRL }, "C-c C-r", "a mode map -> cmd_invoke()",
	    "git-rebase-reword", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 1,
	    NULL, NULL },
	{ { 'e', KEY_MOD_CTRL }, "C-c C-e", "a mode map -> cmd_invoke()",
	    "git-rebase-edit", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 1, NULL,
	    NULL },
	{ { 's', KEY_MOD_CTRL }, "C-c C-s", "a mode map -> cmd_invoke()",
	    "git-rebase-squash", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 1,
	    NULL, NULL },
	{ { 'f', KEY_MOD_CTRL }, "C-c C-f", "a mode map -> cmd_invoke()",
	    "git-rebase-fixup", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 1, NULL,
	    NULL },
	{ { 'd', KEY_MOD_CTRL }, "C-c C-d", "a mode map -> cmd_invoke()",
	    "git-rebase-drop", EDITS, K_CMD, L_GIT_REBASE, RO_HANDLER, 1, NULL,
	    NULL },
	{ { 'k', KEY_MOD_CTRL }, "C-c C-k [compilation]",
	    "a mode map -> cmd_invoke()", "kill-compilation", 0, K_CMD,
	    L_COMPILATION, RO_FREE, 1, NULL, NULL },
	{ { -1, 0 }, "C-c <key>", "the user map -> cmd_invoke()", "", 0, K_CMD,
	    L_USER, RO_UNCHECKED, 1, NULL,
	    "whatever (global-set-key ...) bound; the name resolves at "
	    "dispatch" },

	{ { '%', 0 }, "ESC %", "cmd_execute_named(\"query-replace\")",
	    "query-replace", EDITS, K_CMD, L_GLOBAL, RO_COMMAND, 1, NULL,
	    "the ESC branch reaches the same command M-% does" },
	{ { '@', 0 }, "ESC @", "cmd_execute_named(\"mark-word\")", "mark-word",
	    0, K_CMD, L_GLOBAL, RO_FREE, 1, NULL, NULL },

	{ { KEY_BASE_RET, 0 }, "RET [dired]", "a mode map -> cmd_invoke()",
	    "dired-find-file", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ { '^', 0 }, "^ [dired]", "a mode map -> cmd_invoke()",
	    "dired-up-directory", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ { 'g', 0 }, "g [dired]", "a mode map -> cmd_invoke()", "dired-revert",
	    0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ { 'm', 0 }, "m [dired]", "a mode map -> cmd_invoke()", "dired-mark",
	    0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ { 'd', 0 }, "d [dired]", "a mode map -> cmd_invoke()",
	    "dired-flag-file-deletion", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL,
	    NULL },
	{ { 'u', 0 }, "u [dired]", "a mode map -> cmd_invoke()", "dired-unmark",
	    0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ { 'x', 0 }, "x [dired]", "a mode map -> cmd_invoke()",
	    "dired-do-flagged-delete", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL,
	    NULL },
	{ { 'n', 0 }, "n [dired]", "a mode map -> cmd_invoke()", "next-line", 0,
	    K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },
	{ { 'p', 0 }, "p [dired]", "a mode map -> cmd_invoke()",
	    "previous-line", 0, K_CMD, L_DIRED, RO_FREE, 1, NULL, NULL },

	{ { KEY_BASE_RET, 0 }, "RET [read-only]", "a mode map -> cmd_invoke()",
	    "ibuffer-visit-buffer", 0, K_CMD, L_READONLY, RO_FREE, 1, NULL,
	    "the buffer-list map is live in any read-only buffer that is "
	    "not a listing" },
	{ { 'q', 0 }, "q [special]", "a mode map -> cmd_invoke()",
	    "quit-window", 0, K_CMD, L_SPECIAL, RO_FREE, 1, NULL,
	    "the special map's predicate: another buffer has to exist" },
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
 * to the fallback ranges instead.  Meta combinations are not in this
 * domain: since the decoder flag day they are a base plus a modifier
 * bit, not a parallel set of legacy enumerators, so there is nothing
 * left to enumerate them against -- test_global_table_covers_the_key_
 * domain()'s injectivity pass covers them instead. */
static int key_is_in_domain(int c)
{
	return (c >= 0 && c <= 31) || c == BACKSPACE
	    || (c >= ARROW_LEFT && c <= KEY_F4);
}

/* The key_event a domain keycode decodes to, independently of
 * src/keyevent.c and src/tty.c: the same cross-check test_legacy_
 * adapter_spellings used to do, now scoped to what remains enumerable.
 * Zero base means "not a domain keycode". */
static struct key_event domain_event(int c)
{
	switch (c) {
	case KEY_NULL:
		return (struct key_event) { ' ', KEY_MOD_CTRL };
	case TAB:
		return (struct key_event) { KEY_BASE_TAB, 0 };
	case ENTER:
		return (struct key_event) { KEY_BASE_RET, 0 };
	case ESC:
		return (struct key_event) { KEY_BASE_ESC, 0 };
	case 28:
		return (struct key_event) { '\\', KEY_MOD_CTRL };
	case 29:
		return (struct key_event) { ']', KEY_MOD_CTRL };
	case 30:
		return (struct key_event) { '^', KEY_MOD_CTRL };
	case CTRL_UNDERSCORE:
		return (struct key_event) { '_', KEY_MOD_CTRL };
	case BACKSPACE:
		return (struct key_event) { KEY_BASE_DEL, 0 };
	case ARROW_LEFT:
		return (struct key_event) { KEY_BASE_LEFT, 0 };
	case ARROW_RIGHT:
		return (struct key_event) { KEY_BASE_RIGHT, 0 };
	case ARROW_UP:
		return (struct key_event) { KEY_BASE_UP, 0 };
	case ARROW_DOWN:
		return (struct key_event) { KEY_BASE_DOWN, 0 };
	case DEL_KEY:
		return (struct key_event) { KEY_BASE_DELETE, 0 };
	case HOME_KEY:
		return (struct key_event) { KEY_BASE_HOME, 0 };
	case END_KEY:
		return (struct key_event) { KEY_BASE_END, 0 };
	case PAGE_UP:
		return (struct key_event) { KEY_BASE_PRIOR, 0 };
	case PAGE_DOWN:
		return (struct key_event) { KEY_BASE_NEXT, 0 };
	case CTRL_ARROW_LEFT:
		return (struct key_event) { KEY_BASE_LEFT, KEY_MOD_CTRL };
	case CTRL_ARROW_RIGHT:
		return (struct key_event) { KEY_BASE_RIGHT, KEY_MOD_CTRL };
	case CTRL_ARROW_UP:
		return (struct key_event) { KEY_BASE_UP, KEY_MOD_CTRL };
	case CTRL_ARROW_DOWN:
		return (struct key_event) { KEY_BASE_DOWN, KEY_MOD_CTRL };
	case CTRL_HOME:
		return (struct key_event) { KEY_BASE_HOME, KEY_MOD_CTRL };
	case CTRL_END:
		return (struct key_event) { KEY_BASE_END, KEY_MOD_CTRL };
	case SHIFT_ARROW_LEFT:
		return (struct key_event) { KEY_BASE_LEFT, KEY_MOD_SHIFT };
	case SHIFT_ARROW_RIGHT:
		return (struct key_event) { KEY_BASE_RIGHT, KEY_MOD_SHIFT };
	case SHIFT_ARROW_UP:
		return (struct key_event) { KEY_BASE_UP, KEY_MOD_SHIFT };
	case SHIFT_ARROW_DOWN:
		return (struct key_event) { KEY_BASE_DOWN, KEY_MOD_SHIFT };
	case SHIFT_HOME:
		return (struct key_event) { KEY_BASE_HOME, KEY_MOD_SHIFT };
	case SHIFT_END:
		return (struct key_event) { KEY_BASE_END, KEY_MOD_SHIFT };
	case INSERT_KEY:
		return (struct key_event) { KEY_BASE_INSERT, 0 };
	case SHIFT_INSERT:
		return (struct key_event) { KEY_BASE_INSERT, KEY_MOD_SHIFT };
	case SHIFT_DELETE:
		return (struct key_event) { KEY_BASE_DELETE, KEY_MOD_SHIFT };
	case CTRL_INSERT:
		return (struct key_event) { KEY_BASE_INSERT, KEY_MOD_CTRL };
	case KEY_F3:
		return (struct key_event) { KEY_BASE_F3, 0 };
	case KEY_F4:
		return (struct key_event) { KEY_BASE_F4, 0 };
	}
	/* The rest of the control range is the letter it holds down. */
	if (c >= CTRL_A && c <= CTRL_Z) {
		return (struct key_event) { 'a' + c - CTRL_A, KEY_MOD_CTRL };
	}
	return (struct key_event) { 0, 0 };
}

/* Whether a row is bound to the plain (unmodified) character `c` --
 * what the fallback ranges (self-insert) cover. */
static const struct binding *global_row_for(int c)
{
	struct key_event event = { c, 0 };
	int i;

	for (i = 0; i < global_count(); i++) {
		if (key_event_equal(global_bindings[i].key, event)) {
			return &global_bindings[i];
		}
	}
	return NULL;
}

/* Every keycode still enumerable (the control range, DEL, and the named
 * soft keys) is classified exactly once, checked against an
 * independently written expectation.  Meta combinations have no such
 * domain to enumerate any more -- a base plus a modifier bit -- so the
 * second pass instead checks that no two rows, Meta included, share one
 * key_event. */
static void test_global_table_covers_the_key_domain(void)
{
	int c, i, j;

	for (c = 0; c <= KEY_F4; c++) {
		struct key_event event;
		int seen = 0;

		if (!key_is_in_domain(c)) {
			continue;
		}
		event = domain_event(c);
		CHECKF(event.base != 0, "keycode %d has no expected event", c);
		for (i = 0; i < global_count(); i++) {
			if (key_event_equal(global_bindings[i].key, event)) {
				seen++;
			}
		}
		CHECKF(seen == 1, "keycode %d has %d rows, want 1", c, seen);
	}
	for (i = 0; i < global_count(); i++) {
		for (j = i + 1; j < global_count(); j++) {
			CHECKF(!key_event_equal(global_bindings[i].key,
				   global_bindings[j].key),
			    "%s and %s are the same key",
			    global_bindings[i].seq, global_bindings[j].seq);
		}
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
		CHECKF(key_event_equal(parsed, b->key),
		    "%s is not how the table's key_event is spelled", b->seq);
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
