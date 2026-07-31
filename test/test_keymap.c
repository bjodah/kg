/* test_keymap.c -- key sequences, in layers, resolved to a command name.
 *
 * The properties that make a keymap safe to configure at runtime:
 *
 *  - a leaf holds a name, so a command that does not exist yet, or that
 *    was removed, reports that state instead of calling something;
 *  - a bind that fails leaves the map exactly as it was;
 *  - a node is a command or a prefix, never both;
 *  - every layer advances together, so a major map's own prefix does not
 *    swallow the keys it does not bind;
 *  - storage is bounded and says so.
 *
 * The built-in global map is checked here too: every sequence it
 * declares has to parse and every command it names has to resolve, which
 * is the "built-in declarations must resolve when installed" rule.  That
 * needs the real command table, so this binary links the editor the way
 * test_cmd does. */

#include "../src/cmd.h"
#include "../src/def.h"
#include "../src/kbd.h"
#include "../src/keyevent.h"
#include "../src/keymap.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

static struct keymap_match lookup(const char *sequence)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	struct keymap_match match;
	int count = keymap_parse_sequence(sequence, keys, KEYMAP_SEQUENCE_MAX);

	CHECKF(count > 0, "%s does not parse", sequence);
	keymap_lookup(keys, count > 0 ? count : 0, &match);
	return match;
}

static void test_parse_sequence(void)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];

	CHECK(keymap_parse_sequence("C-x C-s", keys, KEYMAP_SEQUENCE_MAX) == 2);
	CHECK(
	    key_event_equal(keys[0], (struct key_event) { 'x', KEY_MOD_CTRL }));
	CHECK(
	    key_event_equal(keys[1], (struct key_event) { 's', KEY_MOD_CTRL }));
	CHECK(keymap_parse_sequence("M-f", keys, KEYMAP_SEQUENCE_MAX) == 1);
	CHECK(keymap_parse_sequence("C-x r k", keys, KEYMAP_SEQUENCE_MAX) == 3);
	/* Extra spaces are separators, not keys. */
	CHECK(keymap_parse_sequence("  C-x   C-s ", keys, KEYMAP_SEQUENCE_MAX)
	    == 2);
	CHECK(keymap_parse_sequence("", keys, KEYMAP_SEQUENCE_MAX) == -1);
	CHECK(keymap_parse_sequence("   ", keys, KEYMAP_SEQUENCE_MAX) == -1);
	CHECK(keymap_parse_sequence("C-x nonsense", keys, KEYMAP_SEQUENCE_MAX)
	    == -1);
	CHECK(keymap_parse_sequence("a b c d e", keys, KEYMAP_SEQUENCE_MAX)
	    == -1);
	CHECK(keymap_parse_sequence(NULL, keys, KEYMAP_SEQUENCE_MAX) == -1);
}

static void test_bind_rebind_and_unbind(void)
{
	struct keymap *map;

	keymap_reset();
	map = keymap_create("test-global", KEYMAP_LAYER_GLOBAL);
	CHECK(map != NULL);
	CHECK(strcmp(keymap_name(map), "test-global") == 0);

	CHECK(lookup("M-f").result == KEYMAP_NO_MATCH);
	CHECK(keymap_bind(map, "M-f", "forward-word") == 0);
	CHECK(lookup("M-f").result == KEYMAP_COMMAND);
	CHECK(strcmp(lookup("M-f").command, "forward-word") == 0);
	CHECK(lookup("M-f").id == cmd_id_by_name("forward-word"));
	CHECK(lookup("M-f").map == map);

	/* Rebinding replaces, and does not leave the old leaf behind. */
	CHECK(keymap_bind(map, "M-f", "forward-char") == 0);
	CHECK(strcmp(lookup("M-f").command, "forward-char") == 0);

	CHECK(keymap_unbind(map, "M-f") == 0);
	CHECK(lookup("M-f").result == KEYMAP_NO_MATCH);
	CHECK(keymap_unbind(map, "M-f") != 0);
	CHECK(keymap_unbind(map, "not a key") != 0);
	CHECK(keymap_unbind(NULL, "M-f") != 0);
	keymap_reset();
}

static void test_a_node_is_a_command_or_a_prefix(void)
{
	struct keymap *map;

	keymap_reset();
	map = keymap_create("test-global", KEYMAP_LAYER_GLOBAL);
	CHECK(keymap_bind(map, "C-x C-s", "save-buffer") == 0);
	CHECK(lookup("C-x").result == KEYMAP_PREFIX);
	CHECK(lookup("C-x C-s").result == KEYMAP_COMMAND);
	CHECK(lookup("C-x C-f").result == KEYMAP_NO_MATCH);

	/* C-x is already a prefix here, so it cannot also be a command. */
	CHECK(keymap_bind(map, "C-x", "forward-char") != 0);
	CHECK(lookup("C-x").result == KEYMAP_PREFIX);
	CHECK(strcmp(lookup("C-x C-s").command, "save-buffer") == 0);

	/* And the other way round. */
	CHECK(keymap_bind(map, "M-f", "forward-word") == 0);
	CHECK(keymap_bind(map, "M-f M-f", "forward-char") != 0);
	CHECK(lookup("M-f").result == KEYMAP_COMMAND);
	keymap_reset();
}

static void test_bind_refuses_and_changes_nothing(void)
{
	struct keymap *map;

	keymap_reset();
	map = keymap_create("test-global", KEYMAP_LAYER_GLOBAL);
	CHECK(keymap_bind(map, "M-f", "forward-word") == 0);

	CHECK(keymap_bind(map, "not a key", "forward-char") != 0);
	CHECK(keymap_bind(map, "", "forward-char") != 0);
	CHECK(keymap_bind(map, "M-f", "") != 0);
	CHECK(keymap_bind(map, "M-f", NULL) != 0);
	CHECK(keymap_bind(NULL, "M-f", "forward-char") != 0);
	/* The emergency quit is not shadowable, at any depth. */
	CHECK(keymap_is_reserved(&(struct key_event) { 'g', KEY_MOD_CTRL }, 1));
	CHECK(keymap_bind(map, "C-g", "forward-char") != 0);
	CHECK(keymap_bind(map, "C-g x", "forward-char") != 0);
	/* At any depth: traversal cancels on C-g before looking up. */
	CHECK(keymap_bind(map, "C-x C-g", "forward-char") != 0);
	CHECK(keymap_is_reserved((struct key_event[]) { { 'x', KEY_MOD_CTRL },
				     { 'g', KEY_MOD_CTRL } },
	    2));
	CHECK(lookup("C-g").result == KEYMAP_NO_MATCH);

	CHECK(strcmp(lookup("M-f").command, "forward-word") == 0);
	keymap_reset();
}

/* A name is bound, not a command: one that does not exist reports that
 * rather than resolving to whatever is at some slot. */
static void test_unresolved_names(void)
{
	struct keymap *map;
	command_id first;

	keymap_reset();
	map = keymap_create("test-global", KEYMAP_LAYER_GLOBAL);
	CHECK(keymap_bind(map, "M-f", "not-a-command") == 0);
	CHECK(lookup("M-f").result == KEYMAP_UNRESOLVED);
	CHECK(strcmp(lookup("M-f").command, "not-a-command") == 0);
	CHECK(lookup("M-f").id == CMD_ID_NONE);

	/* Defining it makes the same binding resolve. */
	first = cmd_runtime_define("not-a-command");
	CHECK(first != CMD_ID_NONE);
	CHECK(lookup("M-f").result == KEYMAP_COMMAND);
	CHECK(lookup("M-f").id == first);

	/* Removing it makes the binding unresolved again rather than
	 * stale, and defining it again is a different command that the
	 * same binding now names. */
	cmd_runtime_remove("not-a-command");
	CHECK(lookup("M-f").result == KEYMAP_UNRESOLVED);
	CHECK(cmd_runtime_define("not-a-command") != first);
	CHECK(lookup("M-f").id != first);
	cmd_runtime_remove("not-a-command");
	keymap_reset();
}

static void test_precedence(void)
{
	struct keymap *global, *major, *minor, *newer;

	keymap_reset();
	global = keymap_create("global", KEYMAP_LAYER_GLOBAL);
	major = keymap_create("major", KEYMAP_LAYER_MAJOR);
	minor = keymap_create("minor", KEYMAP_LAYER_MINOR);
	newer = keymap_create("newer-minor", KEYMAP_LAYER_MINOR);

	CHECK(keymap_bind(global, "M-f", "forward-word") == 0);
	CHECK(strcmp(lookup("M-f").command, "forward-word") == 0);

	CHECK(keymap_bind(major, "M-f", "forward-char") == 0);
	CHECK(strcmp(lookup("M-f").command, "forward-char") == 0);
	CHECK(lookup("M-f").map == major);

	CHECK(keymap_bind(minor, "M-f", "backward-char") == 0);
	CHECK(strcmp(lookup("M-f").command, "backward-char") == 0);

	/* Newest minor map first. */
	CHECK(keymap_bind(newer, "M-f", "next-line") == 0);
	CHECK(strcmp(lookup("M-f").command, "next-line") == 0);

	/* An inactive map is not consulted, and the next layer answers. */
	keymap_set_active(newer, 0);
	CHECK(!keymap_is_active(newer));
	CHECK(strcmp(lookup("M-f").command, "backward-char") == 0);
	keymap_set_active(minor, 0);
	CHECK(strcmp(lookup("M-f").command, "forward-char") == 0);
	keymap_set_active(major, 0);
	CHECK(strcmp(lookup("M-f").command, "forward-word") == 0);
	keymap_set_active(major, 1);
	CHECK(strcmp(lookup("M-f").command, "forward-char") == 0);
	keymap_reset();
}

/* The case the plan names: a major map that binds C-c C-k makes C-c a
 * prefix there, and C-c u still has to reach the global layer. */
static void test_layers_advance_together(void)
{
	struct keymap *global, *major;

	keymap_reset();
	global = keymap_create("global", KEYMAP_LAYER_GLOBAL);
	major = keymap_create("major", KEYMAP_LAYER_MAJOR);
	CHECK(keymap_bind(major, "C-c C-k", "kill-compilation") == 0);
	CHECK(keymap_bind(global, "C-c u", "forward-word") == 0);

	CHECK(lookup("C-c").result == KEYMAP_PREFIX);
	CHECK(strcmp(lookup("C-c C-k").command, "kill-compilation") == 0);
	CHECK(strcmp(lookup("C-c u").command, "forward-word") == 0);
	CHECK(lookup("C-c u").map == global);
	CHECK(lookup("C-c x").result == KEYMAP_NO_MATCH);
	keymap_reset();
}

/* Recency picks between two commands, but not between running one now
 * and waiting for another key: that is a configuration error, and is
 * reported rather than guessed. */
static void test_ambiguous_configuration(void)
{
	struct keymap *one, *two;

	keymap_reset();
	one = keymap_create("minor-one", KEYMAP_LAYER_MINOR);
	two = keymap_create("minor-two", KEYMAP_LAYER_MINOR);
	CHECK(keymap_bind(one, "C-c", "forward-word") == 0);
	CHECK(keymap_bind(two, "C-c x", "forward-char") == 0);
	CHECK(lookup("C-c").result == KEYMAP_AMBIGUOUS);
	/* The longer sequence is not ambiguous: only one map has it. */
	CHECK(lookup("C-c x").result == KEYMAP_COMMAND);
	/* Disabling one of them settles it. */
	keymap_set_active(two, 0);
	CHECK(lookup("C-c").result == KEYMAP_COMMAND);
	keymap_reset();
}

/* A prefix with no leaf of its own: C-c is a prefix whether or not
 * anything is bound under it, so the key waits rather than reporting
 * itself undefined. */
static void test_prefix_without_a_leaf(void)
{
	struct keymap *map;

	keymap_reset();
	map = keymap_create("test-global", KEYMAP_LAYER_GLOBAL);
	CHECK(keymap_bind_prefix(map, "C-c") == 0);
	CHECK(lookup("C-c").result == KEYMAP_PREFIX);
	CHECK(lookup("C-c x").result == KEYMAP_NO_MATCH);
	/* Declaring it twice is not a conflict, and a leaf may still be
	 * hung under it. */
	CHECK(keymap_bind_prefix(map, "C-c") == 0);
	CHECK(keymap_bind(map, "C-c x", "forward-char") == 0);
	CHECK(lookup("C-c").result == KEYMAP_PREFIX);
	CHECK(strcmp(lookup("C-c x").command, "forward-char") == 0);
	/* But it is still a prefix, so it cannot also become a command. */
	CHECK(keymap_bind(map, "C-c", "forward-word") != 0);
	CHECK(keymap_bind_prefix(map, "C-g") != 0);
	CHECK(keymap_bind_prefix(NULL, "C-c") != 0);
	CHECK(keymap_bind_prefix(map, "not a key") != 0);
	keymap_reset();
}

static void test_storage_is_bounded(void)
{
	struct keymap *map;
	int i, bound = 0, maps = 0;
	char sequence[32];

	keymap_reset();
	while (keymap_create("filler", KEYMAP_LAYER_MINOR)) {
		maps++;
		CHECKF(maps < 1000, "the map table is unbounded");
	}
	keymap_reset();

	map = keymap_create("test-global", KEYMAP_LAYER_GLOBAL);
	for (i = 0; i < 4096; i++) {
		/* 17576 distinct three-key sequences, so a bind that
		 * succeeds is never a rebind of an earlier one. */
		snprintf(sequence, sizeof(sequence), "%c %c %c", 'a' + i % 26,
		    'a' + (i / 26) % 26, 'a' + (i / 676) % 26);
		if (keymap_bind(map, sequence, "forward-char") == 0) {
			bound++;
		}
	}
	CHECKF(bound > 0, "nothing could be bound at all");
	CHECKF(bound < 4096, "the entry table is unbounded");
	/* A refused bind for lack of room still leaves the map usable. */
	CHECK(lookup("a a a").result == KEYMAP_COMMAND);
	keymap_reset();
}

/* Every built-in binding has to parse and name a command that exists.
 * The editor installs the global map on its first keystroke, so this
 * asks kbd.c to install it and then reads the result. */
static void test_builtin_global_map_resolves(void)
{
	static const char *const sequences[] = { "C-f", "C-b", "C-n", "C-p",
		"C-a", "C-e", "M-f", "M-b", "M-<", "M->", "C-v", "M-v", "C-l",
		"M-r", "M-g", "<left>", "<right>", "<up>", "<down>", "<home>",
		"<end>", "<prior>", "<next>", "C-<left>", "C-<right>", "C-<up>",
		"C-<down>", "C-<home>", "C-<end>" };
	size_t i;

	keymap_reset();
	key_install_builtin_maps();
	for (i = 0; i < sizeof(sequences) / sizeof(*sequences); i++) {
		struct keymap_match match = lookup(sequences[i]);

		CHECKF(match.result == KEYMAP_COMMAND,
		    "%s is not a complete binding (result %d)", sequences[i],
		    (int)match.result);
		CHECKF(match.id != CMD_ID_NONE,
		    "%s names %s, which is not a "
		    "command",
		    sequences[i], match.command);
	}
	/* Every binding in every built-in map, including the mode maps that
	 * are not active right now, names a command that exists. */
	CHECKF(keymap_unresolved_count() == 0,
	    "%d built-in bindings name a command that does not exist",
	    keymap_unresolved_count());
	/* Nothing binds the emergency quit or the self-insert range. */
	CHECK(lookup("C-g").result == KEYMAP_NO_MATCH);
	CHECK(lookup("a").result == KEYMAP_NO_MATCH);
	keymap_reset();
}

int main(void)
{
	RUN(test_parse_sequence);
	RUN(test_bind_rebind_and_unbind);
	RUN(test_a_node_is_a_command_or_a_prefix);
	RUN(test_bind_refuses_and_changes_nothing);
	RUN(test_unresolved_names);
	RUN(test_precedence);
	RUN(test_layers_advance_together);
	RUN(test_ambiguous_configuration);
	RUN(test_prefix_without_a_leaf);
	RUN(test_storage_is_bounded);
	RUN(test_builtin_global_map_resolves);
	return test_summary();
}
