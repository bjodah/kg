#include <errno.h>
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

#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "../fe/fe.h"
#include "def.h"

static_assert(FE_API_VERSION == 1);

#ifndef KG_LISP_ARENA_SIZE
#define KG_LISP_ARENA_SIZE (1024U * 1024U)
#endif

#ifndef KG_LISP_STEP_LIMIT
#define KG_LISP_STEP_LIMIT (1U << 20)
#endif

static constexpr size_t lisp_arena_size = KG_LISP_ARENA_SIZE;
static constexpr size_t lisp_step_limit = KG_LISP_STEP_LIMIT;
static constexpr size_t lisp_poll_interval = 256;
static constexpr size_t lisp_max_load_depth = 8;

struct lisp_frame {
	jmp_buf error_jump;
	size_t gc_checkpoint;
};

struct lisp_state {
	void *arena;
	FeContext *context;
	struct lisp_frame frame;
	char error[1024];
	int (*interrupt_check)(void);
	/* Source buffers owned by in-flight (kg-load ...) calls.  Fe errors
	 * longjmp past the natives, so frame recovery frees the leftovers. */
	char *load_buffers[lisp_max_load_depth];
	size_t load_depth;
	bool frame_active;
	bool initialized;
};

static struct lisp_state state;

static void set_error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
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

static void release_load_buffers(void)
{
	while (state.load_depth > 0) {
		state.load_depth--;
		free(state.load_buffers[state.load_depth]);
		state.load_buffers[state.load_depth] = nullptr;
	}
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

static const FeEvalOptions eval_options = {
	.step_limit = lisp_step_limit,
	.poll_interval = lisp_poll_interval,
	.interrupt = interrupt_evaluation,
	.userdata = &state,
};

static char *copy_fe_string(
    FeContext *context, FeObject *object, size_t *length)
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

static FeObject *native_message(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	size_t length;
	char *message;

	FeRequireNoArguments(context, arguments);
	message = copy_fe_string(context, object, &length);
	(void)length;
	editor_set_status_message("%s", message);
	free(message);
	return FeNil(context);
}

static FeObject *native_insert(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	int row, col;
	size_t length;
	char *text;

	FeRequireNoArguments(context, arguments);
	text = copy_fe_string(context, object, &length);
	if (editor.readonly) {
		free(text);
		FeHandleError(context, "buffer is read-only");
	}
	if (length > INT_MAX) {
		free(text);
		FeHandleError(context, "string is too large to insert");
	}

	/* Match yank/paste: one kg-insert call creates one UNDO_YANK_TEXT
	 * record, while the raw bulk insertion suppresses its internal records.
	 */
	if (length != 0) {
		row = editor_current_filerow_or_eof();
		col = editor_current_filecol();
		undo_push(UNDO_YANK_TEXT, row, col, 0, text, (int)length);
		editor_insert_text_raw(text, (int)length);
	}
	free(text);
	return FeNil(context);
}

static FeObject *native_buffer_name(FeContext *context, FeObject *arguments)
{
	char name[PATH_MAX];

	FeRequireNoArguments(context, arguments);
	buf_display_name(buf_current, name, sizeof(name));
	return FeMakeString(context, name);
}

static FeObject *native_point(FeContext *context, FeObject *arguments)
{
	FeObject *position[2];
	int row, col;

	FeRequireNoArguments(context, arguments);
	/* Lisp positions are 1-based for both row and column. */
	row = editor_current_filerow_or_eof();
	col = editor_current_filecol();
	position[0] = FeMakeDouble(context, (FeDouble)row + 1);
	position[1] = FeMakeDouble(context, (FeDouble)col + 1);
	return FeMakeList(context, position, 2);
}

static int fe_number_to_int(FeContext *context, FeObject *object)
{
	FeDouble value = FeToDouble(context, object);

	if (value != value) {
		FeHandleError(context, "row and column must not be NaN");
	}
	if (value > INT_MAX) {
		return INT_MAX;
	}
	if (value < INT_MIN) {
		return INT_MIN;
	}
	return (int)value;
}

static FeObject *native_goto(FeContext *context, FeObject *arguments)
{
	FeObject *row_object = FeGetNextArgument(context, &arguments);
	FeObject *col_object = FeGetNextArgument(context, &arguments);
	int row, col;

	FeRequireNoArguments(context, arguments);
	row = fe_number_to_int(context, row_object);
	col = fe_number_to_int(context, col_object);
	editor_goto_line_direct(row, col);
	return FeNil(context);
}

struct allowed_command {
	const char *name;
	bool mutates;
};

static const struct allowed_command allowed_commands[] = {
	{ "capitalize-word", true },
	{ "delete-horizontal-space", true },
	{ "delete-trailing-space", true },
	{ "downcase-word", true },
	{ "join-line", true },
	{ "just-one-space", true },
	{ "transpose-chars", true },
	{ "upcase-word", true },
	{ "version", false },
	{ "what-cursor-position", false },
};

[[noreturn]] static void command_error(
    FeContext *context, const char *prefix, const char *name)
{
	char message[1024];

	(void)snprintf(message, sizeof(message), "%s: %s", prefix, name);
	FeHandleError(context, message);
}

static FeObject *native_command(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	const struct allowed_command *allowed = nullptr;
	char *name;
	size_t length, i;

	FeRequireNoArguments(context, arguments);
	name = copy_fe_string(context, object, &length);
	(void)length;
	for (i = 0; i < sizeof(allowed_commands) / sizeof(allowed_commands[0]);
	    i++) {
		if (strcmp(name, allowed_commands[i].name) == 0) {
			allowed = &allowed_commands[i];
			break;
		}
	}
	if (allowed == nullptr) {
		char rejected[512];

		(void)snprintf(rejected, sizeof(rejected), "%s", name);
		free(name);
		command_error(context, "command is not allowed", rejected);
	}
	if (allowed->mutates && editor.readonly) {
		free(name);
		FeHandleError(context, "buffer is read-only");
	}
	if (cmd_execute_named(name, STDIN_FILENO) != 0) {
		char unknown[512];

		(void)snprintf(unknown, sizeof(unknown), "%s", name);
		free(name);
		command_error(context, "unknown command", unknown);
	}
	free(name);
	return FeNil(context);
}

/* Resolve <config>/kg/<stem> using $XDG_CONFIG_HOME, falling back to
 * $HOME/.config.  Returns nonzero when no base is set or the path would
 * not fit. */
static int lisp_config_path(char *out, size_t outsize, const char *stem)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home;
	int n;

	if (xdg && xdg[0]) {
		n = snprintf(out, outsize, "%s/kg/%s", xdg, stem);
	} else {
		home = getenv("HOME");
		if (!home || !home[0]) {
			return 1;
		}
		n = snprintf(out, outsize, "%s/.config/kg/%s", home, stem);
	}
	return n < 0 || (size_t)n >= outsize;
}

/* Read the entire file into a malloc'd buffer.  Returns nullptr and sets
 * state.error on failure. */
static char *read_whole_file(const char *path, size_t *size)
{
	FILE *file;
	char *buffer;
	long file_size;

	file = fopen(path, "rb");
	if (!file) {
		set_error("cannot open %s: %s", path, strerror(errno));
		return nullptr;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0) {
		set_error("cannot read %s: %s", path, strerror(errno));
		(void)fclose(file);
		return nullptr;
	}
	rewind(file);
	buffer = malloc(file_size > 0 ? (size_t)file_size : 1);
	if (!buffer) {
		set_error("cannot load %s: out of memory", path);
		(void)fclose(file);
		return nullptr;
	}
	*size = fread(buffer, 1, (size_t)file_size, file);
	if (*size != (size_t)file_size && ferror(file)) {
		set_error("cannot read %s: %s", path, strerror(errno));
		free(buffer);
		(void)fclose(file);
		return nullptr;
	}
	(void)fclose(file);
	return buffer;
}

/* (kg-load NAME): a name containing '/' is a literal path; a bare name
 * resolves to <config>/kg/lisp/NAME.fe.  Runs nested inside the caller's
 * evaluation, inheriting its step budget.  Loading twice evaluates twice;
 * there is no require/provide. */
static FeObject *native_load(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char path[PATH_MAX];
	char stem[PATH_MAX];
	char *name, *buffer;
	size_t length, size, slot;
	int bad;

	FeRequireNoArguments(context, arguments);
	name = copy_fe_string(context, object, &length);
	if (state.load_depth >= lisp_max_load_depth) {
		free(name);
		FeHandleError(context, "kg-load depth limit exceeded");
	}
	if (strchr(name, '/')) {
		bad = snprintf(path, sizeof(path), "%s", name) < 0
		    || strlen(name) >= sizeof(path);
	} else {
		bad = snprintf(stem, sizeof(stem), "lisp/%s.fe", name) < 0
		    || length + sizeof("lisp/.fe") > sizeof(stem)
		    || lisp_config_path(path, sizeof(path), stem);
	}
	if (bad) {
		char rejected[512];

		(void)snprintf(rejected, sizeof(rejected), "%s", name);
		free(name);
		command_error(context, "cannot resolve package path", rejected);
	}
	free(name);

	buffer = read_whole_file(path, &size);
	if (!buffer) {
		char message[sizeof(state.error)];

		copy_result(message, sizeof(message), state.error);
		FeHandleError(context, message);
	}
	slot = state.load_depth;
	state.load_buffers[slot] = buffer;
	state.load_depth++;
	(void)FeEvaluateString(context, path, buffer, size);
	state.load_depth--;
	state.load_buffers[slot] = nullptr;
	free(buffer);
	return FeNil(context);
}

static void register_natives(FeContext *context)
{
	FeDefineNative(context, "kg-message", native_message);
	FeDefineNative(context, "kg-insert", native_insert);
	FeDefineNative(context, "kg-buffer-name", native_buffer_name);
	FeDefineNative(context, "kg-point", native_point);
	FeDefineNative(context, "kg-goto", native_goto);
	FeDefineNative(context, "kg-command", native_command);
	FeDefineNative(context, "kg-load", native_load);
}

int kg_lisp_init(void)
{
	size_t alignment, arena_size, padding;
	void *arena;
	FeContext *context;

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
	state.frame.gc_checkpoint = FeSaveGC(context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		FeCloseContext(state.context);
		free(state.arena);
		reset_state();
		return 1;
	}
	register_natives(context);
	FeRestoreGC(context, state.frame.gc_checkpoint);
	state.frame_active = false;
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
		release_load_buffers();
		copy_result(result, result_size, state.error);
		return 1;
	}

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
		release_load_buffers();
		(void)fclose(file);
		return 1;
	}

	(void)FeEvaluateFileWithOptions(
	    state.context, path, file, &eval_options);
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
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

void kg_lisp_set_interrupt_check(int (*check)(void)) { (void)check; }

int kg_lisp_active(void) { return 0; }

#endif
