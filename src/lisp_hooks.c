#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fe/fe.h"
#include "bufhandle.h"
#include "cmd.h"
#include "def.h"
#include "event.h"
#include "lisp.h"
#include "lisp_hooks.h"
#include "lisp_internal.h"
#include "lisp_obj.h"

#define LISP_MAX_HOOKS 16
#define LISP_MAX_HOOK_ENTRIES_PER_HOOK 16
#define LISP_HOOK_NAME_MAX 64

struct kg_hook_entry {
	FeRoot *root;
	uint32_t id;
	bool buffer_local;
	struct kg_buffer_handle buffer;
};

struct kg_hook {
	char name[LISP_HOOK_NAME_MAX];
	struct kg_hook_entry entries[LISP_MAX_HOOK_ENTRIES_PER_HOOK];
	size_t entry_count;
};

static struct kg_hook hooks[LISP_MAX_HOOKS];
static size_t hook_count = 0;
static uint32_t next_hook_id = 1;
static struct kg_event_subscriber_token event_sub_token;
static bool hooks_registered = false;

static struct kg_hook *find_or_create_hook(const char *name)
{
	size_t i;

	for (i = 0; i < hook_count; i++) {
		if (strcmp(hooks[i].name, name) == 0) {
			return &hooks[i];
		}
	}
	if (hook_count >= LISP_MAX_HOOKS) {
		return NULL;
	}
	struct kg_hook *h = &hooks[hook_count++];
	memset(h, 0, sizeof(*h));
	(void)snprintf(h->name, sizeof(h->name), "%s", name);
	return h;
}

static struct kg_hook *find_hook(const char *name)
{
	size_t i;

	for (i = 0; i < hook_count; i++) {
		if (strcmp(hooks[i].name, name) == 0) {
			return &hooks[i];
		}
	}
	return NULL;
}

/* A hook entry may hold a function value or a symbol naming one.
 * (add-hook 'some-hook 'some-function) is *the* Emacs idiom, and kg's Lisp
 * is Emacs-shaped, so it has to work.  Resolving here -- when the hook
 * runs, rather than in add-hook -- is what makes redefining the function
 * afterwards take effect, as it does in Emacs.  The resolution itself is
 * the shared Lisp-2 designator rule (lisp_callable_designator), which
 * reports rather than raises: an empty function cell becomes the contained
 * hook error `void-function NAME` that README.md and doc/lisp-api.md
 * promise, instead of the anonymous "tried to call non-callable value"
 * FeCall used to produce -- and which, reached from a `(run-hooks ...)`
 * inside a live evaluator run, was not contained at all but a segfault. */
static FeObject *resolve_hook_function(
    FeContext *ctx, FeObject *fn, char *diagnostic, size_t diagnostic_size)
{
	return lisp_callable_designator(ctx, fn, diagnostic, diagnostic_size);
}

/* Containment lives inside Fe now, not in a setjmp of kg's own.
 *
 * (run-hooks) reaches this from *inside* a live evaluator run, and
 * FeCall/FeCallWithOptions transfer a nested run's completion to the
 * *enclosing* run's barrier -- past this C frame.  A host setjmp here was
 * therefore a longjmp target that Fe had already unwound past by the time
 * the host error callback ran: undefined behaviour, and in practice the
 * corrupted GC stack the hook-containment test used to report.  Fe's
 * FeTryCallWithOptions() puts the setjmp inside Fe, in a frame that is live
 * for exactly as long as the call, and *returns* the completion instead of
 * throwing it: on false the callee's cleanups have run, its frames and GC
 * entries are gone, this frame was never unwound, and the accessors
 * describe what happened.  Its barrier is a catch wall too, so a hook that
 * throws to a tag it does not itself catch is contained here as `no-catch`
 * rather than escaping into whatever catch the editor's own evaluation had
 * open.
 *
 * A hook error is swallowed -- a broken hook in a user's init file must not
 * take the editor's evaluation down with it -- but a quit or a budget
 * completion is not a hook error and is put back in flight with
 * FeResignal(): C-g must cancel the whole evaluation, never be eaten by the
 * containment of whichever hook happened to be running when it arrived.
 * The enclosing landing site is Fe's own barrier on the (run-hooks) path,
 * and run_hook_list_guarded()'s frame on the editor-event path. */
static void run_one_hook_function(FeContext *ctx, FeRoot *root, FeObject **args,
    size_t arg_count, const char *hook_name)
{
	FeObject *fn = FeGetRoot(root);
	FeObject *target;
	FeObject *value;
	FeCompletion kind;
	char diagnostic[LISP_CALLABLE_DIAGNOSTIC_MAX];
	struct kg_lisp_exec_ctx saved_exec;
	size_t gc;

	if (fn == NULL || FeIsNil(fn)) {
		return;
	}

	saved_exec = state.exec;
	gc = FeSaveGC(ctx);
	value = FeNil(ctx);

	target = resolve_hook_function(ctx, fn, diagnostic, sizeof(diagnostic));
	if (target == NULL) {
		FeRestoreGC(ctx, gc);
		editor_set_status_message(
		    "Hook error (%s): %s", hook_name, diagnostic);
		state.exec = saved_exec;
		return;
	}
	lisp_exec_enter(ctx);
	cmd_prompt_block();
	if (FeTryCallWithOptions(
		ctx, target, args, arg_count, &eval_options, &value)) {
		cmd_prompt_unblock();
		FeRestoreGC(ctx, gc);
		lisp_exec_leave(1);
		state.exec = saved_exec;
		return;
	}
	kind = FeGetCompletion(ctx);
	cmd_prompt_unblock();
	FeRestoreGC(ctx, gc);
	lisp_exec_leave(0);
	state.exec = saved_exec;
	release_scratch();
	if (kind == FeCompletionQuit || kind == FeCompletionBudget) {
		FeResignal(ctx);
	}
	editor_set_status_message(
	    "Hook error (%s): %s", hook_name, FeGetCompletionMessage(ctx));
}

static void run_hook_list(FeContext *ctx, const char *hook_name,
    struct editor_buffer *b, FeObject **args, size_t arg_count)
{
	struct kg_hook *h = find_hook(hook_name);
	struct kg_buffer_handle handle;
	size_t i;

	if (h == NULL || h->entry_count == 0) {
		return;
	}
	handle = buf_handle_of(b);
	for (i = 0; i < h->entry_count; i++) {
		struct kg_hook_entry *e = &h->entries[i];

		if (e->root == NULL) {
			continue;
		}
		if (e->buffer_local) {
			if (handle.slot != e->buffer.slot
			    || handle.id != e->buffer.id
			    || handle.generation != e->buffer.generation) {
				continue;
			}
		}
		run_one_hook_function(ctx, e->root, args, arg_count, hook_name);
	}
}

/* The editor-event entry point, and the one place on that path that owns a
 * guarded frame.  The subscriber below runs only when no Lisp frame is
 * active (it returns early otherwise), so there is no enclosing evaluator
 * run and no live host frame -- and Fe's error callback aborts without one.
 * This supplies it: one frame for the whole hook list rather than one per
 * hook function, which is what the per-hook setjmp got wrong.  It catches
 * what FeTryCallWithOptions() deliberately does not contain: a raise from
 * kg's own bookkeeping around the call (lisp_exec_enter on a buffer that
 * died), and the quit or budget completion run_one_hook_function() puts
 * back in flight rather than swallowing.  Either one abandons the rest of
 * the hook list, which is the point -- an ordinary hook error does not. */
static void run_hook_list_guarded(FeContext *ctx, const char *hook_name,
    struct editor_buffer *b, FeObject **args, size_t arg_count)
{
	size_t gc = FeSaveGC(ctx);

	state.error[0] = '\0';
	state.frame.gc_checkpoint = gc;
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(ctx, gc);
		lisp_exec_leave(0);
		lisp_settle_completion();
		if (state.last_error_kind == KG_LISP_ERROR_QUIT) {
			editor_set_status_message("Quit");
		} else {
			editor_set_status_message(
			    "Hook error (%s): %s", hook_name, state.error);
		}
		state.frame_active = false;
		release_scratch();
		return;
	}
	run_hook_list(ctx, hook_name, b, args, arg_count);
	FeRestoreGC(ctx, gc);
	state.frame_active = false;
}

static enum kg_event_cb_status lisp_event_subscriber(
    const struct kg_event *ev, enum kg_event_resolution res, void *user_ctx)
{
	FeContext *ctx = user_ctx;
	(void)res;

	if (ctx == NULL || !state.initialized || state.frame_active) {
		return KG_EVENT_CB_CONTINUE;
	}

	switch (ev->kind) {
	case KG_EVENT_BUFFER_CHANGED: {
		struct editor_buffer *b
		    = buf_resolve(ev->payload.changed.buffer);

		if (b != NULL) {
			size_t start_off = lisp_char_offset_of(
			    b, ev->payload.changed.begin_byte);
			size_t end_off = lisp_char_offset_of(b,
			    ev->payload.changed.begin_byte
				+ ev->payload.changed.new_len);
			size_t gc = FeSaveGC(ctx);
			FeObject *args[4];

			args[0] = lisp_buffer_object(
			    ctx, ev->payload.changed.buffer);
			args[1] = lisp_position(ctx, start_off);
			args[2] = lisp_position(ctx, end_off);
			args[3] = FeMakeInteger(
			    ctx, (int64_t)ev->payload.changed.old_len);
			run_hook_list_guarded(
			    ctx, "after-change-functions", b, args, 4);
			FeRestoreGC(ctx, gc);
		}
		break;
	}
	case KG_EVENT_BUFFER_OPENED: {
		struct editor_buffer *b
		    = buf_resolve(ev->payload.buffer_life.buffer);

		if (b != NULL) {
			run_hook_list_guarded(
			    ctx, "find-file-hook", b, NULL, 0);
		}
		break;
	}
	case KG_EVENT_BEFORE_SAVE: {
		struct editor_buffer *b
		    = buf_resolve(ev->payload.before_save.buffer);

		if (b != NULL) {
			run_hook_list_guarded(
			    ctx, "before-save-hook", b, NULL, 0);
		}
		break;
	}
	case KG_EVENT_AFTER_SAVE: {
		if (ev->payload.after_save.success) {
			struct editor_buffer *b
			    = buf_resolve(ev->payload.after_save.buffer);

			if (b != NULL) {
				run_hook_list_guarded(
				    ctx, "after-save-hook", b, NULL, 0);
			}
		}
		break;
	}
	default:
		break;
	}

	return KG_EVENT_CB_CONTINUE;
}

void lisp_hooks_init(FeContext *ctx)
{
	unsigned mask;

	if (hooks_registered) {
		return;
	}
	mask = (1u << KG_EVENT_BUFFER_CHANGED) | (1u << KG_EVENT_BUFFER_OPENED)
	    | (1u << KG_EVENT_BEFORE_SAVE) | (1u << KG_EVENT_AFTER_SAVE);
	event_sub_token = kg_event_subscribe(mask, lisp_event_subscriber, ctx);
	hooks_registered = true;
}

void lisp_hooks_shutdown(FeContext *ctx)
{
	size_t i, j;

	for (i = 0; i < hook_count; i++) {
		for (j = 0; j < hooks[i].entry_count; j++) {
			if (hooks[i].entries[j].root != NULL) {
				FeReleaseRoot(ctx, hooks[i].entries[j].root);
				hooks[i].entries[j].root = NULL;
			}
		}
		hooks[i].entry_count = 0;
	}
	hook_count = 0;

	if (hooks_registered) {
		(void)kg_event_unsubscribe(event_sub_token);
		hooks_registered = false;
	}
}

FeObject *native_add_hook(FeContext *context, FeObject *arguments)
{
	FeObject *name_obj = FeGetNextArgument(context, &arguments);
	FeObject *fn_obj = FeGetNextArgument(context, &arguments);
	FeObject *local_obj = NULL;
	bool buffer_local = false;
	char hook_name[LISP_HOOK_NAME_MAX];
	size_t len;
	char *tmp;
	struct kg_hook *h;
	struct kg_hook_entry *e;

	if (!FeIsNil(arguments)) {
		local_obj = FeGetNextArgument(context, &arguments);
		buffer_local = !FeIsNil(local_obj);
	}
	FeRequireNoArguments(context, arguments);

	if (FeGetType(name_obj) != FeTSymbol
	    && FeGetType(name_obj) != FeTString) {
		FeHandleError(context, "add-hook: expected symbol or string");
	}
	if (FeGetType(fn_obj) != FeTFn && FeGetType(fn_obj) != FeTNativeFn
	    && FeGetType(fn_obj) != FeTSymbol) {
		FeHandleError(context, "add-hook: expected function or symbol");
	}

	tmp = copy_fe_string(context, name_obj, &len);
	if (len == 0 || len >= sizeof(hook_name)) {
		free(tmp);
		FeHandleError(context, "invalid hook name");
	}
	(void)snprintf(hook_name, sizeof(hook_name), "%s", tmp);
	free(tmp);

	h = find_or_create_hook(hook_name);
	if (h == NULL) {
		FeHandleError(context, "hook table full");
	}
	if (h->entry_count >= LISP_MAX_HOOK_ENTRIES_PER_HOOK) {
		FeHandleError(context, "too many functions on hook");
	}

	e = &h->entries[h->entry_count++];
	e->root = FeCreateRoot(context, fn_obj);
	e->id = next_hook_id++;
	e->buffer_local = buffer_local;
	if (buffer_local) {
		e->buffer = state.exec.buffer;
	}

	return FeNil(context);
}

FeObject *native_remove_hook(FeContext *context, FeObject *arguments)
{
	FeObject *name_obj = FeGetNextArgument(context, &arguments);
	FeObject *fn_obj = FeGetNextArgument(context, &arguments);
	char hook_name[LISP_HOOK_NAME_MAX];
	size_t len;
	char *tmp;
	struct kg_hook *h;

	FeRequireNoArguments(context, arguments);

	tmp = copy_fe_string(context, name_obj, &len);
	if (len == 0 || len >= sizeof(hook_name)) {
		free(tmp);
		FeHandleError(context, "invalid hook name");
	}
	(void)snprintf(hook_name, sizeof(hook_name), "%s", tmp);
	free(tmp);

	h = find_hook(hook_name);
	if (h != NULL && h->entry_count > 0) {
		size_t i;

		for (i = 0; i < h->entry_count; i++) {
			if (h->entries[i].root != NULL
			    && FeGetRoot(h->entries[i].root) == fn_obj) {
				FeReleaseRoot(context, h->entries[i].root);
				h->entries[i].root = NULL;
				size_t j;
				for (j = i; j + 1 < h->entry_count; j++) {
					h->entries[j] = h->entries[j + 1];
				}
				h->entry_count--;
				break;
			}
		}
	}

	return FeNil(context);
}

FeObject *native_run_hooks(FeContext *context, FeObject *arguments)
{
	FeObject *name_obj = FeGetNextArgument(context, &arguments);
	char hook_name[LISP_HOOK_NAME_MAX];
	size_t len;
	char *tmp;
	struct editor_buffer *b;

	FeRequireNoArguments(context, arguments);

	tmp = copy_fe_string(context, name_obj, &len);
	if (len == 0 || len >= sizeof(hook_name)) {
		free(tmp);
		FeHandleError(context, "invalid hook name");
	}
	(void)snprintf(hook_name, sizeof(hook_name), "%s", tmp);
	free(tmp);

	b = lisp_exec_buffer(context);
	run_hook_list(context, hook_name, b, NULL, 0);
	return FeNil(context);
}
