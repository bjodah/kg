#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fe/fe.h"
#include "cmd.h"
#include "keybind.h"
#include "keyevent.h"
#include "keymap.h"
#include "lisp.h"
#include "lisp_internal.h"
#include "lisp_obj.h"

/* ---- Types -----------------------------------------------------------
 * Fe's own `atom` only splits pairs from everything else, so the Emacs
 * predicates need the real type tag.  `type_names` and `FeGetType` are
 * both public, so this stays kg-side. */

/* The type name a value answers to: "buffer" or "marker" for an
 * adapter-owned editor object, otherwise the Fe tag's own spelling. */
static const char *lisp_type_name(FeContext *context, FeObject *object)
{
	if (lisp_object_is_buffer(context, object)) {
		return "buffer";
	}
	if (lisp_object_is_marker(context, object)) {
		return "marker";
	}
	return type_names[FeGetType(object)];
}

/* (type-of OBJECT): the type as a symbol, spelled the way Fe's printer
 * spells it -- pair, nil, integer, double, symbol, string, lambda, macro,
 * primitive or native-fn -- or one of the two names lisp_type_name() adds
 * above for an adapter-owned object, buffer and marker.  Only `integer`
 * agrees with Emacs; `double` is Emacs' `float`, `pair` its `cons`, and
 * `nil` its `symbol` (test/lisp-compat/features.json's native-type-of
 * records the gap).  A process wrapper is the one adapter object with no
 * name of its own, so it still answers fe's raw `fex0` tag. */
FeObject *native_type_of(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	const char *name;

	FeRequireNoArguments(context, arguments);
	name = lisp_type_name(context, object);
	return FeMakeSymbol(context, name != nullptr ? name : "unknown");
}

static FeObject *lisp_type_is(
    FeContext *context, FeObject *arguments, FeType type)
{
	FeObject *object = FeGetNextArgument(context, &arguments);

	FeRequireNoArguments(context, arguments);
	return FeMakeBool(context, FeGetType(object) == type);
}

FeObject *native_stringp(FeContext *context, FeObject *arguments)
{
	return lisp_type_is(context, arguments, FeTString);
}

/* A number is an integer or a double, so this is also Emacs' numberp. */
FeObject *native_numberp(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);

	FeRequireNoArguments(context, arguments);
	return FeMakeBool(context,
	    FeGetType(object) == FeTInteger || FeGetType(object) == FeTDouble);
}

FeObject *native_consp(FeContext *context, FeObject *arguments)
{
	return lisp_type_is(context, arguments, FeTPair);
}

/* Emacs' listp: nil or a cons, the two cases `car'/`cdr' accept. */
FeObject *native_listp(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeType type;

	FeRequireNoArguments(context, arguments);
	type = FeGetType(object);
	return FeMakeBool(context, type == FeTNil || type == FeTPair);
}

/* nil is its own type in Fe, but Emacs calls it a symbol and so does this. */
FeObject *native_symbolp(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeType type;

	FeRequireNoArguments(context, arguments);
	type = FeGetType(object);
	return FeMakeBool(context, type == FeTSymbol || type == FeTNil);
}

/* A symbol is a function designator: it resolves through its function cell
 * (lisp_function_designator), so (functionp 'car) is t and a name bound
 * only in the value namespace is not a function.  The verdict on the
 * resolved object is Fe's own FeIsFunction, which is the classification
 * funcall and apply reject an `invalid-function' operand by, so kg's
 * predicate and the interpreter cannot disagree: closures and natives are
 * functions, and so are the primitives whose operands are evaluated before
 * they act, but macros and special forms (`if', `quote', `let') are not --
 * the answer Emacs gives.  Spelling the type check out here instead is what
 * made (functionp 'if) t.  Resolving first is also what keeps a cyclic
 * alias chain from erroring here: fe's host-facing FeGetFunction answers
 * nil for one, and FeIsFunction is then asked about that nil rather than
 * about the symbol -- asking it about the symbol would walk the cycle
 * itself and raise. */
FeObject *native_functionp(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);

	FeRequireNoArguments(context, arguments);
	object = lisp_function_designator(context, object);
	return FeMakeBool(context, FeIsFunction(context, object));
}

[[noreturn]] void command_error(
    FeContext *context, const char *prefix, const char *name)
{
	char message[1024];

	(void)snprintf(message, sizeof(message), "%s: %s", prefix, name);
	FeHandleError(context, message);
}

/* Defined below, beside its other callers. */
static void copy_command_name(
    FeContext *context, FeObject *object, char *out, size_t outsize);

/* The dispatch scopes (command-execute ...) activations have saved, in
 * stable storage because the Fe cleanup that restores one runs after the
 * C frame that pushed it has already been unwound past -- which is the
 * whole reason the cleanup exists.  Bounded by fe's native re-entry limit
 * (32) with headroom; a deeper nest is refused before anything is saved.
 *
 * Cleanups run exactly once and last-in-first-out, so these pop in the
 * order they were pushed and a plain stack is enough. */
#define COMMAND_SCOPE_MAX 64
static struct command_scope scope_stack[COMMAND_SCOPE_MAX];
static unsigned scope_depth;

static void restore_command_scope(FeContext *context, void *data)
{
	(void)context;
	(void)data;
	if (scope_depth == 0) {
		return;
	}
	cmd_scope_restore(scope_stack[--scope_depth]);
}

static void report_command_result(
    FeContext *context, int result, const char *name)
{
	if (result == CMD_READ_ONLY) {
		FeHandleError(context, "buffer is read-only");
	}
	if (result == CMD_NO_TERMINAL) {
		/* The same words lisp_prompt_require() raises, because it is
		 * the same condition: this activation has no descriptor to
		 * prompt on. */
		FeHandleError(
		    context, "interactive prompt is not available here");
	}
	if (result != CMD_RAN) {
		command_error(context, "command is not allowed", name);
	}
}

/* (command-execute COMMAND): COMMAND names a built-in editor command, as a
 * symbol like Emacs or equivalently as a string, since fe reads the text of
 * either.  Which commands may be reached this way, and which of them refuse
 * a read-only buffer, is cmd.c's CMD_LISP_CALLABLE/CMD_EDITS_BUFFER and
 * nothing else; this native only translates the verdict into a Fe error.
 *
 * The prefix is *inherited* from the active command context when there is
 * one, which is Emacs' default for command-execute, and is empty otherwise
 * -- an init file, a hook or a process filter has no keystroke of its own.
 * cmd_active_prefix() answering NULL outside dispatch is what makes that
 * distinction, so editor.current_prefix's stale keystroke value is never
 * read here (07D).
 *
 * The name is copied into a bounded stack buffer before cmd_invoke runs,
 * for the same reason copy_command_name() exists below: cmd_invoke can now
 * reach a Lisp command, and a completion raised inside it longjmps past
 * any free() on this frame.  A heap copy held across it is a leak, which
 * is what the review's ASan run reported. */
FeObject *native_command(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	const struct command_prefix *active = cmd_active_prefix();
	struct command_context ctx = { cmd_prompt_fd(),
		active ? *active : (struct command_prefix) { 0 },
		CMD_ORIGIN_LISP };
	char name[512];
	FeObject *value;
	int rc;

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, object, name, sizeof(name));
	if (scope_depth >= COMMAND_SCOPE_MAX) {
		FeHandleError(context, "command-execute nested too deeply");
	}
	scope_stack[scope_depth++] = cmd_scope_save();
	FeProtectWithCleanup(context, restore_command_scope, nullptr);
	/* A built-in command knows only the window's cursor; the Lisp around
	 * it knows only the runtime point.  Hand the point over before the
	 * command runs and take back where it left the cursor afterwards, so
	 * (goto-char N) (command-execute ...) (insert ...) reads as one
	 * sequence of motions rather than two that ignore each other.  Both
	 * halves are no-ops when the exec buffer is not the one on screen --
	 * a command reached from Lisp acts on the window's buffer, which is
	 * a divergence recorded in doc/lisp-api.md and not one this fixes. */
	kg_lisp_sync_display_options();
	lisp_exec_point_to_window();
	rc = cmd_invoke(name, &ctx);
	lisp_exec_point_from_window(context);
	/* Take the value before anything else: it is unrooted, and only the
	 * absence of allocation between the call and here keeps it alive. */
	value = lisp_take_command_value(context);
	report_command_result(context, rc, name);
	return value;
}

/* The raw `(4)`/`(16)`/`(64)`/… a run of bare C-u produces.
 *
 * `prefix->value` is capped at 1000 for the repeat consumers, so reading it
 * here made C-u C-u C-u C-u C-u `(1000)` where Emacs says `(1024)`: the
 * effective-integer cap had leaked into the raw form, which has none.  The
 * count of presses is what survives the cap, so 4^count is computed from
 * it.  Emacs would use a bignum past int64; the loop stops at the largest
 * representable power of 4 instead, which needs 32 consecutive C-u. */
static int64_t universal_raw_value(const struct command_prefix *prefix)
{
	int64_t value = 4;
	int i;

	for (i = 1; i < prefix->universal_count; i++) {
		if (value > INT64_MAX / 4) {
			return value;
		}
		value *= 4;
	}
	return value;
}

FeObject *lisp_prefix_object(
    FeContext *context, const struct command_prefix *prefix)
{
	FeObject *item;

	if (prefix == nullptr || !prefix->supplied
	    || prefix->raw_kind == PREFIX_RAW_NONE) {
		return FeNil(context);
	}
	if (prefix->raw_kind == PREFIX_RAW_INTEGER) {
		return FeMakeInteger(context, prefix->value);
	}
	if (prefix->raw_kind == PREFIX_RAW_MINUS) {
		return FeMakeSymbol(context, "-");
	}
	item = FeMakeInteger(context, universal_raw_value(prefix));
	return FeMakeList(context, &item, 1);
}

int64_t lisp_prefix_number(FeContext *context, FeObject *object)
{
	int64_t value;

	if (FeIsNil(object)) {
		return 1;
	}
	if (FeGetType(object) == FeTInteger) {
		return FeToInteger(context, object);
	}
	if (FeGetType(object) == FeTSymbol) {
		char text[8];
		(void)FeToString(context, object, text, sizeof(text));
		if (strcmp(text, "-") == 0) {
			return -1;
		}
	}
	if (FeGetType(object) == FeTPair) {
		FeObject *first = FeCar(context, object);
		FeObject *rest = FeCdr(context, object);
		if (FeGetType(first) == FeTInteger && FeIsNil(rest)) {
			value = FeToInteger(context, first);
			return value;
		}
	}
	/* A real condition, not the words: `(condition-case e … )` naming
	 * wrong-type-argument catches this, and the data names the value. */
	lisp_raise_wrong_type(context, "prefix-numeric-value", object);
}

FeObject *native_prefix_numeric_value(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);

	FeRequireNoArguments(context, arguments);
	return FeMakeInteger(context, lisp_prefix_number(context, object));
}

/* Copy a command-name argument into a bounded stack buffer so no heap
 * allocation is live when a later step raises a Fe error. */
static void copy_command_name(
    FeContext *context, FeObject *object, char *out, size_t outsize)
{
	char *name;
	size_t length;

	name = copy_fe_string(context, object, &length);
	if (length == 0 || length >= outsize) {
		free(name);
		FeHandleError(context, "invalid command name");
	}
	memcpy(out, name, length + 1);
	free(name);
}

/* The registry entry whose function cell IS this object, or NULL.
 *
 * Identity, not naming: `(symbol-function 'my-command)` is the very
 * object `define-command` rooted, because `defun`'s expansion binds the
 * closure once and hands the same one to both (07D item 3).  So a
 * function object can be asked the two reflective questions a name can,
 * which is how `commandp` and `interactive-form` stop being purely
 * name-shaped.  A lambda that was never registered is not eq to any of
 * them and answers nil, as it should. */
static struct lisp_command *find_command_by_function(const FeObject *object)
{
	size_t i;

	for (i = 0; i < LISP_MAX_COMMANDS; i++) {
		struct lisp_command *cmd = &state.commands[i];

		if (cmd->name[0] && cmd->function_root != nullptr
		    && FeGetRoot(cmd->function_root) == object) {
			return cmd;
		}
	}
	return nullptr;
}

/* Resolve `commandp`/`interactive-form`'s argument to a Lisp command
 * entry, and say whether a BUILT-IN of that name exists.  A symbol or a
 * string is a name; anything else is asked by identity. */
static struct lisp_command *resolve_command_argument(
    FeContext *context, FeObject *object, bool *builtin)
{
	FeType type = FeGetType(object);
	char name[512];

	*builtin = false;
	if (type != FeTSymbol && type != FeTString) {
		return find_command_by_function(object);
	}
	copy_command_name(context, object, name, sizeof(name));
	*builtin = cmd_lookup(name) != nullptr;
	return find_lisp_command(name);
}

/* (internal--command-names): every command name M-x can reach, as a list
 * of symbols -- the built-in table first, then the Lisp registry.
 *
 * The one enumeration Lisp cannot do for itself.  fe's `env` lists every
 * INTERNED symbol, which covers the primitives, kg's natives and every
 * prelude and user definition, and misses exactly this: a built-in
 * command's name lives in cmdtable, a C array, and is not a symbol
 * anywhere until something writes it.  `apropos` (lisp/help-fns.el) is
 * the caller, and the two lists together are what let it claim to see
 * the whole surface.
 *
 * The list is built back to front so the result is in table order, and
 * each cons is pushed as it is made, since the next `FeMakeSymbol` can
 * collect.  `cmd_name_at` is the M-x picker's own walk (src/cmd.c). */
FeObject *native_command_names(FeContext *context, FeObject *arguments)
{
	FeObject *names = FeNil(context);
	size_t gc = FeSaveGC(context);
	int i;

	FeRequireNoArguments(context, arguments);
	for (i = 0; cmd_name_at(i) != nullptr; i++) {
		/* Counting pass: the list is built backwards below so that
		 * it comes out in the table's own order. */
	}
	while (i-- > 0) {
		names = FeCons(
		    context, FeMakeSymbol(context, cmd_name_at(i)), names);
		/* One slot, not two per name: every allocation pushes its
		 * own result, so the checkpoint is restored each pass and
		 * the chain's head re-pushed -- it roots everything built
		 * so far.  The same idiom fe's own list builders use. */
		FeRestoreGC(context, gc);
		FePushGC(context, names);
	}
	return names;
}

/* (internal--command-documentation NAME): the one-line summary a BUILT-IN
 * command carries in cmdtable, or the docstring a `define-command` was
 * given, or nil.
 *
 * The prelude's `documentation` falls back to this, which is what makes
 * `(documentation 'save-buffer)` answer at all: a built-in has no Lisp
 * definition to have recorded one, and its summary -- the same text M-x
 * and the help screen show -- is the honest answer. */
FeObject *native_command_documentation(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	const struct named_cmd *builtin;
	struct lisp_command *cmd;
	char name[LISP_COMMAND_NAME_MAX];

	FeRequireNoArguments(context, arguments);
	if (FeGetType(object) != FeTSymbol && FeGetType(object) != FeTString) {
		return FeNil(context);
	}
	copy_command_name(context, object, name, sizeof(name));
	builtin = cmd_lookup(name);
	if (builtin != nullptr) {
		return FeMakeString(context, builtin->summary);
	}
	cmd = find_lisp_command(name);
	if (cmd == nullptr || cmd->documentation_root == nullptr) {
		return FeNil(context);
	}
	return FeGetRoot(cmd->documentation_root);
}

/* (commandp OBJECT): t when OBJECT is something M-x can run.
 *
 * Two questions since Phase 19, not one.  A symbol or a string is still
 * asked of the command registry, which is where the answer lives for a
 * name: a built-in is a cmdtable row, and a Lisp-defined command is a
 * `defun` whose body carried an `(interactive …)` declaration.  Anything
 * else is now asked by IDENTITY -- `(commandp (symbol-function
 * 'my-command))` is t, because the object a command's registry entry
 * roots is the very object the name's function cell holds.
 *
 * What still diverges, narrowed to what is left of it: Emacs asks a
 * function whether it CARRIES an interactive form, so an anonymous
 * `(lambda () (interactive) …)` is a command there and is not one here.
 * kg's `interactive` is an inert macro and a lambda carries no metadata,
 * so a function is a command here exactly when it was registered as one.
 * Emacs' optional FOR-CALL-INTERACTIVELY argument is not accepted. */
FeObject *native_commandp(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	bool builtin = false;
	struct lisp_command *cmd;

	FeRequireNoArguments(context, arguments);
	cmd = resolve_command_argument(context, object, &builtin);
	return FeMakeBool(context, builtin || cmd != nullptr);
}

/* (interactive-form COMMAND): the `(interactive …)` declaration COMMAND
 * was defined with, or nil when it is not a command.
 *
 * doc/TODO.md's "interactive reflection" row, and the metadata decision
 * it was waiting on: 07D stored the interactive SPEC as a thunk -- a
 * closure over the descriptor, callable but with nothing to show a
 * reader -- so honest reflection had nothing to return.  Phase 19 stores
 * the raw specification beside it (`define-command`'s fifth argument,
 * which `defun`'s expansion now passes), and this reads it back.
 *
 * Three answers, in the order they are decided:
 *
 *   * a Lisp command that recorded its raw specification answers
 *     `(interactive SPEC)` with the specification as it was written --
 *     the string for a string spec, the descriptor FORM (unevaluated,
 *     not the closure) for a form one;
 *   * a Lisp command with no recorded specification -- a direct
 *     `(define-command 'name fn)`, or `(interactive)` with no code --
 *     answers `(interactive nil)`.  Two elements and not one, because
 *     that is Emacs' own normalization, measured on 31.0.90:
 *     `(defun f () (interactive) 1)` has the interactive form
 *     `(interactive nil)` there;
 *   * a BUILT-IN command answers `(interactive nil)` too, and that is a
 *     true statement about kg rather than a placeholder: a built-in
 *     takes no interactive arguments, because the handlers that need
 *     input read the terminal themselves (CMD_READS_TERMINAL) instead of
 *     declaring a spec for the dispatcher to fill.  Emacs' answer for
 *     one of its own primitives is that primitive's spec string, which
 *     is the recorded divergence `phase19-interactive-form-builtin`.
 *
 * Anything that is not a command answers nil, as in Emacs. */
FeObject *native_interactive_form(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeObject *parts[2];
	bool builtin = false;
	struct lisp_command *cmd;

	FeRequireNoArguments(context, arguments);
	cmd = resolve_command_argument(context, object, &builtin);
	if (cmd == nullptr && !builtin) {
		return FeNil(context);
	}
	parts[0] = FeMakeSymbol(context, "interactive");
	FePushGC(context, parts[0]);
	parts[1] = cmd != nullptr && cmd->interactive_form_root != nullptr
	    ? FeGetRoot(cmd->interactive_form_root)
	    : FeNil(context);
	return FeMakeList(context, parts, 2);
}

static void validate_command_definition(
    FeContext *context, FeObject *fn, FeObject *spec, FeObject *doc)
{
	if (FeGetType(fn) != FeTFn && FeGetType(fn) != FeTNativeFn) {
		FeHandleError(context, "define-command requires a function");
	}
	if (!FeIsNil(spec) && FeGetType(spec) != FeTString
	    && FeGetType(spec) != FeTFn && FeGetType(spec) != FeTNativeFn) {
		FeHandleError(context,
		    "define-command requires a string or function spec");
	}
	if (!FeIsNil(doc) && FeGetType(doc) != FeTString) {
		FeHandleError(
		    context, "define-command documentation requires a string");
	}
}

static struct lisp_command *find_command_slot(const char *name)
{
	struct lisp_command *cmd = find_lisp_command(name);

	if (cmd) {
		return cmd;
	}
	for (size_t i = 0; i < LISP_MAX_COMMANDS; i++) {
		if (!state.commands[i].name[0]) {
			return &state.commands[i];
		}
	}
	return nullptr;
}

/* The next argument if there is one, nil otherwise: `define-command`'s
 * three optional arguments read the same way, and spelling that as three
 * `if`s made the arity the branchiest thing in the function. */
static FeObject *next_optional_argument(
    FeContext *context, FeObject **arguments)
{
	return FeIsNil(*arguments) ? FeNil(context)
				   : FeGetNextArgument(context, arguments);
}

/* (define-command NAME FN &optional SPEC DOC): the explicit kg extension
 * behind defun's interactive declaration.  All replacement roots are made
 * before the old descriptor is changed, so a bounded-table failure is atomic.
 */
FeObject *native_define_command(FeContext *context, FeObject *arguments)
{
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	FeObject *fn = FeGetNextArgument(context, &arguments);
	FeObject *spec, *doc, *raw;
	struct lisp_command *cmd;
	char name[LISP_COMMAND_NAME_MAX];
	FeRoot *function_root, *interactive_root, *documentation_root;
	FeRoot *interactive_form_root;
	enum lisp_interactive_kind kind = LISP_INTERACTIVE_NONE;
	command_id id;

	spec = next_optional_argument(context, &arguments);
	doc = next_optional_argument(context, &arguments);
	/* RAW-SPEC (Phase 19): the specification as it was WRITTEN, which is
	 * the same object as SPEC for a string one and the descriptor form
	 * itself where SPEC is the closure built over it.  Optional, and
	 * unvalidated on purpose -- it is reflection data, and
	 * `interactive-form` shows whatever a definition claimed. */
	raw = next_optional_argument(context, &arguments);
	FeRequireNoArguments(context, arguments);
	validate_command_definition(context, fn, spec, doc);
	copy_command_name(context, name_object, name, sizeof(name));
	if (cmd_lookup(name) != nullptr) {
		command_error(
		    context, "cannot redefine built-in command", name);
	}
	cmd = find_command_slot(name);
	if (!cmd) {
		FeHandleError(context, "too many Lisp commands");
	}
	if (!FeIsNil(spec)) {
		kind = FeGetType(spec) == FeTString ? LISP_INTERACTIVE_STRING
						    : LISP_INTERACTIVE_FORM;
	}
	function_root = FeCreateRoot(context, fn);
	interactive_root = FeCreateRoot(context, spec);
	documentation_root = FeCreateRoot(context, doc);
	interactive_form_root = FeCreateRoot(context, raw);
	id = cmd_runtime_define(name);
	if (id == CMD_ID_NONE) {
		FeReleaseRoot(context, interactive_form_root);
		FeReleaseRoot(context, function_root);
		FeReleaseRoot(context, interactive_root);
		FeReleaseRoot(context, documentation_root);
		FeHandleError(context, "too many Lisp commands");
	}
	if (cmd->name[0]) {
		FeReleaseRoot(context, cmd->function_root);
		FeReleaseRoot(context, cmd->interactive_root);
		FeReleaseRoot(context, cmd->documentation_root);
		FeReleaseRoot(context, cmd->interactive_form_root);
	}
	strcpy(cmd->name, name);
	cmd->function_root = function_root;
	cmd->interactive_root = interactive_root;
	cmd->documentation_root = documentation_root;
	cmd->interactive_form_root = interactive_form_root;
	cmd->interactive_kind = kind;
	/* The registry hands out the identity; redefining keeps it. */
	return FeNil(context);
}

/* (remove-command NAME) */
FeObject *native_remove_command(FeContext *context, FeObject *arguments)
{
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	struct lisp_command *cmd;
	char name[LISP_COMMAND_NAME_MAX];

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, name_object, name, sizeof(name));
	cmd = find_lisp_command(name);
	if (!cmd) {
		command_error(context, "no such Lisp command", name);
	}
	FeReleaseRoot(context, cmd->function_root);
	FeReleaseRoot(context, cmd->interactive_root);
	FeReleaseRoot(context, cmd->documentation_root);
	FeReleaseRoot(context, cmd->interactive_form_root);
	cmd->name[0] = '\0';
	cmd->function_root = nullptr;
	cmd->interactive_root = nullptr;
	cmd->documentation_root = nullptr;
	cmd->interactive_form_root = nullptr;
	cmd->interactive_kind = LISP_INTERACTIVE_NONE;
	/* Frees the identity too: defining this name again is a different
	 * command, and any state the old one owned is dropped. */
	cmd_runtime_remove(name);
	return FeNil(context);
}

FeObject *native_remove_command_if_present(
    FeContext *context, FeObject *arguments)
{
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	struct lisp_command *cmd;
	char name[LISP_COMMAND_NAME_MAX];

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, name_object, name, sizeof(name));
	cmd = find_lisp_command(name);
	if (cmd == nullptr) {
		return FeNil(context);
	}
	FeReleaseRoot(context, cmd->function_root);
	FeReleaseRoot(context, cmd->interactive_root);
	FeReleaseRoot(context, cmd->documentation_root);
	FeReleaseRoot(context, cmd->interactive_form_root);
	cmd->name[0] = '\0';
	cmd->function_root = nullptr;
	cmd->interactive_root = nullptr;
	cmd->documentation_root = nullptr;
	cmd->interactive_form_root = nullptr;
	cmd->interactive_kind = LISP_INTERACTIVE_NONE;
	cmd_runtime_remove(name);
	return FeNil(context);
}

/* (global-set-key SEQUENCE NAME): SEQUENCE must be "C-c <key>"; the name
 * may refer to a static or Lisp command and is resolved at dispatch. */
FeObject *native_bind_key(FeContext *context, FeObject *arguments)
{
	FeObject *seq_object = FeGetNextArgument(context, &arguments);
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	char sequence[64];
	char name[LISP_COMMAND_NAME_MAX];
	int rc;

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, seq_object, sequence, sizeof(sequence));
	copy_command_name(context, name_object, name, sizeof(name));
	rc = keybind_bind(sequence, name);
	if (rc == 1) {
		command_error(context,
		    "invalid key sequence (only \"C-c <key>\" is bindable)",
		    sequence);
	}
	if (rc != 0) {
		FeHandleError(context, "key binding table is full");
	}
	return FeNil(context);
}

/* (global-unset-key SEQUENCE) */
FeObject *native_unbind_key(FeContext *context, FeObject *arguments)
{
	FeObject *seq_object = FeGetNextArgument(context, &arguments);
	char sequence[64];
	int rc;

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, seq_object, sequence, sizeof(sequence));
	rc = keybind_unbind(sequence);
	if (rc == 1) {
		command_error(context,
		    "invalid key sequence (only \"C-c <key>\" is bindable)",
		    sequence);
	}
	if (rc != 0) {
		command_error(context, "key is not bound", sequence);
	}
	return FeNil(context);
}

FeObject *native_define_key(FeContext *context, FeObject *arguments)
{
	FeObject *map_obj = FeGetNextArgument(context, &arguments);
	FeObject *key_obj = FeGetNextArgument(context, &arguments);
	FeObject *cmd_obj = FeGetNextArgument(context, &arguments);
	char map_name[64];
	char sequence[64];
	char command[LISP_COMMAND_NAME_MAX];
	struct keymap *map;
	int rc;

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, map_obj, map_name, sizeof(map_name));
	copy_command_name(context, key_obj, sequence, sizeof(sequence));

	map = keymap_find(map_name);
	if (map == NULL) {
		map = keymap_create(map_name, KEYMAP_LAYER_MAJOR);
		if (map == NULL) {
			FeHandleError(context, "keymap table full");
		}
	}

	if (FeIsNil(cmd_obj)) {
		(void)keymap_unbind(map, sequence);
		return FeNil(context);
	}

	copy_command_name(context, cmd_obj, command, sizeof(command));
	rc = keymap_bind(map, sequence, command);
	if (rc != 0) {
		FeHandleError(context, "cannot bind key sequence");
	}
	return FeNil(context);
}

FeObject *native_lookup_key(FeContext *context, FeObject *arguments)
{
	FeObject *map_obj = FeGetNextArgument(context, &arguments);
	FeObject *key_obj = FeGetNextArgument(context, &arguments);
	char map_name[64];
	char sequence[64];
	struct keymap *map;
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	int count;
	struct keymap_match match;

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, map_obj, map_name, sizeof(map_name));
	copy_command_name(context, key_obj, sequence, sizeof(sequence));

	map = keymap_find(map_name);
	if (map == NULL) {
		return FeNil(context);
	}

	count = keymap_parse_sequence(sequence, keys, KEYMAP_SEQUENCE_MAX);
	if (count <= 0) {
		return FeNil(context);
	}

	/* The named map alone, not the active layers: (lookup-key MAP KEY)
	 * asks what MAP says, and must answer the same whether or not MAP
	 * happens to be in effect. */
	keymap_lookup_in(map, keys, count, &match);
	/* UNRESOLVED too: the sequence *is* bound, to a name no command
	 * table entry answers to yet.  Emacs' lookup-key reports the symbol
	 * a key is bound to without asking whether it is defined, and
	 * hiding the binding here would make a typo look like no binding. */
	if ((match.result == KEYMAP_COMMAND
		|| match.result == KEYMAP_UNRESOLVED)
	    && match.command != NULL) {
		return FeMakeSymbol(context, match.command);
	}
	return FeNil(context);
}

/* (current-local-map): the active major-mode map, named the way kg names
 * it, or nil when no mode map is in effect.  It reports a map that
 * exists -- spelling a name from the buffer's syntax instead would
 * usually name no map at all, and (define-key (current-local-map) ...)
 * would then quietly create a map nothing consults. */
FeObject *native_current_local_map(FeContext *context, FeObject *arguments)
{
	const struct keymap *map = keymap_active_major();
	const char *name;

	FeRequireNoArguments(context, arguments);
	if (map == NULL) {
		return FeNil(context);
	}
	name = keymap_name(map);
	return name ? FeMakeSymbol(context, name) : FeNil(context);
}

/* ---- Phase 2 prelude natives ------------------------------------------
 *
 * doc/plans/2026-08-14-embedded-prelude.md Phase 0.2 found these ten
 * names -- `internal--let', `progn', `internal--doc-put',
 * `internal--variable-doc-put', `nconc', `internal--bind-name',
 * `internal--bind-value', `reverse', `listp' and `null' -- called on
 * every startup path, so lisp/prelude.el could never defer them.  The
 * first three are already `(defalias NAME (symbol-function PRIM))`
 * captures of an fe primitive rather than allocated lambdas and stay
 * that way (`internal--let'/`progn' capture the raw special forms
 * `let'/`do', which a native -- an ordinary evaluated-argument function
 * -- cannot stand in for without breaking them, the same trap
 * `utils/prelude_first_call_census.py' hit wrapping them for the
 * Phase 0.2 census; `null' is already a zero-cost alias to `not').
 * `listp' moved above, next to the other type predicates it belongs
 * with; these six are the rest, moved together because they are what
 * `lisp/prelude.el's own `let'/`let*' bootstrap through, and are one
 * more reason nothing here may assume a Lisp environment: they run
 * before evaluate_prelude() has defined `let' itself for anything later
 * to shadow. */

/* `FeCall'/`FeCallWithOptions' only accept a resolved `FeTFn' or
 * `FeTNativeFn' as `callable' (fe/fe_run.c's `FeCall' rejects anything
 * else with "tried to call non-callable value"), so a raw fe PRIMITIVE
 * such as `setcdr' or `put' cannot be invoked through them directly.
 * `FeEvaluateWithOptions' has no such restriction -- it runs a form
 * through the ordinary evaluator, primitive dispatch included, exactly
 * as evaluating `(put NAME 'variable-documentation DOC)' at prelude load
 * already does -- so this builds `(PRIM 'ARG...)' (each argument quoted,
 * since it is already an evaluated object rather than source to
 * re-evaluate) and evaluates that directly, shared by `native_nconc'
 * (`setcdr') and `native_internal_variable_doc_put' (`put') below. */
static FeObject *lisp_call_primitive(FeContext *context, const char *name,
    FeObject *const *arguments, size_t count)
{
	const size_t gc = FeSaveGC(context);
	FeObject *quote = FeMakeSymbol(context, "quote");
	FeObject *form = FeNil(context);
	FeObject *result;
	size_t i;

	for (i = count; i > 0; i--) {
		FeObject *quoted = FeCons(context, quote,
		    FeCons(context, arguments[i - 1], FeNil(context)));
		form = FeCons(context, quoted, form);
		FePushGC(context, form);
	}
	form = FeCons(context, FeMakeSymbol(context, name), form);
	FePushGC(context, form);
	result = FeEvaluateWithOptions(context, form, &eval_options);
	FeRestoreGC(context, gc);
	return result;
}

/* Emacs' reverse: a fresh list, not a permutation of the original spine,
 * as (nreverse below) is.  Iterative on the spine per lisp/prelude.el's
 * own recursion rule, which this native keeps even though nothing forces
 * it to. */
FeObject *native_reverse(FeContext *context, FeObject *arguments)
{
	FeObject *list = FeGetNextArgument(context, &arguments);
	FeObject *result = FeNil(context);
	const size_t gc = FeSaveGC(context);

	FeRequireNoArguments(context, arguments);
	while (!FeIsNil(list)) {
		result = FeCons(context, FeCar(context, list), result);
		/* One slot, not one per element: every allocation pushes its
		 * own result (MakeObject()'s own FePushGC), so the checkpoint
		 * is restored each pass and the chain's head re-pushed -- it
		 * roots everything built so far.  The same idiom
		 * native_command_names above uses.  `list' itself needs no
		 * push of its own: it is a suffix of the caller's own
		 * argument, rooted below `gc' by whatever rooted the call. */
		FeRestoreGC(context, gc);
		FePushGC(context, result);
		list = FeCdr(context, list);
	}
	return result;
}

/* Emacs' nconc: destructive, and every argument but the last has to be a
 * list.  fe.h exposes no C-level `setcdr' (only `FeCar'/`FeCdr' read a
 * pair), so the splice goes through `lisp_call_primitive' above. */
FeObject *native_nconc(FeContext *context, FeObject *arguments)
{
	FeObject *result = FeNil(context);
	FeObject *tail = FeNil(context);

	while (!FeIsNil(arguments)) {
		FeObject *piece = FeCar(context, arguments);
		FeObject *more = FeCdr(context, arguments);
		FeType piece_type = FeGetType(piece);

		if (!FeIsNil(more) && piece_type != FeTPair
		    && piece_type != FeTNil) {
			lisp_raise_wrong_type(context, "listp", piece);
		}
		if (!FeIsNil(piece)) {
			if (FeIsNil(tail)) {
				result = piece;
			} else {
				FeObject *setcdr_args[2] = { tail, piece };
				(void)lisp_call_primitive(
				    context, "setcdr", setcdr_args, 2);
			}
			if (piece_type == FeTPair) {
				tail = piece;
				while (FeGetType(FeCdr(context, tail))
				    == FeTPair) {
					tail = FeCdr(context, tail);
				}
			}
		}
		arguments = more;
	}
	return result;
}

/* Whether a would-be `let'/`let*' binding name is one of Emacs' three
 * constants: `t', `nil' or a keyword.  A keyword is a symbol whose own
 * name starts with `:' (fe/fe.c's IsKeywordName, which fe.h does not
 * expose -- fe_internal.h is private to fe/, so this reads the name back
 * the same way copy_fe_string's other callers already do rather than
 * reaching for it). */
static bool lisp_is_constant_binding_name(FeContext *context, FeObject *name)
{
	char *text;
	size_t length;
	bool is_keyword;

	if (FeIsNil(name)) {
		return true;
	}
	if (FeGetType(name) != FeTSymbol) {
		return false;
	}
	if (name == FeMakeSymbol(context, "t")) {
		return true;
	}
	text = copy_fe_string(context, name, &length);
	is_keyword = length > 0 && text[0] == ':';
	free(text);
	return is_keyword;
}

/* `let'/`let*' normalise a binding list before handing it to fe's raw
 * `let' primitive; this is the NAME half of a binding element, `B' or
 * `(car B)', with Emacs' constant check. */
FeObject *native_internal_bind_name(FeContext *context, FeObject *arguments)
{
	FeObject *binding = FeGetNextArgument(context, &arguments);
	FeObject *name;

	FeRequireNoArguments(context, arguments);
	name
	    = FeGetType(binding) == FeTPair ? FeCar(context, binding) : binding;
	if (lisp_is_constant_binding_name(context, name)) {
		lisp_raise_setting_constant(context, name);
	}
	return name;
}

/* The VALUE half of a binding element: nil for a bare name, `(cadr B)'
 * for `(NAME VALUE ...)' -- extra elements, same as Emacs, are ignored. */
FeObject *native_internal_bind_value(FeContext *context, FeObject *arguments)
{
	FeObject *binding = FeGetNextArgument(context, &arguments);

	FeRequireNoArguments(context, arguments);
	if (FeGetType(binding) != FeTPair) {
		return FeNil(context);
	}
	return FeCar(context, FeCdr(context, binding));
}

/* `defun'/`defmacro's docstring registry: `(NAME . DOC)' consed onto the
 * global `internal--docs' alist `documentation' (lisp/prelude.el, still
 * Lisp) reads back.  `FeGetValue' returns `nullptr' -- not nil -- for an
 * UNBOUND symbol, which `internal--docs' never is by the time this can be
 * called: `(setq internal--docs nil)' (lisp/prelude.el line 576) is
 * eager and runs during `evaluate_prelude()', strictly before `defun'
 * and `defmacro' (lines 621, 678) are even defined, and this native's
 * only callers are those two macros' own expansions or a direct call
 * from Lisp evaluated after `kg_lisp_init()' has returned successfully
 * -- both postdate line 576 unconditionally.  The `nullptr' guard below
 * is therefore not reachable, kept only because `FeSet' cannot be handed
 * a null second argument and nothing here should assume a private
 * ordering fact about a global variable's own file without saying so. */
FeObject *native_internal_doc_put(FeContext *context, FeObject *arguments)
{
	FeObject *name = FeGetNextArgument(context, &arguments);
	FeObject *doc = FeGetNextArgument(context, &arguments);
	FeObject *docs_symbol = FeMakeSymbol(context, "internal--docs");
	FeObject *current = FeGetValue(context, docs_symbol);

	FeRequireNoArguments(context, arguments);
	FeSet(context, docs_symbol,
	    FeCons(context, FeCons(context, name, doc),
		current ? current : FeNil(context)));
	return doc;
}

/* `defvar'/`defconst's docstring: Emacs stores it under the
 * `variable-documentation' property, which `documentation-property'
 * (lisp/prelude.el, still Lisp) reads back.  fe.h exposes no C-level
 * property-list writer (only the `put'/`get' primitives have one), so
 * this goes through `lisp_call_primitive' above, the same indirection
 * `native_nconc' uses for `setcdr'. */
FeObject *native_internal_variable_doc_put(
    FeContext *context, FeObject *arguments)
{
	FeObject *name = FeGetNextArgument(context, &arguments);
	FeObject *doc = FeGetNextArgument(context, &arguments);

	FeRequireNoArguments(context, arguments);
	if (!FeIsNil(doc)) {
		FeObject *put_args[3] = { name,
			FeMakeSymbol(context, "variable-documentation"), doc };

		(void)lisp_call_primitive(context, "put", put_args, 3);
	}
	return doc;
}
