#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fe/fe.h"
#include "def.h"
#include "event.h"
#include "lisp_hooks.h"
#include "lisp_internal.h"

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

static void run_one_hook_function(FeContext *ctx, FeRoot *root, FeObject **args,
    size_t arg_count, const char *hook_name)
{
	FeObject *fn = FeGetRoot(root);

	if (fn == NULL || FeIsNil(fn)) {
		return;
	}

	/* A hook gets its own guarded frame, and (run-hooks) reaches this
	 * from *inside* a live frame -- so the caller's error_jump and its
	 * checkpoint have to be saved and put back.  Overwriting
	 * state.frame in place left the outer frame's error_jump naming a
	 * stack frame that had already returned, so the next error after a
	 * (run-hooks) longjmp'd into dead stack and killed the editor. */
	struct lisp_frame saved_frame = state.frame;
	bool prev_frame_active = state.frame_active;
	size_t gc = FeSaveGC(ctx);

	state.error[0] = '\0';
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(ctx, gc);
		state.frame = saved_frame;
		state.frame_active = prev_frame_active;
		release_scratch();
		editor_set_status_message(
		    "Hook error (%s): %s", hook_name, state.error);
		return;
	}
	lisp_exec_enter(ctx);
	(void)FeCallWithOptions(ctx, fn, args, arg_count, &eval_options);
	FeRestoreGC(ctx, gc);
	lisp_exec_leave(1);
	state.frame = saved_frame;
	state.frame_active = prev_frame_active;
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
			args[3] = FeMakeDouble(
			    ctx, (FeDouble)ev->payload.changed.old_len);
			run_hook_list(
			    ctx, "after-change-functions", b, args, 4);
			FeRestoreGC(ctx, gc);
		}
		break;
	}
	case KG_EVENT_BUFFER_OPENED: {
		struct editor_buffer *b
		    = buf_resolve(ev->payload.buffer_life.buffer);

		if (b != NULL) {
			run_hook_list(ctx, "find-file-hook", b, NULL, 0);
		}
		break;
	}
	case KG_EVENT_BEFORE_SAVE: {
		struct editor_buffer *b
		    = buf_resolve(ev->payload.before_save.buffer);

		if (b != NULL) {
			run_hook_list(ctx, "before-save-hook", b, NULL, 0);
		}
		break;
	}
	case KG_EVENT_AFTER_SAVE: {
		if (ev->payload.after_save.success) {
			struct editor_buffer *b
			    = buf_resolve(ev->payload.after_save.buffer);

			if (b != NULL) {
				run_hook_list(
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
