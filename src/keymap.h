#ifndef KG_KEYMAP_H
#define KG_KEYMAP_H

#include "cmd.h"

struct key_event;

/* Key sequences, in layers, resolved to a command name.
 *
 * A leaf holds a command *name*, never a handler pointer and never a
 * command_id: configuration may bind a name before the command exists,
 * and a runtime command that is removed and defined again is a different
 * command with the same name.  The name is resolved at dispatch, so a
 * binding whose command is not defined right now reports that state
 * instead of calling through a stale pointer.
 *
 * Layers are searched in this order, and all of them advance together:
 *
 *     transient > minor (newest first) > major/special > global
 *
 * Advancing together is what keeps a major map's own prefix from
 * swallowing the rest of it: a major map that binds "C-c C-k" makes
 * "C-c" a prefix there, and "C-c u" still reaches the global layer. */

/* Longest sequence a binding may have: "C-x r k" is the longest kg has,
 * and a fourth key leaves room for one more prefix level. */
#define KEYMAP_SEQUENCE_MAX 4

enum keymap_layer {
	KEYMAP_LAYER_TRANSIENT,
	KEYMAP_LAYER_MINOR,
	KEYMAP_LAYER_MAJOR,
	KEYMAP_LAYER_GLOBAL,
	KEYMAP_LAYER_COUNT,
};

enum keymap_result {
	KEYMAP_NO_MATCH,
	/* A longer sequence starting with these keys is bound. */
	KEYMAP_PREFIX,
	KEYMAP_COMMAND,
	/* Bound to a name nothing is called right now: a runtime command
	 * that was removed, or configuration that ran before its
	 * definition.  Not an error, and not a call. */
	KEYMAP_UNRESOLVED,
	/* Two maps in one layer disagree about whether this sequence is
	 * finished.  Recency can pick between two commands, but not
	 * between running one now and waiting for another key. */
	KEYMAP_AMBIGUOUS,
};

struct keymap;

struct keymap_match {
	enum keymap_result result;
	const char *command; /* COMMAND and UNRESOLVED */
	command_id id; /* COMMAND */
	const struct keymap *map; /* the layer that answered */
};

/* Creates a map in `layer`, active from the start.  NULL when the map
 * table is full or `name` is missing. */
[[nodiscard]] struct keymap *keymap_create(
    const char *name, enum keymap_layer layer);
[[nodiscard]] const char *keymap_name(const struct keymap *map);

/* Binds `sequence` ("C-x C-s", "M-f", "C-c C-k") to a command name,
 * replacing any binding of exactly that sequence.  Returns 0, or
 * non-zero and an unchanged map: a sequence that does not parse, one
 * that would make a node both a command and a prefix, a reserved key,
 * or no room left. */
[[nodiscard]] int keymap_bind(
    struct keymap *map, const char *sequence, const char *command);
/* Declares `sequence` a prefix with no leaf of its own, for a key that
 * has to wait for the next one even when nothing under it is bound yet:
 * C-c is a prefix whether or not the user has bound anything to it. */
[[nodiscard]] int keymap_bind_prefix(struct keymap *map, const char *sequence);
[[nodiscard]] int keymap_unbind(struct keymap *map, const char *sequence);
void keymap_set_active(struct keymap *map, int active);
[[nodiscard]] int keymap_is_active(const struct keymap *map);

/* What `keys` means in the layers that are active now. */
void keymap_lookup(
    const struct key_event *keys, int count, struct keymap_match *out);

/* Splits a sequence into keys.  Returns how many, or -1. */
[[nodiscard]] int keymap_parse_sequence(
    const char *text, struct key_event *out, int max);

/* Whether `keys` may not be shadowed: the emergency quit has to work
 * when a map is wrong, so nothing binds it. */
[[nodiscard]] int keymap_is_reserved(const struct key_event *keys, int count);

/* How many bindings name a command that does not exist right now.  The
 * built-in maps must install with none; user and runtime bindings may
 * have some, and report it at dispatch. */
[[nodiscard]] int keymap_unresolved_count(void);

/* Drops every map and binding.  For tests; the editor installs its maps
 * once. */
void keymap_reset(void);

#endif /* KG_KEYMAP_H */
