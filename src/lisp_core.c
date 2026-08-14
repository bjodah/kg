#include <stdio.h>
#include <string.h>

/* def.h for ascii_is_space/ascii_is_digit, which the numeric classifier
 * at the foot of this file uses and which is compiled in both build
 * configurations; the KG_USE_LISP half checks its own gated definitions
 * against it through the same top-of-file include. */
#include "def.h"
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
#include <stdint.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#if KG_PERF_COUNTERS
#include <time.h>
#endif

#include "../fe/fe.h"
#include "bufhandle.h"
#include "cmd.h"
/* lisp.h is already included, unconditionally, above -- this second
 * #include is a no-op through the header guard.  Kept out rather than
 * suppressed: the file still checks every KG_USE_LISP-gated definition
 * below against lisp.h's declarations, via the top-of-file include. */
#include "lisp_hooks.h"
#include "lisp_internal.h"
#include "lisp_locals.h"
#include "lisp_obj.h"
#include "lisp_process.h"

#ifdef _WIN32
#define lisp_free_arena kg_aligned_free
#else
#define lisp_free_arena free
#endif

static_assert(FE_API_VERSION == 11);
static_assert(FE_LANGUAGE_VERSION == 14);

#ifndef KG_LISP_ARENA_SIZE
#define KG_LISP_ARENA_SIZE (1024U * 1024U)
#endif

#ifndef KG_LISP_STEP_LIMIT
#define KG_LISP_STEP_LIMIT (1U << 20)
#endif

/* The arena holds the whole Fe context, its 4096-slot GC stack and Fe's
 * arena-resident evaluator frames. FeMinimumArenaSize() measures 66264
 * bytes (~64.7 KiB) at the pinned Fe, so an override much below ~72 KiB
 * fails to start; the default's 1 MiB still leaves roughly 94% of the
 * arena for objects and frame growth -- 56147 object slots and a
 * 1087-frame evaluator stack, as kg_lisp_arena_stats() reports them.
 * All three are measured at the pin, never carried forward. */
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

#if KG_PERF_COUNTERS
/* CLOCK_MONOTONIC in nanoseconds, for the three §15 load-time counters
 * (prelude, user init, packages).  Only a counting build has it: the
 * shipped editor takes no clock_gettime() for any of them. */
long long lisp_monotonic_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}
#endif

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

/* Emacs' `error-message-string' of the condition fe is reporting, spliced
 * over the bare condition NAME fe's own message ends in.
 *
 * fe's `signal' uses the condition symbol as the completion's message
 * (fe_eval.c's PSignal arm passes it as both), so an uncaught
 * `(wrong-type-argument integer-or-marker-p "x")' -- which is what every
 * kg native's type check raises since Phase 13.3 -- reported the words
 * `wrong-type-argument' and nothing else: neither the predicate nor the
 * offending value reached the echo area.  Phase 19 gave fe the rendering
 * and `FeErrorMessageString' to reach it from C; this is the splice.
 *
 * MESSAGE arrives as "LABEL:LINE: SYMBOL", so only a trailing bare
 * condition name is replaced and the position fe latched survives.  That
 * suffix test is also what keeps fe's own descriptive messages: `(car 1)'
 * arrives as "expected pair, got integer", which does not end in
 * `wrong-type-argument', so nothing is spliced and fe's text stands.
 *
 * Rebuilt from the condition object rather than from a buffer the raise
 * arms fill, because a raise a Lisp handler catches never reaches this
 * function and would leave one stale.
 *
 * This replaces render_file_condition(), which did the same splice for the
 * two file classes' (OPERATION STRERROR PATH) triple alone.  It needs no
 * successor: Emacs' rule renders that triple as the same sentence -- a
 * `file-error' subtype takes its message from the data and princs the rest
 * -- so "Cannot open load file: No such file or directory, /nope/x.el" is
 * now a special case of the general rendering rather than a private one,
 * and the empty-OPERATION corner it handled ("d, p") is Emacs' own
 * empty-message rule. */
static bool render_condition(
    FeContext *context, char *out, size_t size, const char *message)
{
	FeObject *condition = FeGetCondition(context);
	char name[64];
	size_t used, length;

	if (FeGetType(condition) != FeTPair) {
		return false;
	}
	length = FeToString(
	    context, FeCar(context, condition), name, sizeof(name));
	if (length >= sizeof(name)) {
		return false;
	}
	used = strlen(message);
	if (used < length || strcmp(message + used - length, name) != 0) {
		return false;
	}
	used -= length;
	/* `out' is state.error[1024] and MESSAGE is fe's own 1024-byte
	 * buffer, so today this cannot overflow -- but nothing couples the
	 * two sizes, and MESSAGE carries a host-controlled load label up to
	 * PATH_MAX.  Checked, not assumed. */
	if (used >= size) {
		return false;
	}
	memcpy(out, message, used);
	out[used] = '\0';
	(void)FeErrorMessageString(context, condition, out + used, size - used);
	return true;
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
	if (!render_condition(
		context, lisp->error, sizeof(lisp->error), message)) {
		copy_result(lisp->error, sizeof(lisp->error), message);
	}
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
			FeReleaseRoot(
			    state.context, state.commands[i].function_root);
			FeReleaseRoot(
			    state.context, state.commands[i].interactive_root);
			FeReleaseRoot(state.context,
			    state.commands[i].documentation_root);
			FeReleaseRoot(state.context,
			    state.commands[i].interactive_form_root);
			cmd_runtime_remove(state.commands[i].name);
			state.commands[i].name[0] = '\0';
			state.commands[i].function_root = nullptr;
			state.commands[i].interactive_root = nullptr;
			state.commands[i].documentation_root = nullptr;
			state.commands[i].interactive_form_root = nullptr;
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
		lisp_free_arena(arena);
		set_error("cannot open Fe context");
		return 1;
	}

	state.arena = arena;
	state.context = context;
	state.initialized = true;
	FeSetUserData(context, &state);
	FeSetErrorFn(context, handle_error);
	FeSetGCFn(context, lisp_object_gc);
	/* The per-binding buffer tag Emacs keeps in its specpdl, in the one
	 * place fe offers for it (FE_API_VERSION 11).  Registered here rather
	 * than lazily when the first buffer-local binding appears, because a
	 * `let' pushed before the registration would carry no tag and restore
	 * to the wrong cell afterwards; src/lisp_locals.c's fast path is what
	 * makes it free for a session that never says `setq-local'. */
	FeSetBindingFns(context, lisp_locals_bind_tag, lisp_locals_bind_target);
	state.frame.gc_checkpoint = FeSaveGC(context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		char detail[sizeof(state.error)];

		copy_result(detail, sizeof(detail), state.error);
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		FeCloseContext(state.context);
		lisp_free_arena(state.arena);
		reset_state();
		set_error("%s: %s",
		    in_prelude ? "cannot evaluate Lisp prelude"
			       : "cannot register Lisp natives",
		    detail);
		return 1;
	}
	register_natives(context);
	FeSet(context, FeMakeSymbol(context, "current-prefix-arg"),
	    FeNil(context));
	lisp_hooks_init(context);
	lisp_process_init(context);
	in_prelude = true;
#if KG_PERF_COUNTERS
	{
		/* Wall-clock, not a counter: see KG_PERF_LISP_PRELUDE_NS's
		 * comment in perf.h for why this one is not asserted
		 * anywhere. clock_gettime() itself is not gated behind
		 * KG_PERF_COUNTERS elsewhere in kg because nothing else here
		 * needs wall time; guarding it keeps a counting build the
		 * only one that pays for a syscall the shipped editor never
		 * makes. */
		long long before = lisp_monotonic_ns();

		evaluate_prelude(context);
		KG_PERF_SET(
		    KG_PERF_LISP_PRELUDE_NS, lisp_monotonic_ns() - before);
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
	lisp_free_arena(state.arena);
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
#if KG_PERF_COUNTERS
	{
		long long before = lisp_monotonic_ns();
		int failed = kg_lisp_load_file(path);

		KG_PERF_SET(
		    KG_PERF_LISP_USER_INIT_NS, lisp_monotonic_ns() - before);
		return failed;
	}
#else
	return kg_lisp_load_file(path);
#endif
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
	/* Unlink from the innermost-first chain.  Cleanups are LIFO so this
	 * is almost always the head, but walking is cheap and cannot leave a
	 * freed link behind. */
	if (state.prefix_binding == binding) {
		state.prefix_binding = binding->outer;
	} else {
		struct lisp_prefix_binding *scan = state.prefix_binding;

		while (scan != nullptr && scan->outer != binding) {
			scan = scan->outer;
		}
		if (scan != nullptr) {
			scan->outer = binding->outer;
		}
	}
	free(binding);
}

[[noreturn]] static void interactive_error(FeContext *context, const char *text)
{
	FeHandleError(context, text);
}

/* The one route from a kg native to a real, catchable condition:
 * `(signal 'SYMBOL 'DATA)`, raised so that a `condition-case` between the
 * native and its caller actually sees it.
 *
 * WHY A LISP FORM AT ALL.  FeHandleError() always builds an
 * `(error "text")` condition, so a host seam that merely *spells* a
 * condition name in its message is prose in a structured-conditions tree:
 * `(condition-case e … (wrong-type-argument …))` cannot catch it, and the
 * offending value is nowhere.  Fe exposes no host entry point that
 * constructs an arbitrary condition -- RaiseCondition is private to fe --
 * so the language's own `signal` is the only construction available.
 *
 * WHY THE PROTECTED CALL AND NOT A PLAIN FeEvaluateWithOptions.  Measured:
 * that route does not reach an enclosing `condition-case` at all.
 * FeCall/FeEvaluate start a nested run whose completion transfers to the
 * *outermost* barrier -- kg's own error_jump -- past every handler
 * lexically between the native and the raise, which is the same defect
 * sub-plan 11D Part 3 fixed for the loader.  The protected call contains
 * the completion instead, and FeResignal puts it back in flight in the
 * enclosing run with its kind, condition object and message intact.  Until
 * Phase 13.2 this function had the plain shape and
 * `(condition-case e (prefix-numeric-value "x") (wrong-type-argument …))`
 * escaped its own handler, reaching the host as an uncatchable error.
 *
 * `signal` is function-shaped from fe language version 11 on, but a
 * nullary closure over the form is still what is called: it keeps one
 * construction for every raise here, and evaluating the lambda itself
 * only builds the closure and cannot raise.
 *
 * Every allocation can collect, so `data` and each intermediate stay on
 * the GC stack until the call is made.  The checkpoint is not restored:
 * no exit from here returns. */
[[noreturn]] static void raise_signal_form(
    FeContext *context, const char *symbol, FeObject *data)
{
	FeObject *parts[3];
	FeObject *form, *result;

	FePushGC(context, data);
	parts[0] = FeMakeSymbol(context, "quote");
	parts[1] = data;
	data = FeMakeList(context, parts, 2);
	FePushGC(context, data);
	parts[0] = FeMakeSymbol(context, "quote");
	parts[1] = FeMakeSymbol(context, symbol);
	form = FeMakeList(context, parts, 2);
	FePushGC(context, form);
	parts[0] = form;
	parts[1] = data;
	form = FeMakeList(context, parts, 2);
	FePushGC(context, form);
	form = FeCons(context, FeMakeSymbol(context, "signal"), form);
	FePushGC(context, form);
	parts[0] = FeMakeSymbol(context, "lambda");
	parts[1] = FeNil(context);
	parts[2] = form;
	form = FeMakeList(context, parts, 3);
	FePushGC(context, form);
	form = FeEvaluateWithOptions(context, form, &eval_options);
	FePushGC(context, form);
	if (!FeTryCallWithOptions(
		context, form, nullptr, 0, &eval_options, &result)) {
		FeResignal(context);
	}
	/* `signal` cannot return normally; say so for the compiler and for a
	 * future fe in which it somehow could. */
	FeHandleError(context, symbol);
}

/* Raise Emacs' `(wrong-type-argument PREDICATE VALUE)`: the predicate the
 * argument failed first, the offending value second, which is the shape
 * Emacs and fe's own RaiseWrongType both use.  The message text is the
 * bare condition name, again as fe's is.  PREDICATE is a C string literal
 * at every call site -- it names the Emacs predicate the argument would
 * have had to satisfy, measured against 31.0.90 rather than guessed. */
[[noreturn]] void lisp_raise_wrong_type(
    FeContext *context, const char *predicate, FeObject *value)
{
	FeObject *parts[2];

	FePushGC(context, value);
	parts[0] = FeMakeSymbol(context, predicate);
	parts[1] = value;
	raise_signal_form(
	    context, "wrong-type-argument", FeMakeList(context, parts, 2));
}

/* Raise Emacs' `(void-variable SYMBOL)`: the one-element data shape a
 * reference to an unbound name answers.  kg needs it because two readers
 * -- `default-value` and `buffer-local-value` -- reach a value cell
 * without going through the evaluator's own reference path, and answering
 * nil where Emacs raises would be the lie this phase exists to remove. */
[[noreturn]] void lisp_raise_void_variable(FeContext *context, FeObject *symbol)
{
	FeObject *parts[1];

	FePushGC(context, symbol);
	parts[0] = symbol;
	raise_signal_form(
	    context, "void-variable", FeMakeList(context, parts, 1));
}

/* Raise Emacs' `(args-out-of-range ...)`: the range failure that is not a
 * type failure.  Emacs' data is the offending arguments themselves, in the
 * order the call wrote them and with no predicate in front -- measured,
 * `(match-beginning -1)` is `(args-out-of-range -1 0)` and
 * `(substring "abc" 0 9)` is `(args-out-of-range "abc" 0 9)` -- so the
 * caller passes the values, and the two-element form is what kg's own
 * range failures need. */
[[noreturn]] void lisp_raise_args_out_of_range(
    FeContext *context, FeObject *first, FeObject *second)
{
	FeObject *parts[2];

	FePushGC(context, first);
	FePushGC(context, second);
	parts[0] = first;
	parts[1] = second;
	raise_signal_form(
	    context, "args-out-of-range", FeMakeList(context, parts, 2));
}

/* Raise Emacs' `(end-of-buffer)` or `(beginning-of-buffer)`: the two edges
 * a motion or an edit runs into, both with NO data -- measured on the
 * pinned 31.0.90, `(condition-case e (forward-char 20) (error e))` in a
 * three-character buffer is `(end-of-buffer)` and nothing more.  Both
 * names joined fe's condition table at the Phase 20 pin; before that
 * `signal` refused them and every kg native that reached an edge clamped
 * silently instead. */
[[noreturn]] void lisp_raise_buffer_edge(FeContext *context, bool at_end)
{
	raise_signal_form(context,
	    at_end ? "end-of-buffer" : "beginning-of-buffer", FeNil(context));
}

/* The two type checks a native writes most: `(wrong-type-argument stringp
 * X)` and `(wrong-type-argument symbolp X)`, raised before the accessor
 * that would otherwise produce fe's own "expected string, got integer" --
 * a Fe implementation detail that names neither Emacs' condition nor its
 * data.  kg accepts a string wherever Emacs takes a symbol for a hook or
 * a feature name, so the symbol check accepts both and still reports
 * Emacs' `symbolp` for anything else. */
void lisp_check_string(FeContext *context, FeObject *object)
{
	if (FeGetType(object) != FeTString) {
		lisp_raise_wrong_type(context, "stringp", object);
	}
}

void lisp_check_symbol_or_string(FeContext *context, FeObject *object)
{
	FeType type = FeGetType(object);

	if (type != FeTSymbol && type != FeTString) {
		lisp_raise_wrong_type(context, "symbolp", object);
	}
}

/* Raise one of Emacs' file conditions with Emacs' own data shape
 * (sub-plan 12D Part 2).  The route is raise_signal_form's above and for
 * the same reasons; what is local here is the data, Emacs' three-element
 * (OPERATION STRERROR PATH) list of strings rather than a two-element
 * list, measured on 31.0.90:
 *
 *   (load "/nonexistent-dir/x.el")
 *     (file-missing "Cannot open load file" "No such file or directory"
 *                   "/nonexistent-dir/x.el")
 *
 * SYMBOL is `file-missing' where the file is absent and `file-error'
 * otherwise; Emacs' third leaf, `permission-denied', is out of scope
 * (12A Decision 1: it is measurable only unprivileged, and this box runs
 * the suite as root), so an unreadable file lands on the parent class,
 * which is where a handler naming file-error catches it either way.
 *
 * WHAT THIS WOULD HAVE COST, and what repairs it: fe's `signal' uses the
 * bare condition name as the completion's message (fe_eval.c's PSignal
 * arm passes `symbol' as both the name and the message), so an UNCAUGHT
 * missing-file load would report `file-missing' and nothing else -- the
 * file name gone from every diagnostic.  render_condition() above is the
 * repair, and since Phase 19 it is the general one: Emacs' rendering
 * rule gives a `file-error' subtype its message from the data and princs
 * the rest, so this triple's sentence falls out of the same code every
 * other condition's does.
 *
 * Every allocation can collect, so each intermediate stays on the GC
 * stack until the list is built.  The checkpoint is not restored:
 * raise_signal_form does not return. */
[[noreturn]] void lisp_raise_file_condition(FeContext *context,
    const char *symbol, const char *operation, const char *detail,
    const char *path)
{
	FeObject *parts[3];

	parts[0] = FeMakeString(context, operation);
	FePushGC(context, parts[0]);
	parts[1] = FeMakeString(context, detail);
	FePushGC(context, parts[1]);
	parts[2] = FeMakeString(context, path);
	FePushGC(context, parts[2]);
	raise_signal_form(context, symbol, FeMakeList(context, parts, 3));
}

static void interactive_push_arg(
    FeContext *context, FeObject **args, int *argc, FeObject *value)
{
	if (*argc >= LISP_INTERACTIVE_MAX_ARGS) {
		interactive_error(context, "too many interactive arguments");
	}
	args[(*argc)++] = value;
	FePushGC(context, value);
}

typedef FeObject *(*interactive_reader)(
    FeContext *context, int fd, char code, const char *prompt);

/* The interactive codes take no defaults and no initial input: the whole
 * of what a code says is its prompt.  `f' requires an existing file where
 * `F' does not, and `b' an existing buffer where `B' takes any name --
 * which is the same must_match the public forms' MUSTMATCH argument
 * selects. */
static FeObject *read_interactive_number_code(
    FeContext *context, int fd, char code, const char *prompt)
{
	struct lisp_read_options opt = { nullptr, nullptr, false, "n" };

	(void)code;
	return lisp_read_number_prompt(context, fd, prompt, &opt);
}

static FeObject *read_interactive_string_code(
    FeContext *context, int fd, char code, const char *prompt)
{
	struct lisp_read_options opt = { nullptr, nullptr, false, "s" };

	(void)code;
	return lisp_read_string_prompt(context, fd, prompt, &opt);
}

static FeObject *read_interactive_path_code(
    FeContext *context, int fd, char code, const char *prompt)
{
	struct lisp_read_options opt
	    = { nullptr, nullptr, code == 'f', "path" };

	return lisp_read_path_prompt(context, fd, prompt, &opt);
}

static FeObject *read_interactive_buffer_code(
    FeContext *context, int fd, char code, const char *prompt)
{
	struct lisp_read_options opt
	    = { nullptr, nullptr, code == 'b', "buffer" };

	return lisp_read_buffer_prompt(context, fd, prompt, &opt);
}

static interactive_reader interactive_reader_for(char code)
{
	static const interactive_reader readers[UCHAR_MAX + 1] = {
		['n'] = read_interactive_number_code,
		['s'] = read_interactive_string_code,
		['f'] = read_interactive_path_code,
		['F'] = read_interactive_path_code,
		['b'] = read_interactive_buffer_code,
		['B'] = read_interactive_buffer_code,
		['N'] = read_interactive_number_code,
	};

	return readers[(unsigned char)code];
}

static FeObject *read_interactive_prompt(
    FeContext *context, int fd, char code, const char *prompt, FeObject *raw)
{
	interactive_reader reader;

	if (code == 'N' && raw != NULL && !FeIsNil(raw)) {
		return FeMakeInteger(context, lisp_prefix_number(context, raw));
	}
	lisp_prompt_require(context);
	reader = interactive_reader_for(code);
	return reader(context, fd, code, prompt);
}

typedef int (*interactive_argument_handler)(FeContext *context, int fd,
    FeObject *raw, FeObject **args, int *argc, char code, const char *prompt);

static int interactive_prompt_argument(FeContext *context, int fd,
    FeObject *raw, FeObject **args, int *argc, char code, const char *prompt)
{
	interactive_push_arg(context, args, argc,
	    read_interactive_prompt(context, fd, code, prompt, raw));
	return 0;
}

static int interactive_prefix_argument(FeContext *context, int fd,
    FeObject *raw, FeObject **args, int *argc, char code, const char *prompt)
{
	(void)fd;
	(void)code;
	(void)prompt;
	interactive_push_arg(context, args, argc,
	    FeMakeInteger(context, lisp_prefix_number(context, raw)));
	return 0;
}

static int interactive_raw_argument(FeContext *context, int fd, FeObject *raw,
    FeObject **args, int *argc, char code, const char *prompt)
{
	(void)fd;
	(void)code;
	(void)prompt;
	interactive_push_arg(context, args, argc, raw);
	return 0;
}

static int interactive_region_argument(FeContext *context, int fd,
    FeObject *raw, FeObject **args, int *argc, char code, const char *prompt)
{
	FeObject *empty = FeNil(context);

	(void)fd;
	(void)raw;
	(void)code;
	(void)prompt;
	if (*argc > LISP_INTERACTIVE_MAX_ARGS - 2) {
		interactive_error(context, "too many interactive arguments");
	}
	interactive_push_arg(
	    context, args, argc, native_region_beginning(context, empty));
	interactive_push_arg(
	    context, args, argc, native_region_end(context, empty));
	return 0;
}

static interactive_argument_handler interactive_handler_for(char code)
{
	static const interactive_argument_handler handlers[UCHAR_MAX + 1] = {
		['s'] = interactive_prompt_argument,
		['n'] = interactive_prompt_argument,
		['N'] = interactive_prompt_argument,
		['f'] = interactive_prompt_argument,
		['F'] = interactive_prompt_argument,
		['b'] = interactive_prompt_argument,
		['B'] = interactive_prompt_argument,
		['p'] = interactive_prefix_argument,
		['P'] = interactive_raw_argument,
		['r'] = interactive_region_argument,
	};

	return handlers[(unsigned char)code];
}

static int interactive_add_prefix(FeContext *context, int fd, FeObject *raw,
    FeObject **args, int *argc, char code, const char *prompt)
{
	interactive_argument_handler handler = interactive_handler_for(code);

	if (handler != nullptr) {
		return handler(context, fd, raw, args, argc, code, prompt);
	}
	/* 07A's measured Emacs code set, complete and in order:
	 * a b B c C d D e f F G i k K m M n N p P r R s S U v x X z Z.
	 * n, N, s, f, F, b, B, p, P and r are in the handler table above and
	 * never reach here; what remains is a valid Emacs code kg has not
	 * implemented, which is a different answer from a malformed byte.
	 *
	 * It had `V`, which Emacs has no code for, and lacked n/N/s while
	 * the comment claimed to be the whole set.
	 *
	 * `*`, `@` and `^` are Emacs' interactive *modifiers* rather than
	 * codes -- they lead the spec instead of naming an argument -- and
	 * are deferred, not invalid: 07A Decision 6 and doc/TODO.md both
	 * promise "unsupported" for a valid-but-deferred spelling. */
	if (strchr("abBcCdDefFGikKmMnNpPrRsSUvxXzZ*@^", code) != nullptr) {
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

/* Copy one clause's prompt text -- everything from `from` up to the
 * clause's terminating newline, or to the end of the spec when it is the
 * last clause -- into a bounded buffer. */
static void clause_prompt(
    const char *from, const char *end, char *out, size_t outsize)
{
	size_t length = end != nullptr ? (size_t)(end - from) : strlen(from);

	if (length >= outsize) {
		length = outsize - 1;
	}
	memcpy(out, from, length);
	out[length] = '\0';
}

/* Copy a Fe string into a caller-owned bounded buffer, raising when it does
 * not fit.  The point is that no heap allocation is live afterwards: a
 * prompt, a quit or a validation failure below raises, and an Fe raise
 * longjmps past every free() on this C frame -- which ASan reported as a
 * leak for every command whose spec prompts and is then cancelled.  Same
 * reason copy_command_name() exists in lisp_cmd.c. */
static void copy_bounded(FeContext *context, FeObject *object, char *out,
    size_t outsize, const char *toolong)
{
	size_t length;
	char *text = copy_fe_string(context, object, &length);

	if (length >= outsize) {
		free(text);
		interactive_error(context, toolong);
	}
	memcpy(out, text, length + 1);
	free(text);
}

static int interactive_string_args(
    FeContext *context, int fd, FeObject *spec, FeObject *raw, FeObject **args)
{
	char text[LISP_INTERACTIVE_SPEC_MAX];
	const char *clause;
	const char *next;
	int argc = 0;

	copy_bounded(context, spec, text, sizeof(text),
	    "interactive specification is too long");
	clause = text;
	if (*text == '\0') {
		return 0;
	}
	while (*clause) {
		char prompt[128];

		next = strchr(clause, '\n');
		if (next != nullptr && next == clause) {
			interactive_error(
			    context, "invalid empty interactive clause");
		}
		if (argc >= LISP_INTERACTIVE_MAX_ARGS) {
			interactive_error(
			    context, "too many interactive arguments");
		}
		/* The prompt is this clause's tail and stops at the newline.
		 * Passing `clause + 1` handed the reader the rest of the
		 * whole spec, so (interactive "sFirst: \nsSecond: ") asked
		 * "First: ^JsSecond: " -- the second clause leaking into the
		 * first one's prompt.  Bounded rather than allocated: a
		 * prompt is display text, and 127 bytes is already far wider
		 * than the echo area. */
		clause_prompt(clause + 1, next, prompt, sizeof(prompt));
		if (interactive_add_prefix(
			context, fd, raw, args, &argc, *clause, prompt)
		    != 0) {
			return argc;
		}
		if (next == nullptr) {
			break;
		}
		clause = next + 1;
		if (*clause == '\0') {
			interactive_error(
			    context, "invalid empty interactive clause");
		}
	}
	return argc;
}

static int interactive_form_args(
    FeContext *context, struct lisp_command *cmd, FeObject **args)
{
	FeObject *value;
	FeRoot *value_root;
	int argc = 0;

	value = FeCallWithOptions(context, FeGetRoot(cmd->interactive_root),
	    nullptr, 0, &eval_options);
	value_root = FeCreateRoot(context, value);
	while (!FeIsNil(value)) {
		if (FeGetType(value) != FeTPair) {
			/* The improper tail is what is not a list, and the
			 * condition names it: `(wrong-type-argument listp
			 * VALUE)`, which is fe's own shape for the same
			 * complaint and Emacs' too. */
			FeObject *tail = value;
			FeReleaseRoot(context, value_root);
			lisp_raise_wrong_type(context, "listp", tail);
		}
		if (argc == LISP_INTERACTIVE_MAX_ARGS) {
			FeReleaseRoot(context, value_root);
			interactive_error(
			    context, "too many interactive arguments");
		}
		/* interactive_push_arg roots each element on the GC stack,
		 * which is what has to outlive the root released below:
		 * releasing the list's only root and *then* letting the
		 * command call allocate left every constructed argument
		 * collectable, which is 07D's "root every list element until
		 * FeCallWithOptions has taken the vector". */
		interactive_push_arg(
		    context, args, &argc, FeCar(context, value));
		value = FeCdr(context, value);
	}
	FeReleaseRoot(context, value_root);
	return argc;
}

static int lisp_command_recover(const char *command_name)
{
	/* Drain every binding still live, not only the innermost.  Fe's own
	 * unwind has already run the cleanup of each *nested* binding, so
	 * what is normally left here is the top-level one -- which nothing
	 * released at all once a nested activation had overwritten the
	 * single slot this used to read. */
	while (state.prefix_binding != nullptr) {
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

static struct lisp_prefix_binding *lisp_command_bind_prefix(
    FeObject *symbol, FeRoot *old_root, FeRoot *prefix_root, int nested)
{
	struct lisp_prefix_binding *binding = calloc(1, sizeof(*binding));

	FeSet(state.context, symbol, FeGetRoot(prefix_root));
	if (binding == nullptr) {
		FeReleaseRoot(state.context, old_root);
		FeReleaseRoot(state.context, prefix_root);
		FeHandleError(state.context, "out of memory");
		free(binding);
		return nullptr;
	} else {
		binding->context = state.context;
		binding->symbol = symbol;
		binding->old_root = old_root;
		binding->new_root = prefix_root;
		binding->active = 1;
		binding->outer = state.prefix_binding;
		state.prefix_binding = binding;
		if (nested) {
			FeProtectWithCleanup(
			    state.context, cleanup_prefix_binding, binding);
		}
	}
	return binding;
}

static int lisp_command_arguments(
    struct lisp_command *cmd, int fd, FeObject *prefix_object, FeObject **args)
{
	if (cmd->interactive_kind == LISP_INTERACTIVE_STRING) {
		return interactive_string_args(state.context, fd,
		    FeGetRoot(cmd->interactive_root), prefix_object, args);
	}
	if (cmd->interactive_kind == LISP_INTERACTIVE_FORM) {
		return interactive_form_args(state.context, cmd, args);
	}
	return 0;
}

/* Bind the raw prefix, build the arguments and call: the half of a command
 * activation that is identical whether the command was reached from a key
 * or from a nested (command-execute ...).  Returns the command's value, or
 * nullptr when the binding could not be allocated (which has already
 * reported).  Every exit but that one leaves the binding either restored
 * (top level) or registered as an Fe cleanup (nested). */
static FeObject *lisp_command_activate(
    struct lisp_command *cmd, int fd, int nested)
{
	const struct command_prefix *prefix = cmd_active_prefix();
	FeObject *args[LISP_INTERACTIVE_MAX_ARGS];
	FeObject *prefix_object, *symbol, *form, *quoted, *result;
	FeObject *parts[2];
	FeRoot *prefix_root, *old_root;
	struct lisp_prefix_binding *binding;
	size_t checkpoint = FeSaveGC(state.context);
	int argc;

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
	binding
	    = lisp_command_bind_prefix(symbol, old_root, prefix_root, nested);
	if (binding == nullptr) {
		return nullptr;
	}
	argc = lisp_command_arguments(cmd, fd, prefix_object, args);
	if (nested) {
		/* Transparent to the enclosing condition-case: a plain
		 * FeCallWithOptions raises past the *enclosing* run's
		 * barrier -- kg's own error_jump -- so a handler lexically
		 * around (command-execute …) never saw the condition.  The
		 * protected call returns it instead, and FeResignal puts it
		 * back in flight in the enclosing run with kind, condition
		 * object and message intact.  FeResignal's own cleanup drain
		 * runs the prefix binding's restore exactly once. */
		if (!FeTryCallWithOptions(state.context,
			FeGetRoot(cmd->function_root), args, (size_t)argc,
			&eval_options, &result)) {
			FeResignal(state.context);
		}
		FeRestoreGC(state.context, checkpoint);
		return result;
	}
	result = FeCallWithOptions(state.context, FeGetRoot(cmd->function_root),
	    args, (size_t)argc, &eval_options);
	/* The argument vector's GC-stack roots have done their job; drop them
	 * here so a nested activation, which has no top-level frame restore
	 * behind it, costs a constant number of slots however often it runs.
	 * `result` survives because nothing below allocates -- the same rule
	 * lisp_take_command_value() documents. */
	FeRestoreGC(state.context, checkpoint);
	if (!nested) {
		cleanup_prefix_binding(state.context, binding);
	}
	return result;
}

int kg_lisp_run_command(const char *name, int fd)
{
	char command_name[LISP_COMMAND_NAME_MAX];
	struct lisp_command *cmd;

	if (!state.initialized || name == nullptr) {
		return 1;
	}
	cmd = find_lisp_command(name);
	if (!cmd) {
		return 1;
	}
	if (state.frame_active) {
		/* Nested (command-execute ...): an evaluator is already
		 * running and owns state.frame.  07D says to "build and call
		 * inside that evaluator", and that is the whole of the
		 * nested path -- no second setjmp, no second
		 * lisp_exec_enter/leave, no write to frame_active.
		 *
		 * Installing one anyway is what the review found: the
		 * activation overwrote the single state.frame's checkpoint
		 * and error_jump, then on return set frame_active = false
		 * and called lisp_exec_leave(1) while the outer command was
		 * still running.  The outer command's next buffer operation
		 * then found no execution context and raised "current buffer
		 * is dead", which the error callback turns into abort()
		 * because no frame is armed to catch it.
		 *
		 * A completion raised in here propagates to the enclosing
		 * run, so a surrounding condition-case sees it, exactly as
		 * 07D requires. */
		state.command_value = lisp_command_activate(cmd, fd, 1);
		return 0;
	}

	/* The command may remove or redefine itself before raising, so keep a
	 * stable copy of the registered name for the diagnostic below; a
	 * stack copy needs no cleanup across the longjmp. */
	memcpy(command_name, cmd->name, sizeof(command_name));

	state.error[0] = '\0';
	state.last_error_kind = KG_LISP_ERROR_NONE;
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		return lisp_command_recover(command_name);
	}
	lisp_exec_enter(state.context);
	if (lisp_command_activate(cmd, fd, 0) == nullptr) {
		state.frame_active = false;
		lisp_exec_leave(0);
		return 0;
	}
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	lisp_exec_leave(1);
	return 0;
}

/* The value the most recent command activation produced, cleared by the
 * read.  A bare pointer rather than an FeRoot on purpose: creating a root
 * conses, and the *point* of this slot is that nothing allocates between
 * FeCallWithOptions returning in lisp_command_activate() and
 * native_command handing the value back to the evaluator.  Anything that
 * wants to hold it past that must root it itself. */
FeObject *lisp_take_command_value(FeContext *context)
{
	FeObject *value = state.command_value;

	state.command_value = nullptr;
	return value != nullptr ? value : FeNil(context);
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

/* The value cell has no C accessor by design (see fe/fe.c's symbol
 * accessors: its unit of currency is the binding cell), so a host reads a
 * variable by evaluating the symbol -- which is what
 * lisp_command_activate() already does for `current-prefix-arg`.  The
 * object is evaluated rather than a source string read: interning an
 * existing name and evaluating a symbol both allocate nothing, so a caller
 * asking this question does not disturb the arena.  That is a property, not
 * an optimisation -- test/pty/lisp-exhaustion-mid-init-visible.yaml pins
 * what an arena an init file pinned looks like afterwards, and the reader
 * this used to run gave a slot back and moved the number.
 *
 * `boundp` first: a name nothing ever declared is void, and evaluating it
 * would raise rather than answer nil.  The frame is this file's ordinary
 * one, because the raise that is left -- interning a new symbol into a full
 * arena -- has to land somewhere.  No lisp_exec_enter()/release_frame_
 * buffers(): evaluating a symbol calls no native, opens no load and takes
 * no scratch, so there is nothing for either to do. */
int kg_lisp_variable_non_nil(const char *name)
{
	FeObject *symbol, *value;
	/* Written after the setjmp and read after it returns normally, which
	 * is the shape -Wclobbered is about; kg_lisp_init()'s `in_prelude`
	 * is the same declaration for the same reason. */
	volatile int non_nil = 0;

	if (name == nullptr || !state.initialized || state.frame_active) {
		return 0;
	}
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		lisp_settle_completion();
		return 0;
	}
	symbol = FeMakeSymbol(state.context, name);
	if (FeIsBound(state.context, symbol)) {
		value = FeEvaluateWithOptions(
		    state.context, symbol, &eval_options);
		non_nil = !FeIsNil(value);
	}
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	return non_nil;
}

/* Fe has no host callback on assignment, so C cannot be notified at the
 * exact setq.  Poll at the two seams that need a current answer: before a
 * repaint, and inside a geometry-consuming native.  One symbol lookup and
 * one frame cover every buffer; editor_set_tab_width() makes the unchanged
 * case a comparison and owns all derived-state invalidation. */
static void sync_display_options_in_frame(void)
{
	FeObject *symbol = FeMakeSymbol(state.context, "tab-width");
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		FeObject *value;
		int width = KG_TAB_WIDTH;

		if (!buflist[i].active) {
			continue;
		}
		value = lisp_locals_buffer_value(
		    state.context, symbol, buf_handle(i));
		if (value != nullptr && FeGetType(value) == FeTInteger) {
			int64_t n = FeToInteger(state.context, value);

			if (n >= 1 && n <= KG_TAB_WIDTH_MAX) {
				width = (int)n;
			}
		}
		editor_set_tab_width(&buflist[i], width);
	}
}

void kg_lisp_sync_display_options(void)
{
	if (!state.initialized) {
		return;
	}
	if (state.frame_active) {
		sync_display_options_in_frame();
		return;
	}
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		lisp_settle_completion();
		return;
	}
	sync_display_options_in_frame();
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
}

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

void kg_lisp_run_kill_buffer_hook(struct kg_buffer_handle handle)
{
	(void)handle;
}

void kg_lisp_run_post_command_hook(void) { }

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

int kg_lisp_variable_non_nil(const char *name)
{
	(void)name;
	return 0;
}

void kg_lisp_sync_display_options(void) { }

int kg_lisp_arena_stats(struct kg_lisp_arena_stats *out)
{
	(void)out;
	return 1;
}

void kg_lisp_perf_snapshot(void) { }

#endif /* KG_USE_LISP */

/* Outside both halves: the numeric classifier is pure C with no Fe in it,
 * so it is compiled once and is the same function in a WITH_LISP=0 build.
 *
 * The grammar is deliberately checked *before* strtoimax/strtod rather
 * than after, because those two accept far more than a number prompt
 * should and report it only through a combination of `end` and `errno`
 * that is easy to read wrong -- which is what happened: an empty answer
 * became 0 (and the keystrokes after it fell into the buffer), whitespace
 * alone became 0, and `inf`, `nan` and `0x10` were all accepted, the last
 * as 16.  ascii_is_* rather than <ctype.h> because this grammar is ASCII
 * by definition and kg never calls setlocale(). */
/* Advance over a run of ASCII digits and say whether there was one. */
static bool scan_digits(const char **p)
{
	const char *start = *p;

	while (ascii_is_digit(**p)) {
		(*p)++;
	}
	return *p != start;
}

/* The optional exponent tail, `[eE][+-]?digits`.  -1 when the `e` is
 * there but the digits are not (`1e`, `1e+`), 0 when there is no
 * exponent, 1 when one was consumed.  Split out for the same reason
 * fe.c's own ClassifyExponent is. */
static int scan_exponent(const char **p)
{
	if (**p != 'e' && **p != 'E') {
		return 0;
	}
	(*p)++;
	if (**p == '+' || **p == '-') {
		(*p)++;
	}
	return scan_digits(p) ? 1 : -1;
}

enum kg_number_token kg_number_token_classify(const char *text)
{
	const char *p = text;
	bool integral;
	bool fraction = false;
	int exponent;

	while (ascii_is_space(*p)) {
		p++;
	}
	if (*p == '+' || *p == '-') {
		p++;
	}
	integral = scan_digits(&p);
	if (*p == '.') {
		p++;
		fraction = scan_digits(&p);
	}
	/* `+`, `-`, `.`, `e5` and `.e3` have no digits anywhere. */
	if (!integral && !fraction) {
		return KG_NUMBER_TOKEN_NONE;
	}
	exponent = scan_exponent(&p);
	if (exponent < 0) {
		return KG_NUMBER_TOKEN_NONE; /* `1e`, `1e+` */
	}
	while (ascii_is_space(*p)) {
		p++;
	}
	if (*p != '\0') {
		return KG_NUMBER_TOKEN_NONE; /* `0x10`, `5.x`, `1 2`, `1.2.3` */
	}
	/* `5` and `5.` are integers; `5.0`, `.5` and `1e3` are floats -- an
	 * exponent makes it a float, as in fe. */
	return fraction || exponent > 0 ? KG_NUMBER_TOKEN_FLOAT
					: KG_NUMBER_TOKEN_INTEGER;
}
