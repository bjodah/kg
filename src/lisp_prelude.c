#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../fe/fe.h"
#include "lisp_internal.h"

struct native_binding {
	const char *name;
	FeNativeFn *fn;
};

/* Every name is the Emacs one wherever Emacs has a matching form; the
 * rest are unprefixed and descriptive. */
static const struct native_binding native_bindings[] = {
	{ "message", native_message },
	{ "insert", native_insert },
	{ "delete-region", native_delete_region },
	{ "delete-char", native_delete_char },
	{ "erase-buffer", native_erase_buffer },
	{ "replace-region", native_replace_region },
	{ "buffer-name", native_buffer_name },
	{ "buffer-file-name", native_buffer_file_name },
	{ "buffer-modified-p", native_buffer_modified_p },
	{ "set-buffer-modified-p", native_set_buffer_modified_p },
	/* `load' and `require' themselves are prelude Lisp (lisp/prelude.el's
	 * loader section): loops of internal--read-form/eval inside one
	 * input unit, so throws, conditions and quits out of a loaded form
	 * reach the caller's own catches and handlers.  These natives are
	 * the loops' C halves. */
	{ "internal--resolve-load", native_internal_resolve_load },
	{ "internal--load-begin", native_internal_load_begin },
	{ "internal--read-form", native_internal_read_form },
	{ "internal--load-end", native_internal_load_end },
	{ "internal--require-resolve", native_internal_require_resolve },
	{ "internal--require-push", native_internal_require_push },
	{ "internal--require-pop", native_internal_require_pop },
	{ "internal--require-check", native_internal_require_check },
	{ "provide", native_provide },
	{ "featurep", native_featurep },
	/* kg's own name: load-path is a bounded C array, not a Fe list a
	 * package could push onto with (setq load-path ...), so it needs a
	 * native rather than the Emacs spelling. */
	{ "add-to-load-path", native_add_to_load_path },
	{ "global-set-key", native_bind_key },
	{ "global-unset-key", native_unbind_key },
	{ "point", native_point_offset },
	{ "point-min", native_point_min },
	{ "point-max", native_point_max },
	{ "goto-char", native_goto_char },
	{ "goto-line", native_goto_line },
	{ "line-number-at-pos", native_line_number },
	{ "current-column", native_current_column },
	{ "mark", native_mark },
	{ "set-mark", native_set_mark },
	{ "deactivate-mark", native_deactivate_mark },
	{ "region-beginning", native_region_beginning },
	{ "region-end", native_region_end },
	{ "buffer-substring", native_buffer_substring },
	{ "char-after", native_char_after },
	{ "forward-word", native_forward_word },
	{ "backward-word", native_backward_word },
	/* Phase 17's motion family (src/lisp_motion.c).  Every one of these
	 * CLAMPS at the ends of the buffer where Emacs signals
	 * `beginning-of-buffer'/`end-of-buffer': fe gates `signal' on its own
	 * condition table and neither name is in it.  Recorded per native in
	 * the manifest, and as an fe-side row in doc/TODO.md. */
	{ "forward-line", native_forward_line },
	{ "forward-char", native_forward_char },
	{ "backward-char", native_backward_char },
	/* Emacs has both `beginning-of-line' (a function taking a count) and
	 * `move-beginning-of-line' (what C-a runs); kg had only the command. */
	{ "beginning-of-line", native_beginning_of_line },
	{ "end-of-line", native_end_of_line },
	{ "skip-chars-forward", native_skip_chars_forward },
	{ "skip-chars-backward", native_skip_chars_backward },
	{ "bounds-of-thing-at-point", native_bounds_of_thing },
	{ "string-length", native_string_length },
	{ "substring", native_substring },
	{ "concat", native_concat },
	{ "format", native_format },
	{ "string=", native_string_equal },
	{ "char-to-string", native_char_to_string },
	{ "string-to-char", native_string_to_char },
	{ "string-to-number", native_string_to_number },
	{ "make-string", native_make_string },
	/* ASCII-only case conversion: kg carries no Unicode case tables.
	 * See src/lisp_string.c and the manifest rows. */
	{ "upcase", native_upcase },
	{ "downcase", native_downcase },
	{ "capitalize", native_capitalize },
	{ "type-of", native_type_of },
	{ "stringp", native_stringp },
	{ "symbolp", native_symbolp },
	{ "numberp", native_numberp },
	{ "consp", native_consp },
	{ "functionp", native_functionp },
	{ "commandp", native_commandp },
	{ "interactive-form", native_interactive_form },
	/* The two halves of the command table Lisp cannot see for itself:
	 * a built-in's name is not a symbol until something writes it, and
	 * its documentation is the one-line summary cmd.c carries. */
	{ "internal--command-names", native_command_names },
	{ "internal--command-documentation", native_command_documentation },
	{ "command-execute", native_command },
	{ "prefix-numeric-value", native_prefix_numeric_value },
	/* Emacs defines commands with defun plus (interactive); kg keeps a
	 * name -> function registry, so these two have no Emacs analogue. */
	{ "define-command", native_define_command },
	{ "remove-command", native_remove_command },
	{ "internal--remove-command-if-present",
	    native_remove_command_if_present },
	/* The minibuffer reads (Phase 16).  Same seam, same re-entrancy
	 * rule and same C-g-is-`quit' as the interactive codes above; what
	 * these add is asking mid-body rather than only in an argument
	 * list. */
	{ "read-string", native_read_string },
	{ "read-number", native_read_number },
	{ "read-file-name", native_read_file_name },
	{ "read-buffer", native_read_buffer },
	{ "y-or-n-p", native_y_or_n_p },
	{ "yes-or-no-p", native_yes_or_no_p },
	{ "completing-read", native_completing_read },
	{ "current-buffer", native_current_buffer },
	{ "buffer-list", native_buffer_list },
	{ "get-buffer", native_get_buffer },
	{ "get-buffer-create", native_get_buffer_create },
	{ "buffer-live-p", native_buffer_live_p },
	{ "set-buffer", native_set_buffer },
	/* set-buffer selects without touching a window; this one shows the
	 * buffer.  Emacs' function form of a name kg had only as a
	 * minibuffer-reading command, which left a package able to build a
	 * report buffer and unable to put it on screen. */
	{ "switch-to-buffer", native_switch_to_buffer },
	{ "kill-buffer", native_kill_buffer },
	{ "search-forward", native_search_forward },
	{ "search-backward", native_search_backward },
	{ "re-search-forward", native_re_search_forward },
	{ "re-search-backward", native_re_search_backward },
	{ "match-beginning", native_match_beginning },
	{ "match-end", native_match_end },
	/* The regex-from-Lisp seam: the engine is src/regex.h's, already
	 * behind C-s and re-search-forward.  What string-match adds is a
	 * string subject and Emacs' character-indexed match data. */
	{ "string-match", native_string_match },
	{ "regexp-quote", native_regexp_quote },
	{ "looking-at", native_looking_at },
	{ "make-marker", native_make_marker },
	{ "point-marker", native_point_marker },
	{ "copy-marker", native_copy_marker },
	{ "set-marker", native_set_marker },
	{ "marker-position", native_marker_position },
	{ "marker-buffer", native_marker_buffer },
	{ "internal--excursion-capture", native_excursion_capture },
	{ "internal--excursion-restore", native_excursion_restore },
	{ "internal--set-buffer-local", native_internal_set_buffer_local },
	{ "make-local-variable", native_make_local_variable },
	{ "kill-local-variable", native_kill_local_variable },
	{ "local-variable-p", native_local_variable_p },
	{ "default-value", native_default_value },
	{ "set-default", native_set_default },
	{ "buffer-local-value", native_buffer_local_value },
	{ "add-hook", native_add_hook },
	{ "remove-hook", native_remove_hook },
	{ "run-hooks", native_run_hooks },
	{ "define-key", native_define_key },
	{ "lookup-key", native_lookup_key },
	{ "current-local-map", native_current_local_map },
	{ "start-process", native_start_process },
	{ "start-shell-command", native_start_shell_command },
	{ "process-live-p", native_process_live_p },
	{ "delete-process", native_delete_process },
	{ "process-buffer", native_process_buffer },
	{ "set-process-filter", native_set_process_filter },
	{ "set-process-sentinel", native_set_process_sentinel },
	{ "process-status", native_process_status },
	/* Phase 1 of doc/plans/2026-08-14-embedded-prelude.md: the deferred-
	 * stub lookup. lisp/prelude.el's internal--make-deferred-stub is the
	 * only caller, once per deferred name, at most once per process. */
	{ "internal--force-deferred", native_internal_force_deferred },
	/* Phase 2 of doc/plans/2026-08-14-embedded-prelude.md: seven of the
	 * ten names Phase 0.2 found eager on every startup path, moved from
	 * lisp/prelude.el lambdas to natives (src/lisp_cmd.c).  The other
	 * three -- internal--let, progn, null -- are already zero-cost
	 * `(defalias NAME (symbol-function PRIM))` captures of an fe
	 * primitive and stay Lisp; see src/lisp_cmd.c's own comment on why a
	 * native cannot stand in for the first two. */
	{ "listp", native_listp },
	{ "reverse", native_reverse },
	{ "nconc", native_nconc },
	{ "internal--bind-name", native_internal_bind_name },
	{ "internal--bind-value", native_internal_bind_value },
	{ "internal--doc-put", native_internal_doc_put },
	{ "internal--variable-doc-put", native_internal_variable_doc_put },
};

void register_natives(FeContext *context)
{
	size_t i;

	for (i = 0; i < sizeof(native_bindings) / sizeof(native_bindings[0]);
	    i++) {
		FeDefineNative(
		    context, native_bindings[i].name, native_bindings[i].fn);
	}
}

/* Forms kg provides that upstream fe does not: the Emacs Lisp surface,
 * written in Fe and evaluated at startup so it is available before any
 * init file runs.
 *
 * lisp/prelude.el is the canonical source; the array below is a generated,
 * byte-for-byte copy of its EAGER forms -- everything except the 93 names
 * utils/prelude_deferred_names.txt lists (utils/embed_lisp_split.py, `make
 * lisp-prelude-check` proves the split still reproduces lisp/prelude.el
 * exactly).  The rules the prelude's definitions must follow -- ordering,
 * recursion, macro-expansion semantics -- are documented once, in that
 * file's own header comment, next to the code they constrain. */
#include "lisp_prelude_generated.inc"

/* Phase 1 of doc/plans/2026-08-14-embedded-prelude.md: the deferred half
 * of the split above.  lisp_prelude_deferred_generated[] holds the same
 * 93 forms' bytes, unchanged, concatenated in their original file order;
 * lisp_prelude_deferred_generated_index[] (utils/embed_lisp_split.py) is
 * where each one starts and how long it is.  Both are read by exactly one
 * function, native_internal_force_deferred() below -- nothing else in
 * this translation unit, or any other, touches them. */
struct lisp_prelude_deferred_entry {
	const char *name;
	size_t offset;
	size_t length;
};
#include "lisp_prelude_deferred_generated.inc"

/* The prelude gets its own evaluation, so it neither consumes nor shares
 * the budget of later user evaluations. */
void evaluate_prelude(FeContext *context)
{
	(void)FeEvaluateStringWithOptions(context, "prelude",
	    (const char *)lisp_prelude_generated, lisp_prelude_generated_len,
	    &eval_options);
}

/* Evaluate the ONE deferred form named by `arguments' -- a symbol or
 * string, the same designator copy_fe_string() already accepts for
 * add-hook/provide/featurep -- replacing its function cell with the real
 * definition.  lisp/prelude.el's internal--make-deferred-stub is the only
 * caller: each stub calls this exactly once, on its own first
 * invocation, before forwarding that call's arguments to the freshly
 * installed function, which is what makes a deferred name behave the
 * same as an eager one from its very first call onward (see
 * install_deferred_stubs() below for the other half).
 *
 * A linear scan over 93 entries, not a hash lookup: this runs at most
 * once per deferred name per process, so the lookup's own cost is
 * nowhere near what the stub it replaces already saved. */
FeObject *native_internal_force_deferred(
    FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char *name;
	size_t length, i;

	FeRequireNoArguments(context, arguments);
	name = copy_fe_string(context, object, &length);
	for (i = 0; i < lisp_prelude_deferred_generated_index_len; i++) {
		if (strcmp(name, lisp_prelude_deferred_generated_index[i].name)
		    == 0) {
			free(name);
			(void)FeEvaluateStringWithOptions(context,
			    "prelude-deferred",
			    (const char *)lisp_prelude_deferred_generated
				+ lisp_prelude_deferred_generated_index[i]
				    .offset,
			    lisp_prelude_deferred_generated_index[i].length,
			    &eval_options);
			return object;
		}
	}
	free(name);
	FeHandleError(
	    context, "internal--force-deferred: unknown deferred name");
}

/* Install one self-replacing stub per name in
 * lisp_prelude_deferred_generated_index[], driven from this generated
 * table rather than from a loop written in lisp/prelude.el -- so the
 * table is the only place the 93 names are enumerated and nothing needs
 * to be kept in step with it by hand.  Each stub is an ordinary Lisp
 * closure (internal--make-deferred-stub's return value, lisp/prelude.el),
 * not a native: functionp, funcall, apply, a hook, and a recursive or
 * cross-deferred call all see a real FeTFn, indistinguishable from an
 * eager definition both before and after its first call replaces it.
 * Runs once, right after evaluate_prelude() -- so the factory function it
 * calls is already defined -- inside kg_lisp_init()'s own setjmp, exactly
 * like evaluate_prelude() itself. */
void install_deferred_stubs(FeContext *context)
{
	FeObject *factory_symbol
	    = FeMakeSymbol(context, "internal--make-deferred-stub");
	FeObject *factory;
	size_t i;

	if (!FeIsFBound(context, factory_symbol)) {
		/* Not the shipped prelude: a prefix of lisp/prelude.el ending
		 * before the factory's own definition (utils/
		 * prelude_slot_census.py's per-section census truncates the
		 * file at arbitrary points, unaware of the eager/deferred
		 * split) has nothing to install a stub FROM.  Skipping is the
		 * same answer every other deferred name's absence from such a
		 * prefix already gets -- that experiment measures exactly
		 * this much prelude, no more. */
		return;
	}
	factory = FeGetFunction(context, factory_symbol);
	for (i = 0; i < lisp_prelude_deferred_generated_index_len; i++) {
		FeObject *symbol = FeMakeSymbol(
		    context, lisp_prelude_deferred_generated_index[i].name);
		FeObject *stub = FeCallWithOptions(
		    context, factory, &symbol, 1, &eval_options);

		FeSetFunction(context, symbol, stub);
	}
}
