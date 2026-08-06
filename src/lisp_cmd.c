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
	rc = cmd_invoke(name, &ctx);
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

/* (commandp OBJECT): t when OBJECT names something M-x can run.
 *
 * kg's answer is the command registry's, which is the only place the
 * question has an answer here: a built-in is a cmdtable row, and a
 * Lisp-defined command is a `defun` whose body carried an `(interactive
 * …)` declaration.  Emacs asks the *function* whether it has an
 * interactive form, and so also says t for a lambda or a keyboard macro
 * that is not bound to any name; kg has no interactive-form reflection
 * (07D leaves it to a later metadata decision), so only a *name* can be
 * a command here.  That is recorded as a divergence rather than guessed
 * at.  Emacs' optional FOR-CALL-INTERACTIVELY argument is not accepted.
 *
 * Anything that is not a symbol or a string is nil, as in Emacs. */
FeObject *native_commandp(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeType type = FeGetType(object);
	char name[512];

	FeRequireNoArguments(context, arguments);
	if (type != FeTSymbol && type != FeTString) {
		return FeMakeBool(context, false);
	}
	copy_command_name(context, object, name, sizeof(name));
	return FeMakeBool(context,
	    cmd_lookup(name) != nullptr || kg_lisp_command_exists(name) != 0);
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

/* (define-command NAME FN &optional SPEC DOC): the explicit kg extension
 * behind defun's interactive declaration.  All replacement roots are made
 * before the old descriptor is changed, so a bounded-table failure is atomic.
 */
FeObject *native_define_command(FeContext *context, FeObject *arguments)
{
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	FeObject *fn = FeGetNextArgument(context, &arguments);
	FeObject *spec = FeNil(context);
	FeObject *doc = FeNil(context);
	struct lisp_command *cmd;
	char name[LISP_COMMAND_NAME_MAX];
	FeRoot *function_root, *interactive_root, *documentation_root;
	enum lisp_interactive_kind kind = LISP_INTERACTIVE_NONE;
	command_id id;

	if (!FeIsNil(arguments)) {
		spec = FeGetNextArgument(context, &arguments);
	}
	if (!FeIsNil(arguments)) {
		doc = FeGetNextArgument(context, &arguments);
	}
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
	id = cmd_runtime_define(name);
	if (id == CMD_ID_NONE) {
		FeReleaseRoot(context, function_root);
		FeReleaseRoot(context, interactive_root);
		FeReleaseRoot(context, documentation_root);
		FeHandleError(context, "too many Lisp commands");
	}
	if (cmd->name[0]) {
		FeReleaseRoot(context, cmd->function_root);
		FeReleaseRoot(context, cmd->interactive_root);
		FeReleaseRoot(context, cmd->documentation_root);
	}
	strcpy(cmd->name, name);
	cmd->function_root = function_root;
	cmd->interactive_root = interactive_root;
	cmd->documentation_root = documentation_root;
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
	cmd->name[0] = '\0';
	cmd->function_root = nullptr;
	cmd->interactive_root = nullptr;
	cmd->documentation_root = nullptr;
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
	cmd->name[0] = '\0';
	cmd->function_root = nullptr;
	cmd->interactive_root = nullptr;
	cmd->documentation_root = nullptr;
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
