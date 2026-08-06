#include <stdio.h>
#include <string.h>

#include "lisp.h"
#include "perf.h"

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
#include <stdlib.h>
#include <unistd.h>
#if KG_PERF_COUNTERS
#include <time.h>
#endif

#include "../fe/fe.h"
#include "cmd.h"
#include "def.h"
/* lisp.h is already included, unconditionally, above -- this second
 * #include is a no-op through the header guard.  Kept out rather than
 * suppressed: the file still checks every KG_USE_LISP-gated definition
 * below against lisp.h's declarations, via the top-of-file include. */
#include "lisp_hooks.h"
#include "lisp_internal.h"
#include "lisp_obj.h"
#include "lisp_process.h"

static_assert(FE_API_VERSION == 6);
static_assert(FE_LANGUAGE_VERSION == 6);

#ifndef KG_LISP_ARENA_SIZE
#define KG_LISP_ARENA_SIZE (1024U * 1024U)
#endif

#ifndef KG_LISP_STEP_LIMIT
#define KG_LISP_STEP_LIMIT (1U << 20)
#endif

/* The arena holds the whole Fe context, its 4096-slot GC stack and Fe's
 * arena-resident evaluator frames. FeMinimumArenaSize() measures 56856
 * bytes (~55.5 KiB) at the pinned Fe, so an override much below ~64 KiB
 * fails to start; the default's 1 MiB still leaves roughly 95% of the
 * arena for objects and frame growth -- 56225 object slots and a
 * 1097-frame evaluator stack, as kg_lisp_arena_stats() reports them. */
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
	/* A longjmp abandons every nested (require ...) the same way it
	 * abandons every nested (load ...) above; the requiring stack holds
	 * no allocations, just names, so recovery is resetting the count. */
	state.requiring_depth = 0;
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
	lisp->error_kind = FeGetCompletion(context);
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

/* Resolve a function designator to the object it names, the Lisp-2 rule:
 * a symbol reads its function cell (FeGetFunction, which follows defalias
 * indirection and yields nil for an empty cell, and nil -- rather than an
 * error -- for a cyclic alias chain) and any other object passes through
 * unchanged.  The value cell is never consulted, so a name bound only as a
 * value resolves to nothing.  Shared by hooks, process filters/sentinels
 * and functionp. */
FeObject *lisp_function_designator(FeContext *context, FeObject *object)
{
	return FeGetType(object) == FeTSymbol ? FeGetFunction(context, object)
					      : object;
}

static void describe_callable_failure(FeContext *context, const char *condition,
    FeObject *object, char *diagnostic, size_t diagnostic_size)
{
	size_t length;
	char *name;

	if (FeGetType(object) != FeTSymbol) {
		copy_result(diagnostic, diagnostic_size,
		    "tried to call non-callable value");
		return;
	}
	name = copy_fe_string(context, object, &length);
	(void)snprintf(diagnostic, diagnostic_size, "%s %s", condition, name);
	free(name);
}

/* The same rule at a call site the *host* makes -- a hook, a process
 * filter, a sentinel -- reporting rather than raising when the designator
 * names nothing callable.
 *
 * Reporting, and not FeHandleError, is forced by where this runs.  It runs
 * before FeCall has started a nested evaluator run, so Fe's innermost
 * catch still belongs to whatever run sits *below* this C frame -- the
 * `(run-hooks ...)` call's own, when a hook runs from Lisp rather than
 * from an editor event.  A raise here unwinds past the caller's guarded
 * frame before the host error handler ever sees it, and that handler then
 * longjmps into a C frame that has already returned; only an error raised
 * inside the run FeCall starts is contained by the caller's setjmp.  That
 * is why the promised "contained hook error" was, on the (run-hooks) path,
 * a segfault -- for an empty function cell and equally for Fe's own
 * "tried to call non-callable value".
 *
 * Returns the callable, or nullptr with `diagnostic` filled in with the
 * message to report: `void-function NAME` for a designator chain that
 * resolves to nothing, and `invalid-function NAME` for a cell holding
 * something FeCall will not call (a macro, or a primitive -- Fe calls
 * those only from call position).  Resolution itself no longer raises at
 * all: the last hole was FeGetFunction's `cyclic-function-indirection`,
 * and fe's host-facing resolver now answers nil for a cycle instead,
 * precisely because a C caller has no frame for the longjmp to land in.
 * (copy_fe_string, spelling the name into the message, can still raise on
 * an allocation failure -- an out-of-memory path, not a designator one.)
 *
 * `void-function NAME` covers the cycle too, and deliberately.  A cycle
 * and a chain that dies in an empty cell are both nil from
 * FeGetFunction, and the one thing that could separate them here --
 * FeIsFBound -- cannot: `(fset 'a 'b)` with `b` unbound has a non-empty
 * cell as surely as `(fset 'x 'x)` does, so it would rename Fe's own
 * `void-function a` for a dead chain.  Naming the condition kg can prove
 * beats naming one it cannot, and either way the name reported is the one
 * the program wrote, as Emacs reports it. */
FeObject *lisp_callable_designator(FeContext *context, FeObject *object,
    char *diagnostic, size_t diagnostic_size)
{
	FeObject *resolved = lisp_function_designator(context, object);
	FeType type = FeGetType(resolved);

	if (FeIsNil(resolved)) {
		describe_callable_failure(context, "void-function", object,
		    diagnostic, diagnostic_size);
		return nullptr;
	}
	if (type != FeTFn && type != FeTNativeFn) {
		describe_callable_failure(context, "invalid-function", object,
		    diagnostic, diagnostic_size);
		return nullptr;
	}
	return resolved;
}

/* The body-thunk call every *wrapping* native makes -- save-excursion and
 * with-current-buffer today: run `body` between a save that has already
 * been registered with FeProtectWithCleanup and the restore that registry
 * will perform, and leave the enclosing run able to see whatever the body
 * did.
 *
 * FeCall was not that.  It transfers a nested run's completion to the
 * *enclosing* run's barrier, past this native's C frame -- and kg's
 * outermost barrier is the seam's own error_jump, so an error raised inside
 * (save-excursion ...) skipped every condition-case that lexically enclosed
 * the form and surfaced at top level instead.  FeTryCallWithOptions returns
 * the completion rather than throwing it, and FeResignal puts it back in
 * flight in the enclosing run with its kind, its condition object and its
 * message intact, so that condition-case matches on the original condition
 * symbol, and a quit or a budget completion still cancels the whole
 * evaluation.
 *
 * The C restore is deliberately not run here.  It is registered, and
 * FeResignal's own cleanup drain runs it exactly once on the way out --
 * before the handler, which is the order Emacs gives unwind-protect.
 * Running it by hand as well would restore twice.
 *
 * What this still cannot make transparent is `throw`.  Fe walls the throw
 * search at a native re-entry boundary by design (fe's own compat manifest
 * calls it "a tested wall"), so a throw inside the body finds no catch and
 * becomes the condition `(no-catch TAG VALUE)` -- which resignals as an
 * ordinary error an enclosing condition-case can handle, but which the
 * `catch` that names the tag does not receive.  That divergence is written
 * down in test/lisp-compat/features.json's catch-throw-reachability row;
 * closing it means expanding these two forms to Lisp `unwind-protect` in
 * the prelude, so that no native frame stands between the throw and its
 * catch at all. */
FeObject *lisp_call_body(FeContext *context, FeObject *body)
{
	FeObject *value = FeNil(context);

	if (FeTryCallWithOptions(context, body, nullptr, 0, nullptr, &value)) {
		return value;
	}
	FeResignal(context);
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
			FeReleaseRoot(state.context, state.commands[i].function_root);
			FeReleaseRoot(state.context, state.commands[i].interactive_root);
			FeReleaseRoot(state.context, state.commands[i].documentation_root);
			cmd_runtime_remove(state.commands[i].name);
			state.commands[i].name[0] = '\0';
			state.commands[i].function_root = nullptr;
			state.commands[i].interactive_root = nullptr;
			state.commands[i].documentation_root = nullptr;
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
	FeSet(context, FeMakeSymbol(context, "current-prefix-arg"), FeNil(context));
	lisp_hooks_init(context);
	lisp_process_init(context);
	in_prelude = true;
#if KG_PERF_COUNTERS
	{
		struct timespec before, after;

		/* Wall-clock, not a counter: see KG_PERF_LISP_PRELUDE_NS's
		 * comment in perf.h for why this one is not asserted
		 * anywhere. clock_gettime() itself is not gated behind
		 * KG_PERF_COUNTERS elsewhere in kg because nothing else here
		 * needs wall time; guarding it keeps this the one place a
		 * counting build pays for a syscall the shipped editor never
		 * makes. */
		clock_gettime(CLOCK_MONOTONIC, &before);
		evaluate_prelude(context);
		clock_gettime(CLOCK_MONOTONIC, &after);
		KG_PERF_SET(KG_PERF_LISP_PRELUDE_NS,
		    (after.tv_sec - before.tv_sec) * 1000000000LL
			+ (after.tv_nsec - before.tv_nsec));
	}
#else
	evaluate_prelude(context);
#endif
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
	state.last_error_kind = KG_LISP_ERROR_NONE;
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		release_frame_buffers();
		lisp_exec_leave(0);
		lisp_settle_completion();
		if (state.last_error_kind == KG_LISP_ERROR_QUIT) {
			copy_result(result, result_size, "Quit");
		} else {
			copy_result(result, result_size, state.error);
		}
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
	state.last_error_kind = KG_LISP_ERROR_NONE;
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		release_frame_buffers();
		lisp_exec_leave(0);
		(void)fclose(file);
		lisp_settle_completion();
		if (state.last_error_kind == KG_LISP_ERROR_QUIT) {
			copy_result(state.error, sizeof(state.error), "Quit");
		}
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
	if (lisp_config_path(path, sizeof(path), "init.el")) {
		return 0;
	}
	if (access(path, F_OK) != 0) {
		return 0;
	}
	return kg_lisp_load_file(path);
}

const char *kg_lisp_last_error(void) { return state.error; }

enum kg_lisp_error_kind kg_lisp_last_error_kind(void)
{
	return state.last_error_kind;
}

/* Latch Fe's completion kind into lisp.h's fe-free mirror, then disarm the
 * in-flight kind.  Every seam that has finished handling a completion calls
 * this, and that is the point: the latch is what cmd.c reads instead of
 * comparing the reported message to the string "Quit" (sub-plan 06A's
 * Decision 5 is explicit that a host tells quit from an error by kind, not
 * by parsing text), and the disarm keeps a later handler from acting on a
 * stale kind.  The hook and process seams already disarmed and these three
 * did not, which is the asymmetry this removes. */
void lisp_settle_completion(void)
{
	/* Exhaustive over FeCompletion, which is why it is a table and not a
	 * switch with a default: a kind Fe adds is an FE_API_VERSION move,
	 * and the static_assert at the top of this file is what fails then.
	 * Throw mirrors as an error because it only ever reaches a host as
	 * the `no-catch` condition it becomes at a barrier. */
	static const enum kg_lisp_error_kind mirror[] = {
		[FeCompletionNormal] = KG_LISP_ERROR_NONE,
		[FeCompletionError] = KG_LISP_ERROR_ERROR,
		[FeCompletionQuit] = KG_LISP_ERROR_QUIT,
		[FeCompletionThrow] = KG_LISP_ERROR_ERROR,
		[FeCompletionBudget] = KG_LISP_ERROR_BUDGET,
	};

	state.last_error_kind = mirror[state.error_kind];
	state.error_kind = FeCompletionNormal;
}

static void cleanup_prefix_binding(FeContext *context, void *ptr)
{
	struct lisp_prefix_binding *binding = ptr;

	(void)context;
	if (!binding->active) {
		return;
	}
	FeSet(binding->context, binding->symbol, FeGetRoot(binding->old_root));
	FeReleaseRoot(binding->context, binding->old_root);
	FeReleaseRoot(binding->context, binding->new_root);
	binding->active = 0;
	if (state.prefix_binding == binding) {
		state.prefix_binding = nullptr;
	}
	free(binding);
}

[[noreturn]] static void interactive_error(FeContext *context, const char *text)
{
	FeHandleError(context, text);
}

static int interactive_add_prefix(FeContext *context,
	FeObject *raw, FeObject **args, int *argc, char code)
{
	if (code == 'p') {
		args[(*argc)++] = FeMakeInteger(
		    context, lisp_prefix_number(context, raw));
		return 0;
	}
	if (code == 'P') {
		args[(*argc)++] = raw;
		return 0;
	}
	if (code == 'r') {
		if (*argc > LISP_INTERACTIVE_MAX_ARGS - 2) {
			interactive_error(context, "too many interactive arguments");
		}
		FeObject *empty = FeNil(context);
		args[(*argc)++] = native_region_beginning(context, empty);
		args[(*argc)++] = native_region_end(context, empty);
		return 0;
	}
	/* This is the complete measured Emacs code set.  Only p/P/r are
	 * implemented here; the rest are valid but deliberately deferred to 07E
	 * or a later metadata slice. */
	if (strchr("abBcCdDefFGikKmMnNpPrRsSUVvXxZz", code) != nullptr) {
		char message[64];
		(void)snprintf(message, sizeof(message),
		    "unsupported interactive code %c", code);
		interactive_error(context, message);
	}
	{
		char message[64];
		(void)snprintf(message, sizeof(message),
		    "invalid interactive code %c", code);
		interactive_error(context, message);
	}
}

static int interactive_string_args(FeContext *context, FeObject *spec,
	FeObject *raw, FeObject **args)
{
	char *text;
	const char *clause;
	const char *next;
	size_t length;
	int argc = 0;

	text = copy_fe_string(context, spec, &length);
	clause = text;
	if (*text == '\0') {
		free(text);
		return 0;
	}
	while (*clause) {
		next = strchr(clause, '\n');
		if (next != nullptr && next == clause) {
			free(text);
			interactive_error(context, "invalid empty interactive clause");
		}
		if (argc >= LISP_INTERACTIVE_MAX_ARGS) {
			free(text);
			interactive_error(context, "too many interactive arguments");
		}
		if (interactive_add_prefix(context, raw, args, &argc, *clause)
		    != 0) {
			free(text);
			return argc;
		}
		if (next == nullptr) {
			break;
		}
		clause = next + 1;
		if (*clause == '\0') {
			free(text);
			interactive_error(context, "invalid empty interactive clause");
		}
	}
	free(text);
	return argc;
}

static int interactive_form_args(FeContext *context, struct lisp_command *cmd,
	FeObject **args)
{
	FeObject *value;
	FeRoot *value_root;
	int argc = 0;

	value = FeCallWithOptions(context, FeGetRoot(cmd->interactive_root),
	    nullptr, 0, &eval_options);
	value_root = FeCreateRoot(context, value);
	while (!FeIsNil(value)) {
		if (FeGetType(value) != FeTPair) {
			FeReleaseRoot(context, value_root);
			FeHandleError(context, "wrong-type-argument listp");
		}
		if (argc == LISP_INTERACTIVE_MAX_ARGS) {
			FeReleaseRoot(context, value_root);
			FeHandleError(context, "too many interactive arguments");
		}
		args[argc++] = FeCar(context, value);
		value = FeCdr(context, value);
	}
	FeReleaseRoot(context, value_root);
	return argc;
}

int kg_lisp_run_command(const char *name, int fd)
{
	char command_name[LISP_COMMAND_NAME_MAX];
	struct lisp_command *cmd;
	const struct command_prefix *prefix = cmd_active_prefix();
	FeObject *args[LISP_INTERACTIVE_MAX_ARGS];
	FeObject *prefix_object, *symbol, *form, *quoted;
	FeObject *parts[2];
	FeRoot *prefix_root, *old_root;
	struct lisp_prefix_binding *binding;
	int argc = 0;
	int nested;

	(void)fd;
	if (!state.initialized || name == nullptr) {
		return 1;
	}
	cmd = find_lisp_command(name);
	if (!cmd) {
		return 1;
	}
	nested = state.frame_active;

	/* The command may remove or redefine itself before raising, so keep a
	 * stable copy of the registered name for the diagnostic below; a
	 * stack copy needs no cleanup across the longjmp. */
	memcpy(command_name, cmd->name, sizeof(command_name));

	state.error[0] = '\0';
	state.last_error_kind = KG_LISP_ERROR_NONE;
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		if (state.prefix_binding != nullptr) {
			cleanup_prefix_binding(state.context, state.prefix_binding);
		}
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		release_frame_buffers();
		lisp_exec_leave(0);
		lisp_settle_completion();
		if (state.last_error_kind == KG_LISP_ERROR_QUIT) {
			editor_set_status_message("Quit");
		} else {
			editor_set_status_message(
			    "Lisp error: %s: %s", command_name, state.error);
		}
		return 0;
	}
	lisp_exec_enter(state.context);
	symbol = FeMakeSymbol(state.context, "current-prefix-arg");
	parts[0] = FeMakeSymbol(state.context, "quote");
	parts[1] = symbol;
	quoted = FeMakeList(state.context, parts, 2);
	parts[0] = FeMakeSymbol(state.context, "symbol-value");
	parts[1] = quoted;
	form = FeMakeList(state.context, parts, 2);
	old_root = FeCreateRoot(state.context,
	    FeEvaluateWithOptions(state.context, form, &eval_options));
	prefix_object = lisp_prefix_object(state.context, prefix);
	prefix_root = FeCreateRoot(state.context, prefix_object);
	FeSet(state.context, symbol, FeGetRoot(prefix_root));
	binding = calloc(1, sizeof(*binding));
	if (binding == nullptr) {
		FeReleaseRoot(state.context, old_root);
		FeReleaseRoot(state.context, prefix_root);
		FeHandleError(state.context, "out of memory");
	}
	binding->context = state.context;
	binding->symbol = symbol;
	binding->old_root = old_root;
	binding->new_root = prefix_root;
	binding->active = 1;
	state.prefix_binding = binding;
	if (nested) {
		FeProtectWithCleanup(state.context, cleanup_prefix_binding, binding);
	}
	if (cmd->interactive_kind == LISP_INTERACTIVE_STRING) {
		argc = interactive_string_args(state.context,
		    FeGetRoot(cmd->interactive_root), prefix_object, args);
	} else if (cmd->interactive_kind == LISP_INTERACTIVE_FORM) {
		argc = interactive_form_args(state.context, cmd, args);
	}
	(void)FeCallWithOptions(state.context, FeGetRoot(cmd->function_root), args,
	    (size_t)argc, &eval_options);
	if (!nested) {
		cleanup_prefix_binding(state.context, binding);
	}
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

int kg_lisp_arena_stats(struct kg_lisp_arena_stats *out)
{
	FeArenaStats stats;

	if (out == nullptr || !state.initialized) {
		return 1;
	}
	stats = FeGetArenaStats(state.context);
	out->total_slots = stats.total_slots;
	out->free_slots = stats.free_slots;
	out->peak_live_objects = stats.peak_live_objects;
	out->collection_count = stats.collection_count;
	out->peak_gc_stack_depth = stats.peak_gc_stack_depth;
	out->frame_capacity = stats.frame_capacity;
	out->peak_frame_depth = stats.peak_frame_depth;
	out->peak_cleanup_stack_depth = stats.peak_cleanup_stack_depth;
	out->peak_native_reentry = stats.peak_native_reentry;
	out->allocation_failures = stats.allocation_failures;
	return 0;
}

void kg_lisp_perf_snapshot(void)
{
	struct kg_lisp_arena_stats stats;

	if (kg_lisp_arena_stats(&stats) != 0) {
		return;
	}
	KG_PERF_SET(KG_PERF_LISP_ARENA_TOTAL_SLOTS, stats.total_slots);
	KG_PERF_SET(KG_PERF_LISP_ARENA_FREE_SLOTS, stats.free_slots);
	KG_PERF_SET(KG_PERF_LISP_ARENA_PEAK_LIVE, stats.peak_live_objects);
	KG_PERF_SET(KG_PERF_LISP_GC_COUNT, stats.collection_count);
	KG_PERF_SET(KG_PERF_LISP_PEAK_GC_STACK, stats.peak_gc_stack_depth);
	KG_PERF_SET(KG_PERF_LISP_FRAME_CAPACITY, stats.frame_capacity);
	KG_PERF_SET(KG_PERF_LISP_PEAK_FRAME_DEPTH, stats.peak_frame_depth);
	KG_PERF_SET(
	    KG_PERF_LISP_PEAK_CLEANUP_STACK, stats.peak_cleanup_stack_depth);
	KG_PERF_SET(
	    KG_PERF_LISP_PEAK_NATIVE_REENTRY, stats.peak_native_reentry);
	KG_PERF_SET(KG_PERF_LISP_ALLOC_FAILURES, stats.allocation_failures);
}

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

enum kg_lisp_error_kind kg_lisp_last_error_kind(void)
{
	return KG_LISP_ERROR_NONE;
}

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

int kg_lisp_arena_stats(struct kg_lisp_arena_stats *out)
{
	(void)out;
	return 1;
}

void kg_lisp_perf_snapshot(void) { }

#endif /* KG_USE_LISP */
