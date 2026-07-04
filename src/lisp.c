#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lisp.h"

static void copy_result(char *result, size_t result_size, const char *text)
{
	if (result == nullptr || result_size == 0) {
		return;
	}

	(void)snprintf(result, result_size, "%s", text);
}

#ifdef KG_USE_LISP

#include <setjmp.h>
#include <stdckdint.h>
#include <stdlib.h>

#include "../fe/fe.h"

static_assert(FE_API_VERSION == 1);

#ifndef KG_LISP_ARENA_SIZE
#define KG_LISP_ARENA_SIZE (1024U * 1024U)
#endif

#ifndef KG_LISP_STEP_LIMIT
#define KG_LISP_STEP_LIMIT (1U << 20)
#endif

static constexpr size_t lisp_arena_size = KG_LISP_ARENA_SIZE;
static constexpr size_t lisp_step_limit = KG_LISP_STEP_LIMIT;

struct lisp_frame {
	jmp_buf error_jump;
	size_t gc_checkpoint;
};

struct lisp_state {
	void *arena;
	FeContext *context;
	struct lisp_frame frame;
	char error[256];
	bool frame_active;
	bool initialized;
};

static struct lisp_state state;

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

static const FeEvalOptions eval_options = {
	.step_limit = lisp_step_limit,
	.poll_interval = 0,
	.interrupt = nullptr,
	.userdata = nullptr,
};

int kg_lisp_init(void)
{
	size_t alignment, arena_size, padding;
	void *arena;
	FeContext *context;

	if (state.initialized) {
		return 0;
	}

	alignment = FeArenaAlignment();
	if (alignment == 0) {
		return 1;
	}
	padding = lisp_arena_size % alignment;
	arena_size = lisp_arena_size;
	if (padding != 0
	    && ckd_add(&arena_size, arena_size, alignment - padding)) {
		return 1;
	}

	arena = aligned_alloc(alignment, arena_size);
	if (!arena) {
		return 1;
	}
	context = FeOpenContext(arena, arena_size);
	if (!context) {
		free(arena);
		return 1;
	}

	state.arena = arena;
	state.context = context;
	state.initialized = true;
	FeSetUserData(context, &state);
	FeSetErrorFn(context, handle_error);
	return 0;
}

void kg_lisp_shutdown(void)
{
	if (!state.initialized) {
		return;
	}

	FeCloseContext(state.context);
	free(state.arena);
	memset(&state, 0, sizeof(state));
}

int kg_lisp_eval_string(
    const char *source, size_t length, char *result, size_t result_size)
{
	FeObject *value;

	if (!state.initialized) {
		copy_result(result, result_size, "lisp not initialized");
		return 1;
	}
	if (state.frame_active) {
		copy_result(result, result_size,
		    "nested lisp evaluation is not supported");
		return 1;
	}

	state.error[0] = '\0';
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		copy_result(result, result_size, state.error);
		return 1;
	}

	value = FeEvaluateStringWithOptions(
	    state.context, "eval", source, length, &eval_options);
	if (result != nullptr && result_size != 0) {
		(void)FeToString(state.context, value, result, result_size);
	}
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	return 0;
}

int kg_lisp_load_file(const char *path)
{
	FILE *file;

	if (!state.initialized || path == nullptr || state.frame_active) {
		return 1;
	}
	file = fopen(path, "rb");
	if (!file) {
		return 1;
	}

	state.error[0] = '\0';
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		(void)fclose(file);
		return 1;
	}

	(void)FeEvaluateFileWithOptions(
	    state.context, path, file, &eval_options);
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	(void)fclose(file);
	return 0;
}

int kg_lisp_active(void) { return 1; }

#else

int kg_lisp_init(void) { return 1; }

void kg_lisp_shutdown(void) { }

int kg_lisp_eval_string(
    const char *source, size_t length, char *result, size_t result_size)
{
	(void)source;
	(void)length;
	copy_result(result, result_size, "lisp not compiled in");
	return 1;
}

int kg_lisp_load_file(const char *path)
{
	(void)path;
	return 1;
}

int kg_lisp_active(void) { return 0; }

#endif
