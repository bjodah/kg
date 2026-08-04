#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../fe/fe.h"
#include "cmd.h"
#include "keybind.h"
#include "keyevent.h"
#include "keymap.h"
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
 * spells it: pair, nil, double, symbol, string, lambda, macro, primitive
 * or native-fn. */
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

/* Every number is a double, so this is also Emacs' numberp. */
FeObject *native_numberp(FeContext *context, FeObject *arguments)
{
	return lisp_type_is(context, arguments, FeTDouble);
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

/* True for closures, natives and primitives alike; a macro is not a
 * function, as in Emacs. */
FeObject *native_functionp(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeType type;

	FeRequireNoArguments(context, arguments);
	type = FeGetType(object);
	return FeMakeBool(context,
	    type == FeTFn || type == FeTNativeFn || type == FeTPrimitive);
}

[[noreturn]] void command_error(
    FeContext *context, const char *prefix, const char *name)
{
	char message[1024];

	(void)snprintf(message, sizeof(message), "%s: %s", prefix, name);
	FeHandleError(context, message);
}

/* (command-execute COMMAND): COMMAND names a built-in editor command, as a
 * symbol like Emacs or equivalently as a string, since fe reads the text of
 * either.  Which commands may be reached this way, and which of them refuse
 * a read-only buffer, is cmd.c's CMD_LISP_CALLABLE/CMD_EDITS_BUFFER and
 * nothing else; this native only translates the verdict into a Fe error.
 * Explicit empty prefix: a Lisp-invoked command has no keystroke of its
 * own, so it must not inherit whatever prefix arg was left over from the
 * keystroke that triggered this Lisp call (or none at all, e.g. init.el
 * running at startup). */
FeObject *native_command(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct command_context ctx
	    = { STDIN_FILENO, { 0, 0 }, CMD_ORIGIN_LISP };
	char rejected[512];
	char *name;
	size_t length;
	int rc;

	FeRequireNoArguments(context, arguments);
	name = copy_fe_string(context, object, &length);
	(void)length;
	rc = cmd_invoke(name, &ctx);
	(void)snprintf(rejected, sizeof(rejected), "%s", name);
	free(name);
	if (rc == CMD_READ_ONLY) {
		FeHandleError(context, "buffer is read-only");
	}
	if (rc != CMD_RAN) {
		command_error(context, "command is not allowed", rejected);
	}
	return FeNil(context);
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

/* (define-command NAME FN): registers FN as an interactive command
 * visible to M-x and key bindings.  Redefinition releases the previous
 * function's root. */
FeObject *native_define_command(FeContext *context, FeObject *arguments)
{
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	FeObject *fn = FeGetNextArgument(context, &arguments);
	struct lisp_command *cmd;
	char name[LISP_COMMAND_NAME_MAX];
	FeRoot *root;
	size_t i;

	FeRequireNoArguments(context, arguments);
	if (FeGetType(fn) != FeTFn && FeGetType(fn) != FeTNativeFn) {
		FeHandleError(context, "define-command requires a function");
	}
	copy_command_name(context, name_object, name, sizeof(name));
	if (cmd_lookup(name) != nullptr) {
		command_error(
		    context, "cannot redefine built-in command", name);
	}
	cmd = find_lisp_command(name);
	if (!cmd) {
		for (i = 0; i < LISP_MAX_COMMANDS; i++) {
			if (!state.commands[i].name[0]) {
				cmd = &state.commands[i];
				break;
			}
		}
	}
	if (!cmd) {
		FeHandleError(context, "too many Lisp commands");
	}
	/* Create the new root before releasing the old one so a failed
	 * creation leaves the previous definition intact. */
	root = FeCreateRoot(context, fn);
	if (cmd->name[0]) {
		FeReleaseRoot(context, cmd->root);
	}
	strcpy(cmd->name, name);
	cmd->root = root;
	/* The registry hands out the identity; this table only holds the
	 * function.  Redefining keeps the identity, so a command that
	 * repeats itself does not lose its place when its body changes. */
	(void)cmd_runtime_define(name);
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
	FeReleaseRoot(context, cmd->root);
	cmd->name[0] = '\0';
	cmd->root = nullptr;
	/* Frees the identity too: defining this name again is a different
	 * command, and any state the old one owned is dropped. */
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
