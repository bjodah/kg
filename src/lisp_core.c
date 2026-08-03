#include <stdio.h>
#include <string.h>

#include "lisp.h"

/* Shared with the WITH_LISP=0 stubs and, through lisp_internal.h, with the
 * adapter modules, so it is defined outside either configuration. */
void copy_result(char *result, size_t result_size, const char *text)
{
	if (result == nullptr || result_size == 0) {
		return;
	}

	(void)snprintf(result, result_size, "%s", text);
}

#ifdef KG_USE_LISP

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../fe/fe.h"
#include "cmd.h"
#include "def.h"
#include "lisp.h"
#include "lisp_hooks.h"
#include "lisp_internal.h"
#include "lisp_process.h"

static_assert(FE_API_VERSION == 1);

#ifndef KG_LISP_ARENA_SIZE
#define KG_LISP_ARENA_SIZE (1024U * 1024U)
#endif

#ifndef KG_LISP_STEP_LIMIT
#define KG_LISP_STEP_LIMIT (1U << 20)
#endif

/* The arena holds the whole Fe context, whose 4096-slot GC stack alone is
 * about 36 KiB, and the prelude then costs about 33 KiB of objects.  An
 * override below ~68 KiB therefore fails to start; the default leaves ~93%
 * of the arena for user code. */
static constexpr size_t lisp_arena_size = KG_LISP_ARENA_SIZE;
static constexpr size_t lisp_step_limit = KG_LISP_STEP_LIMIT;
static constexpr size_t lisp_poll_interval = 256;

struct lisp_state state;

void set_error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	/* C23 expands va_start to __builtin_c23_va_start, which clang's
	 * valist checker does not model, so it reports `ap` as
	 * uninitialized here.  Same suppression as the other va_start
	 * sites in the editor. */
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	(void)vsnprintf(state.error, sizeof(state.error), format, ap);
	va_end(ap);
}

static void reset_state(void)
{
	char error[sizeof(state.error)];

	copy_result(error, sizeof(error), state.error);
	memset(&state, 0, sizeof(state));
	copy_result(state.error, sizeof(state.error), error);
}

static void release_frame_buffers(void)
{
	while (state.load_depth > 0) {
		state.load_depth--;
		free(state.load_buffers[state.load_depth]);
		state.load_buffers[state.load_depth] = nullptr;
	}
	free(state.scratch);
	state.scratch = nullptr;
}

void release_scratch(void)
{
	free(state.scratch);
	state.scratch = nullptr;
}

[[noreturn]] static void handle_error(
    FeContext *context, const char *message, FeObject *call_trace)
{
	struct lisp_state *lisp = FeGetUserData(context);

	(void)call_trace;
	if (lisp == nullptr || !lisp->frame_active) {
		abort();
	}
	copy_result(lisp->error, sizeof(lisp->error), message);
	longjmp(lisp->frame.error_jump, 1);
}

static bool interrupt_evaluation(FeContext *context, void *userdata)
{
	struct lisp_state *lisp = userdata;

	(void)context;
	return lisp->interrupt_check != nullptr && lisp->interrupt_check() != 0;
}

const FeEvalOptions eval_options = {
	.step_limit = lisp_step_limit,
	.poll_interval = lisp_poll_interval,
	.interrupt = interrupt_evaluation,
	.userdata = &state,
	.cleanup_step_limit = lisp_step_limit,
};

char *copy_fe_string(FeContext *context, FeObject *object, size_t *length)
{
	size_t allocation;
	char *text;

	*length = FeStringByteLength(context, object);
	if (ckd_add(&allocation, *length, 1)) {
		FeHandleError(context, "string is too large");
	}
	text = malloc(allocation);
	if (!text) {
		FeHandleError(context, "out of memory");
	}
	if (!FeCopyStringBytes(context, object, text, *length)) {
		free(text);
		FeHandleError(context, "cannot copy string");
	}
	text[*length] = '\0';
	return text;
}

struct lisp_command *find_lisp_command(const char *name)
{
	size_t i;

	for (i = 0; i < LISP_MAX_COMMANDS; i++) {
		if (state.commands[i].name[0]
		    && strcmp(state.commands[i].name, name) == 0) {
			return &state.commands[i];
		}
	}
	return nullptr;
}

static void release_lisp_commands(void)
{
	size_t i;

	for (i = 0; i < LISP_MAX_COMMANDS; i++) {
		if (state.commands[i].name[0]) {
			FeReleaseRoot(state.context, state.commands[i].root);
			cmd_runtime_remove(state.commands[i].name);
			state.commands[i].name[0] = '\0';
			state.commands[i].root = nullptr;
		}
	}
}

int kg_lisp_init(void)
{
	size_t alignment, arena_size, padding;
	void *arena;
	FeContext *context;
	volatile bool in_prelude = false;

	if (state.initialized) {
		return 0;
	}
	state.error[0] = '\0';

	alignment = FeArenaAlignment();
	if (alignment == 0) {
		set_error("invalid Fe arena alignment");
		return 1;
	}
	padding = lisp_arena_size % alignment;
	arena_size = lisp_arena_size;
	if (padding != 0
	    && ckd_add(&arena_size, arena_size, alignment - padding)) {
		set_error("Lisp arena size overflow");
		return 1;
	}

	arena = aligned_alloc(alignment, arena_size);
	if (!arena) {
		set_error("cannot allocate Lisp arena: %s", strerror(errno));
		return 1;
	}
	context = FeOpenContext(arena, arena_size);
	if (!context) {
		free(arena);
		set_error("cannot open Fe context");
		return 1;
	}

	state.arena = arena;
	state.context = context;
	state.initialized = true;
	FeSetUserData(context, &state);
	FeSetErrorFn(context, handle_error);
	FeSetGCFn(context, lisp_object_gc);
	state.frame.gc_checkpoint = FeSaveGC(context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		char detail[sizeof(state.error)];

		copy_result(detail, sizeof(detail), state.error);
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		FeCloseContext(state.context);
		free(state.arena);
		reset_state();
		set_error("%s: %s",
		    in_prelude ? "cannot evaluate Lisp prelude"
			       : "cannot register Lisp natives",
		    detail);
		return 1;
	}
	register_natives(context);
	lisp_hooks_init(context);
	lisp_process_init(context);
	in_prelude = true;
	evaluate_prelude(context);
	FeRestoreGC(context, state.frame.gc_checkpoint);
	state.frame_active = false;
	return 0;
}

void kg_lisp_shutdown(void)
{
	if (!state.initialized) {
		return;
	}

	lisp_process_shutdown(state.context);
	lisp_hooks_shutdown(state.context);
	release_lisp_commands();
	FeCloseContext(state.context);
	free(state.arena);
	memset(&state, 0, sizeof(state));
}

int kg_lisp_eval_string(
    const char *source, size_t length, char *result, size_t result_size)
{
	FeObject *value;

	if (!state.initialized) {
		set_error("lisp not initialized");
		copy_result(result, result_size, state.error);
		return 1;
	}
	if (state.frame_active) {
		set_error("nested lisp evaluation is not supported");
		copy_result(result, result_size, state.error);
		return 1;
	}

	state.error[0] = '\0';
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		release_frame_buffers();
		lisp_exec_leave(0);
		copy_result(result, result_size, state.error);
		return 1;
	}
	lisp_exec_enter(state.context);

	/* Fe's core has no printing error path: the installed handler copies
	 * the diagnostic and longjmps.  Evaluation therefore cannot write to
	 * the terminal or alter its mode. */
	value = FeEvaluateStringWithOptions(
	    state.context, "eval", source, length, &eval_options);
	if (result != nullptr && result_size != 0) {
		(void)FeToString(state.context, value, result, result_size);
	}
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	lisp_exec_leave(1);
	return 0;
}

int kg_lisp_load_file(const char *path)
{
	FILE *file;
	int saved_errno;

	if (!state.initialized) {
		set_error("lisp not initialized");
		return 1;
	}
	if (path == nullptr) {
		set_error("invalid Lisp file path");
		return 1;
	}
	if (state.frame_active) {
		set_error("nested lisp evaluation is not supported");
		return 1;
	}
	file = fopen(path, "rb");
	if (!file) {
		saved_errno = errno;
		set_error("cannot open %s: %s", path, strerror(saved_errno));
		return 1;
	}

	state.error[0] = '\0';
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		release_frame_buffers();
		lisp_exec_leave(0);
		(void)fclose(file);
		return 1;
	}
	lisp_exec_enter(state.context);

	(void)FeEvaluateFileWithOptions(
	    state.context, path, file, &eval_options);
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	lisp_exec_leave(1);
	if (fclose(file) != 0) {
		saved_errno = errno;
		set_error("cannot close %s: %s", path, strerror(saved_errno));
		return 1;
	}
	return 0;
}

/* Load the init file if one exists.  A missing or unresolvable file is
 * normal and reports success; any other failure is reported so the caller
 * can display kg_lisp_last_error().  Partially applied init files stand:
 * forms evaluated before an error remain in effect. */
int kg_lisp_load_init(void)
{
	char path[PATH_MAX];

	if (!state.initialized) {
		return 0;
	}
	if (lisp_config_path(path, sizeof(path), "init.fe")) {
		return 0;
	}
	if (access(path, F_OK) != 0) {
		return 0;
	}
	return kg_lisp_load_file(path);
}

const char *kg_lisp_last_error(void) { return state.error; }

int kg_lisp_run_command(const char *name, int fd)
{
	char command_name[LISP_COMMAND_NAME_MAX];
	struct lisp_command *cmd;

	(void)fd;
	if (!state.initialized || name == nullptr) {
		return 1;
	}
	cmd = find_lisp_command(name);
	if (!cmd) {
		return 1;
	}
	if (state.frame_active) {
		editor_set_status_message("Lisp is busy");
		return 0;
	}

	/* The command may remove or redefine itself before raising, so keep a
	 * stable copy of the registered name for the diagnostic below; a
	 * stack copy needs no cleanup across the longjmp. */
	memcpy(command_name, cmd->name, sizeof(command_name));

	state.error[0] = '\0';
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		release_frame_buffers();
		lisp_exec_leave(0);
		editor_set_status_message(
		    "Lisp error: %s: %s", command_name, state.error);
		return 0;
	}
	lisp_exec_enter(state.context);

	/* Call the rooted function directly so the FeCall runs under the
	 * normal step budget and interrupt polling. */
	(void)FeCallWithOptions(
	    state.context, FeGetRoot(cmd->root), nullptr, 0, &eval_options);
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	lisp_exec_leave(1);
	return 0;
}

int kg_lisp_command_exists(const char *name)
{
	return state.initialized && name != nullptr
	    && find_lisp_command(name) != nullptr;
}

const char *kg_lisp_command_name(int index)
{
	size_t i;
	int seen = 0;

	if (index < 0) {
		return nullptr;
	}
	for (i = 0; i < LISP_MAX_COMMANDS; i++) {
		if (!state.commands[i].name[0]) {
			continue;
		}
		if (seen == index) {
			return state.commands[i].name;
		}
		seen++;
	}
	return nullptr;
}

void kg_lisp_set_interrupt_check(int (*check)(void))
{
	state.interrupt_check = check;
}

int kg_lisp_active(void) { return 1; }

#else

static char disabled_error[64] = "lisp not compiled in";

int kg_lisp_init(void)
{
	copy_result(
	    disabled_error, sizeof(disabled_error), "lisp not compiled in");
	return 1;
}

void kg_lisp_shutdown(void) { }

int kg_lisp_eval_string(
    const char *source, size_t length, char *result, size_t result_size)
{
	(void)source;
	(void)length;
	copy_result(
	    disabled_error, sizeof(disabled_error), "lisp not compiled in");
	copy_result(result, result_size, disabled_error);
	return 1;
}

int kg_lisp_load_file(const char *path)
{
	(void)path;
	copy_result(
	    disabled_error, sizeof(disabled_error), "lisp not compiled in");
	return 1;
}

int kg_lisp_load_init(void) { return 0; }

const char *kg_lisp_last_error(void) { return disabled_error; }

int kg_lisp_run_command(const char *name, int fd)
{
	(void)name;
	(void)fd;
	return 1;
}

int kg_lisp_command_exists(const char *name)
{
	(void)name;
	return 0;
}

const char *kg_lisp_command_name(int index)
{
	(void)index;
	return nullptr;
}

void kg_lisp_set_interrupt_check(int (*check)(void)) { (void)check; }

int kg_lisp_active(void) { return 0; }

#endif /* KG_USE_LISP */
