#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../fe/fe.h"
#include "bufhandle.h"
#include "def.h"
#include "event.h"
#include "lisp.h"
#include "lisp_internal.h"
#include "lisp_obj.h"
#include "lisp_process.h"
#include "process.h"
#include "process_table.h"
#include "prochandle.h"

/* argv[0] (the program) plus up to this many more strings; one slot short
 * of LISP_MAX_PROCESS_ARGS is the NULL terminator kg_process_spawn() needs. */
#define LISP_MAX_PROCESS_ARGS 32
/* Combined byte budget for start-process's argv strings, held in one
 * malloc'd, NUL-separated buffer tracked as state.scratch so frame
 * recovery frees it if a later argument is malformed. */
#define LISP_PROCESS_ARGV_BYTES (32 * 1024)

/* One process table slot's Lisp-side binding: the filter and/or sentinel
 * function, if either is set.  Indexed by slot, generation-checked against
 * the table's own (slot, generation) identity so a slot a new process has
 * reclaimed never runs the previous occupant's callbacks -- see
 * binding_for(). */
struct kg_process_binding {
	uint32_t generation; /* 0: no binding recorded for this slot yet */
	FeRoot *filter;
	FeRoot *sentinel;
};

static struct kg_process_binding bindings[KG_PROCESS_TABLE_MAX];
static struct kg_event_subscriber_token event_sub_token;
static bool subscribed;

static void release_binding(struct kg_process_binding *b)
{
	if (b->filter != NULL) {
		FeReleaseRoot(state.context, b->filter);
		b->filter = NULL;
	}
	if (b->sentinel != NULL) {
		FeReleaseRoot(state.context, b->sentinel);
		b->sentinel = NULL;
	}
}

/* `handle`'s binding slot.  Only ever called with a handle that currently
 * resolves (kg_process_table_resolves() true), which is what makes the
 * generation check below safe: a mismatch means the slot's stale binding
 * belongs to whatever process occupied it *before* `handle`, reclaimed
 * without ever releasing its filter/sentinel roots (kg_process_table_spawn()
 * may reclaim a finished, unreleased entry) -- release them here rather
 * than leak across the reuse, then adopt `handle`'s generation.  NULL only
 * for an out-of-range handle, which a resolving handle never is. */
static struct kg_process_binding *binding_for(struct kg_process_handle handle)
{
	struct kg_process_binding *b;

	if (handle.slot >= KG_PROCESS_TABLE_MAX) {
		return NULL;
	}
	b = &bindings[handle.slot];
	if (b->generation != handle.generation) {
		release_binding(b);
		b->generation = handle.generation;
	}
	return b;
}

/* Copy of src/lisp_hooks.c's run_one_hook_function(), and for the same
 * reason: containment belongs inside Fe, in a frame that is live for the
 * length of the call, not in a host setjmp that Fe's own transfer has
 * already unwound past.  FeTryCallWithOptions() returns the completion
 * instead of throwing it, so a broken filter or sentinel is reported and
 * swallowed here -- the semantics this seam always promised -- while a quit
 * or a budget completion is put back in flight with FeResignal() rather
 * than swallowed, exactly as the hook seam does it.  The landing site for
 * that is the guarded frame the event callback below owns; a filter or a
 * sentinel only ever runs from there, since the subscriber declines to run
 * while a Lisp frame is active. */
static void call_process_callback(FeContext *ctx, FeRoot *root, FeObject **args,
    size_t argc, const char *what)
{
	FeObject *fn = FeGetRoot(root);
	FeObject *value;
	FeCompletion kind;
	size_t gc;
	char diagnostic[LISP_CALLABLE_DIAGNOSTIC_MAX];

	if (fn == NULL || FeIsNil(fn)) {
		return;
	}
	gc = FeSaveGC(ctx);
	value = FeNil(ctx);
	/* A filter or sentinel may be a symbol designator: resolve it now that
	 * the callback runs, exactly as hooks do, so redefining the named
	 * function afterwards takes effect.  An empty cell is reported, not
	 * raised -- see lisp_callable_designator -- so the status line names
	 * which function was missing instead of Fe complaining about an
	 * anonymous non-callable value. */
	fn = lisp_callable_designator(ctx, fn, diagnostic, sizeof(diagnostic));
	if (fn == NULL) {
		FeRestoreGC(ctx, gc);
		editor_set_status_message(
		    "Process %s error: %s", what, diagnostic);
		return;
	}
	lisp_exec_enter(ctx);
	if (FeTryCallWithOptions(ctx, fn, args, argc, &eval_options, &value)) {
		FeRestoreGC(ctx, gc);
		lisp_exec_leave(1);
		return;
	}
	kind = FeGetCompletion(ctx);
	FeRestoreGC(ctx, gc);
	lisp_exec_leave(0);
	release_scratch();
	if (kind == FeCompletionQuit || kind == FeCompletionBudget) {
		FeResignal(ctx);
	}
	editor_set_status_message(
	    "Process %s error: %s", what, FeGetCompletionMessage(ctx));
}

/* Flush whatever output `handle` still has queued to its filter, if one is
 * set.  Requirement (a)'s flush-before-sentinel step calls this explicitly
 * before running the sentinel, rather than leaning on this module's
 * subscriber having already run for an earlier OUTPUT event -- an EXIT can
 * be the only event left for a process whose last bytes never got their
 * own successfully-published OUTPUT event (see process_table.c's Phase 7a
 * notes on reservation refusal). */
static void deliver_filter_output(FeContext *ctx,
    struct kg_process_handle handle, struct kg_process_binding *b)
{
	char *text;
	size_t len;

	if (b->filter == NULL) {
		return;
	}
	text = kg_process_table_take_output(handle, &len);
	if (text == NULL) {
		return;
	}
	if (len > 0) {
		size_t gc = FeSaveGC(ctx);
		FeObject *args[2];

		args[0] = lisp_process_object(ctx, handle);
		args[1] = FeMakeString(ctx, text);
		/* The Fe string owns a copy; drop the C buffer before the
		 * callback runs, because a quit or budget completion inside
		 * the filter is re-signalled rather than swallowed and
		 * leaves this frame by longjmp. */
		free(text);
		text = NULL;
		call_process_callback(ctx, b->filter, args, 2, "filter");
		FeRestoreGC(ctx, gc);
	}
	free(text);
}

static void format_status_string(
    char *out, size_t outsize, const struct kg_process_table_info *info)
{
	switch (info->status) {
	case KG_PROCESS_EXITED:
		if (info->code == 0) {
			(void)snprintf(out, outsize, "finished\n");
		} else {
			(void)snprintf(out, outsize,
			    "exited abnormally with code %d\n", info->code);
		}
		break;
	case KG_PROCESS_SIGNALED:
		(void)snprintf(
		    out, outsize, "terminated by signal %d\n", info->code);
		break;
	default:
		(void)snprintf(out, outsize, "run\n");
		break;
	}
}

static void deliver_sentinel(FeContext *ctx, struct kg_process_handle handle,
    struct kg_process_binding *b)
{
	struct kg_process_table_info info;
	char status_text[64];
	size_t gc;
	FeObject *args[2];

	if (b->sentinel == NULL || !kg_process_table_query(handle, &info)) {
		return;
	}
	format_status_string(status_text, sizeof(status_text), &info);
	gc = FeSaveGC(ctx);
	args[0] = lisp_process_object(ctx, handle);
	args[1] = FeMakeString(ctx, status_text);
	call_process_callback(ctx, b->sentinel, args, 2, "sentinel");
	FeRestoreGC(ctx, gc);
}

/* The guarded frame for one process event, and the only one on this path:
 * the subscriber below runs only when no Lisp frame is active, so there is
 * no enclosing evaluator run and no live host frame -- and Fe's error
 * callback aborts without one.  It catches what the protected call in
 * call_process_callback() deliberately does not contain: a raise from kg's
 * own bookkeeping around the call, and the quit or budget completion that
 * seam re-signals instead of swallowing.  Either abandons the rest of this
 * event's delivery, which is the point; an ordinary filter or sentinel
 * error does not. */
static void deliver_process_event(FeContext *ctx,
    struct kg_process_handle handle, struct kg_process_binding *b, bool exited)
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
			    "Process error: %s", state.error);
		}
		state.frame_active = false;
		release_scratch();
		return;
	}
	/* Requirement (a): every remaining byte reaches the filter before the
	 * sentinel runs, for both event kinds -- an EXIT is not assumed to
	 * follow an already-delivered OUTPUT event for the same bytes. */
	deliver_filter_output(ctx, handle, b);
	if (exited) {
		deliver_sentinel(ctx, handle, b);
	}
	FeRestoreGC(ctx, gc);
	state.frame_active = false;
}

static enum kg_event_cb_status lisp_process_event_cb(const struct kg_event *ev,
    enum kg_event_resolution resolution, void *user_ctx)
{
	FeContext *ctx = user_ctx;
	struct kg_process_handle handle;
	struct kg_process_binding *b;

	if (ctx == NULL || !state.initialized || state.frame_active) {
		return KG_EVENT_CB_CONTINUE;
	}
	if (ev->kind != KG_EVENT_PROCESS_OUTPUT
	    && ev->kind != KG_EVENT_PROCESS_EXIT) {
		return KG_EVENT_CB_CONTINUE;
	}
	if (resolution == KG_EVENT_RESOLVED_GONE) {
		return KG_EVENT_CB_CONTINUE;
	}
	handle = ev->payload.process.process;
	b = binding_for(handle);
	if (b == NULL) {
		return KG_EVENT_CB_CONTINUE;
	}
	deliver_process_event(
	    ctx, handle, b, ev->kind == KG_EVENT_PROCESS_EXIT);
	return KG_EVENT_CB_CONTINUE;
}

void lisp_process_init(FeContext *ctx)
{
	if (subscribed) {
		return;
	}
	event_sub_token = kg_event_subscribe(
	    (1u << KG_EVENT_PROCESS_OUTPUT) | (1u << KG_EVENT_PROCESS_EXIT),
	    lisp_process_event_cb, ctx);
	subscribed = true;
}

void lisp_process_shutdown(FeContext *ctx)
{
	size_t i;

	(void)ctx;
	for (i = 0; i < KG_PROCESS_TABLE_MAX; i++) {
		release_binding(&bindings[i]);
		bindings[i].generation = 0;
	}
	if (subscribed) {
		(void)kg_event_unsubscribe(event_sub_token);
		subscribed = false;
	}
}

/* ---- start-process / start-shell-command ------------------------------- */

/* Build `program`, then as many of `rest`'s strings as fit, into a NUL-
 * separated buffer tracked as state.scratch (freed by frame recovery on any
 * raise below, and explicitly by the caller once kg_process_table_spawn()
 * has used it), and fill `argv` with pointers into it, NULL-terminated. */
static void build_process_argv(FeContext *context, FeObject *program,
    FeObject *rest, const char *argv[LISP_MAX_PROCESS_ARGS + 1], int *argc_out)
{
	char *buf = malloc(LISP_PROCESS_ARGV_BYTES);
	size_t used = 0;
	int argc = 0;
	FeObject *obj = program;

	if (!buf) {
		FeHandleError(context, "start-process: out of memory");
	}
	state.scratch = buf;
	for (;;) {
		size_t len;

		if (FeGetType(obj) != FeTString) {
			FeHandleError(context,
			    "start-process: expected a string argument");
		}
		if (argc >= LISP_MAX_PROCESS_ARGS) {
			FeHandleError(
			    context, "start-process: too many arguments");
		}
		len = FeStringByteLength(context, obj);
		if (used + len + 1 > LISP_PROCESS_ARGV_BYTES) {
			FeHandleError(
			    context, "start-process: arguments too long");
		}
		if (len > 0
		    && !FeCopyStringBytes(context, obj, buf + used, len)) {
			FeHandleError(
			    context, "start-process: cannot copy argument");
		}
		buf[used + len] = '\0';
		argv[argc++] = buf + used;
		used += len + 1;
		if (FeIsNil(rest)) {
			break;
		}
		obj = FeGetNextArgument(context, &rest);
	}
	argv[argc] = NULL;
	*argc_out = argc;
}

FeObject *native_start_process(FeContext *context, FeObject *arguments)
{
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	FeObject *buffer_object = FeGetNextArgument(context, &arguments);
	FeObject *program_object = FeGetNextArgument(context, &arguments);
	const char *argv[LISP_MAX_PROCESS_ARGS + 1];
	int argc;
	struct kg_buffer_handle buffer;
	struct kg_spawn_request req = { .stdin_fd = -1,
		.stderr_to_output = true,
		.nonblocking_output = true };
	struct kg_process_handle handle;

	if (FeGetType(name_object) != FeTString) {
		FeHandleError(context, "start-process: expected a name string");
	}
	buffer = lisp_buffer_or_name_or_nil(
	    context, buffer_object, "start-process");
	build_process_argv(context, program_object, arguments, argv, &argc);
	req.argv = argv;
	handle = kg_process_table_spawn(&req, buffer);
	release_scratch();
	if (handle.generation == 0) {
		FeHandleError(context, "start-process: cannot start process");
	}
	return lisp_process_object(context, handle);
}

FeObject *native_start_shell_command(FeContext *context, FeObject *arguments)
{
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	FeObject *buffer_object = FeGetNextArgument(context, &arguments);
	FeObject *command_object = FeGetNextArgument(context, &arguments);
	struct kg_buffer_handle buffer;
	char *command;
	size_t length;
	struct kg_spawn_request req = { .stdin_fd = -1,
		.stderr_to_output = true,
		.nonblocking_output = true };
	struct kg_process_handle handle;

	FeRequireNoArguments(context, arguments);
	if (FeGetType(name_object) != FeTString) {
		FeHandleError(
		    context, "start-shell-command: expected a name string");
	}
	buffer = lisp_buffer_or_name_or_nil(
	    context, buffer_object, "start-shell-command");
	if (FeGetType(command_object) != FeTString) {
		FeHandleError(
		    context, "start-shell-command: expected a command string");
	}
	command = copy_fe_string(context, command_object, &length);
	req.command = command;
	handle = kg_process_table_spawn(&req, buffer);
	free(command);
	if (handle.generation == 0) {
		FeHandleError(
		    context, "start-shell-command: cannot start command");
	}
	return lisp_process_object(context, handle);
}

/* ---- The remaining six natives ------------------------------------------ */

FeObject *native_process_live_p(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct kg_process_handle handle;
	struct kg_process_table_info info;

	FeRequireNoArguments(context, arguments);
	handle = lisp_process_resolve(context, object, "process-live-p");
	if (!kg_process_table_query(handle, &info)) {
		return FeMakeBool(context, false);
	}
	return FeMakeBool(context, info.status == KG_PROCESS_RUNNING);
}

FeObject *native_delete_process(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct kg_process_handle handle;
	struct kg_process_binding *b;

	FeRequireNoArguments(context, arguments);
	handle = lisp_process_resolve(context, object, "delete-process");
	if (!kg_process_table_terminate_and_release(handle)) {
		FeHandleError(context, "delete-process: cannot delete process");
	}
	/* "Roots. Filter and sentinel roots are released when the entry is
	 * released" (process_table.h's design comment): do that explicitly
	 * here rather than waiting for a later spawn to reclaim the slot and
	 * discover the stale binding. */
	b = binding_for(handle);
	if (b != NULL) {
		release_binding(b);
	}
	return FeNil(context);
}

FeObject *native_process_buffer(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct kg_process_handle handle;
	struct kg_buffer_handle buffer;

	FeRequireNoArguments(context, arguments);
	handle = lisp_process_resolve(context, object, "process-buffer");
	buffer = kg_process_table_buffer(handle);
	if (buf_resolve(buffer) == NULL) {
		return FeNil(context);
	}
	return lisp_buffer_object(context, buffer);
}

/* binding_for() answers NULL only for a slot outside the table, which a
 * handle that just resolved cannot name -- but that invariant lives in
 * process_table.c, and gcc's analyzer (.ci/ci-03) is right to refuse to
 * infer it across two translation units.  State it here, where the
 * dereference is, rather than leave the next reader to reconstruct it. */
static struct kg_process_binding *binding_for_live(
    FeContext *context, struct kg_process_handle handle, const char *what)
{
	struct kg_process_binding *b = binding_for(handle);

	if (b == NULL) {
		FeHandleError(context, what);
	}
	return b;
}

static bool lisp_callable_or_nil(FeObject *fn)
{
	return fn != NULL
	    && (FeIsNil(fn) || FeGetType(fn) == FeTFn
		|| FeGetType(fn) == FeTNativeFn || FeGetType(fn) == FeTSymbol);
}

FeObject *native_set_process_filter(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeObject *fn = FeGetNextArgument(context, &arguments);
	struct kg_process_handle handle;
	struct kg_process_binding *b;

	FeRequireNoArguments(context, arguments);
	handle = lisp_process_resolve(context, object, "set-process-filter");
	if (!kg_process_table_resolves(handle)) {
		FeHandleError(context, "set-process-filter: process is dead");
	}
	if (!lisp_callable_or_nil(fn)) {
		FeHandleError(
		    context, "set-process-filter: expected function or nil");
	}
	b = binding_for_live(context, handle, "set-process-filter: no slot");
	if (b->filter != NULL) {
		FeReleaseRoot(context, b->filter);
		b->filter = NULL;
	}
	if (!FeIsNil(fn)) {
		b->filter = FeCreateRoot(context, fn);
	}
	(void)kg_process_table_set_has_filter(handle, b->filter != NULL);
	return fn;
}

FeObject *native_set_process_sentinel(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeObject *fn = FeGetNextArgument(context, &arguments);
	struct kg_process_handle handle;
	struct kg_process_binding *b;

	FeRequireNoArguments(context, arguments);
	handle = lisp_process_resolve(context, object, "set-process-sentinel");
	if (!kg_process_table_resolves(handle)) {
		FeHandleError(context, "set-process-sentinel: process is dead");
	}
	if (!lisp_callable_or_nil(fn)) {
		FeHandleError(
		    context, "set-process-sentinel: expected function or nil");
	}
	b = binding_for_live(context, handle, "set-process-sentinel: no slot");
	if (b->sentinel != NULL) {
		FeReleaseRoot(context, b->sentinel);
		b->sentinel = NULL;
	}
	if (!FeIsNil(fn)) {
		b->sentinel = FeCreateRoot(context, fn);
	}
	return fn;
}

FeObject *native_process_status(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct kg_process_handle handle;
	struct kg_process_table_info info;

	FeRequireNoArguments(context, arguments);
	handle = lisp_process_resolve(context, object, "process-status");
	if (!kg_process_table_query(handle, &info)) {
		return FeNil(context);
	}
	switch (info.status) {
	case KG_PROCESS_EXITED:
		return FeMakeSymbol(context, "exit");
	case KG_PROCESS_SIGNALED:
		return FeMakeSymbol(context, "signal");
	default:
		return FeMakeSymbol(context, "run");
	}
}
