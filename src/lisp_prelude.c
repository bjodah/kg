#include <stddef.h>

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
	{ "replace-region", native_replace_region },
	{ "buffer-name", native_buffer_name },
	{ "load", native_load },
	{ "provide", native_provide },
	{ "require", native_require },
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
	{ "bounds-of-thing-at-point", native_bounds_of_thing },
	{ "string-length", native_string_length },
	{ "substring", native_substring },
	{ "concat", native_concat },
	{ "format", native_format },
	{ "string=", native_string_equal },
	{ "char-to-string", native_char_to_string },
	{ "string-to-char", native_string_to_char },
	{ "type-of", native_type_of },
	{ "stringp", native_stringp },
	{ "symbolp", native_symbolp },
	{ "numberp", native_numberp },
	{ "consp", native_consp },
	{ "functionp", native_functionp },
	{ "command-execute", native_command },
	/* Emacs defines commands with defun plus (interactive); kg keeps a
	 * name -> function registry, so these two have no Emacs analogue. */
	{ "define-command", native_define_command },
	{ "remove-command", native_remove_command },
	{ "current-buffer", native_current_buffer },
	{ "buffer-list", native_buffer_list },
	{ "get-buffer", native_get_buffer },
	{ "get-buffer-create", native_get_buffer_create },
	{ "buffer-live-p", native_buffer_live_p },
	{ "set-buffer", native_set_buffer },
	{ "kill-buffer", native_kill_buffer },
	{ "search-forward", native_search_forward },
	{ "search-backward", native_search_backward },
	{ "re-search-forward", native_re_search_forward },
	{ "re-search-backward", native_re_search_backward },
	{ "match-beginning", native_match_beginning },
	{ "match-end", native_match_end },
	{ "make-marker", native_make_marker },
	{ "set-marker", native_set_marker },
	{ "marker-position", native_marker_position },
	{ "marker-buffer", native_marker_buffer },
	{ "internal--save-excursion", native_save_excursion },
	{ "internal--with-current-buffer", native_with_current_buffer },
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
 * byte-for-byte copy of it (utils/embed_lisp.py, `make lisp-prelude-check`
 * proves the two agree).  The rules the prelude's definitions must follow
 * -- ordering, recursion, macro-expansion semantics -- are documented once,
 * in that file's own header comment, next to the code they constrain. */
#include "lisp_prelude_generated.inc"

/* The prelude gets its own evaluation, so it neither consumes nor shares
 * the budget of later user evaluations. */
void evaluate_prelude(FeContext *context)
{
	(void)FeEvaluateStringWithOptions(context, "prelude",
	    (const char *)lisp_prelude_generated, lisp_prelude_generated_len,
	    &eval_options);
}
