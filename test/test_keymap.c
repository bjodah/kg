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
#include "../src/dap.h"
#include "../src/def.h"
#include "../src/kbd.h"
#include "../src/keybind.h"
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

static void test_minor_precedence_and_activation_churn(void)
{
	struct keymap *global, *major, *minor, *newer;
	int i;

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

	/* Activation does not change recency: creation order remains the
	 * tiebreaker through repeated session-like on/off churn. */
	for (i = 0; i < 32; i++) {
		keymap_set_active(minor, 1);
		keymap_set_active(newer, 1);
		CHECK(lookup("M-f").map == newer);
		keymap_set_active(newer, 0);
		CHECK(lookup("M-f").map == minor);
		keymap_set_active(minor, 0);
		CHECK(lookup("M-f").map == major);
		keymap_set_active(newer, 1);
		keymap_set_active(minor, 1);
		CHECK(lookup("M-f").map == newer);
	}
	keymap_reset();
}

/* Spelling a sequence back out, which is how every diagnostic and every
 * describe command names a key.  Canonical, and round-trips through the
 * parser. */
static void test_format_sequence(void)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	struct key_event again[KEYMAP_SEQUENCE_MAX];
	char text[KEYMAP_SEQUENCE_FORMAT_MAX];
	static const char *const spellings[] = { "C-x C-s", "M-f", "C-x r C-k",
		"RET", "C-M-s", "S-<home>", "<f3>", "M-%" };
	size_t i;

	for (i = 0; i < sizeof(spellings) / sizeof(*spellings); i++) {
		int count = keymap_parse_sequence(
		    spellings[i], keys, KEYMAP_SEQUENCE_MAX);

		CHECKF(count > 0, "%s does not parse", spellings[i]);
		CHECKF(keymap_format_sequence(keys, count, text, sizeof(text))
			== 0,
		    "%s does not format", spellings[i]);
		CHECKF(strcmp(text, spellings[i]) == 0, "%s formats as %s",
		    spellings[i], text);
		CHECK(keymap_parse_sequence(text, again, KEYMAP_SEQUENCE_MAX)
		    == count);
	}

	/* No room is a refusal, not a truncated spelling that would parse
	 * back as a different, shorter sequence. */
	CHECK(
	    keymap_parse_sequence("C-x r C-k", keys, KEYMAP_SEQUENCE_MAX) == 3);
	CHECK(keymap_format_sequence(keys, 3, text, 4) != 0);
	CHECK(text[0] == '\0');
	CHECK(keymap_format_sequence(keys, 3, NULL, sizeof(text)) != 0);
	/* Nothing to say is not a failure. */
	CHECK(keymap_format_sequence(keys, 0, text, sizeof(text)) == 0);
	CHECK(text[0] == '\0');
}

/* Enumeration: what the describe commands walk, since they have to show
 * what is bound without knowing what to ask for. */
static void test_enumerate_bindings(void)
{
	struct keymap *global, *major;
	struct keymap_binding binding;
	char text[KEYMAP_SEQUENCE_FORMAT_MAX];
	int i, seen_leaf = 0, seen_prefix = 0;

	keymap_reset();
	CHECK(keymap_binding_count() == 0);
	CHECK(keymap_binding_at(0, &binding) != 0);

	global = keymap_create("global", KEYMAP_LAYER_GLOBAL);
	major = keymap_create("major", KEYMAP_LAYER_MAJOR);
	CHECK(keymap_bind(global, "M-f", "forward-word") == 0);
	CHECK(keymap_bind_prefix(global, "C-c") == 0);
	CHECK(keymap_bind(major, "C-c C-k", "kill-compilation") == 0);
	CHECK(keymap_binding_count() == 3);

	for (i = 0; i < keymap_binding_count(); i++) {
		CHECK(keymap_binding_at(i, &binding) == 0);
		CHECK(keymap_format_sequence(
			  binding.keys, binding.count, text, sizeof(text))
		    == 0);
		if (strcmp(text, "M-f") == 0) {
			seen_leaf++;
			CHECK(strcmp(binding.command, "forward-word") == 0);
			CHECK(binding.map == global);
		}
		/* A prefix declared with no leaf enumerates with no
		 * command, which is how it is told from a binding whose
		 * command was removed. */
		if (strcmp(text, "C-c") == 0) {
			seen_prefix++;
			CHECK(binding.command == NULL);
		}
		if (strcmp(text, "C-c C-k") == 0) {
			CHECK(binding.map == major);
			CHECK(binding.count == 2);
		}
	}
	CHECK(seen_leaf == 1);
	CHECK(seen_prefix == 1);

	/* Out of range both ways, and a NULL destination. */
	CHECK(keymap_binding_at(-1, &binding) != 0);
	CHECK(keymap_binding_at(keymap_binding_count(), &binding) != 0);
	CHECK(keymap_binding_at(0, NULL) != 0);

	/* Unbinding takes it out of the enumeration too. */
	CHECK(keymap_unbind(global, "M-f") == 0);
	CHECK(keymap_binding_count() == 2);
	keymap_reset();
}

/* What a winning layer is standing in front of, asked by re-lookup so it
 * reports what dispatch would really do rather than restating the
 * precedence rule. */
static void test_lookup_shadowed(void)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	struct keymap *global, *major;
	struct keymap_match match, shadowed;
	int count;

	keymap_reset();
	global = keymap_create("global", KEYMAP_LAYER_GLOBAL);
	major = keymap_create("major", KEYMAP_LAYER_MAJOR);
	CHECK(keymap_bind(global, "M-p", "previous-line") == 0);
	CHECK(keymap_bind(major, "M-p", "git-rebase-move-line-up") == 0);

	count = keymap_parse_sequence("M-p", keys, KEYMAP_SEQUENCE_MAX);
	CHECK(count == 1);
	keymap_lookup(keys, count, &match);
	CHECK(strcmp(match.command, "git-rebase-move-line-up") == 0);
	CHECK(match.map == major);

	keymap_lookup_shadowed(keys, count, match.map, &shadowed);
	CHECK(shadowed.result == KEYMAP_COMMAND);
	CHECK(strcmp(shadowed.command, "previous-line") == 0);
	CHECK(shadowed.map == global);

	/* Asking again leaves the map exactly as it was: the mode binding
	 * still wins, and the map is still active. */
	CHECK(keymap_is_active(major));
	keymap_lookup(keys, count, &match);
	CHECK(strcmp(match.command, "git-rebase-move-line-up") == 0);

	/* A key only the bottom layer has shadows nothing. */
	CHECK(keymap_bind(global, "M-w", "kill-ring-save") == 0);
	count = keymap_parse_sequence("M-w", keys, KEYMAP_SEQUENCE_MAX);
	keymap_lookup(keys, count, &match);
	CHECK(match.map == global);
	keymap_lookup_shadowed(keys, count, match.map, &shadowed);
	CHECK(shadowed.result == KEYMAP_NO_MATCH);
	/* Nor does an unbound key, whose lookup named no map. */
	keymap_lookup_shadowed(keys, count, NULL, &shadowed);
	CHECK(shadowed.result == KEYMAP_NO_MATCH);
	CHECK(shadowed.command == NULL);

	/* An inactive map is left inactive, not switched on by the
	 * save-and-restore. */
	keymap_set_active(major, 0);
	keymap_lookup_shadowed(keys, count, major, &shadowed);
	CHECK(!keymap_is_active(major));
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
	struct keymap_usage before, after;
	int i, bound = 0, maps = 0;
	char sequence[32];

	keymap_reset();
	while (keymap_create("filler", KEYMAP_LAYER_MINOR)) {
		maps++;
		CHECKF(maps < 1000, "the map table is unbounded");
	}
	/* Bounded above, and bounded below by what one session needs at
	 * once: kbd.c installs ten maps (the global one and nine mode
	 * maps), keybind.c creates an eleventh the first time a user binds
	 * a C-c key, the optional debugger reserves three, and configuration
	 * may define four after that.  The final test below makes the useful
	 * pre-debugger half of that claim against the real maps. */
	CHECKF(maps == 18, "map capacity is %d, expected 18", maps);
	before = keymap_test_usage();
	CHECK(
	    keymap_create("must-not-be-interned", KEYMAP_LAYER_MINOR) == NULL);
	after = keymap_test_usage();
	CHECK(after.maps == before.maps);
	CHECK(after.entries == before.entries);
	CHECK(after.name_bytes == before.name_bytes);
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
	CHECKF(bound == 256, "entry capacity is %d, expected 256", bound);
	/* A refused bind for lack of room still leaves the map usable. */
	CHECK(lookup("a a a").result == KEYMAP_COMMAND);
	before = keymap_test_usage();
	CHECK(keymap_bind(map, "z z z", "must-not-be-interned") != 0);
	after = keymap_test_usage();
	CHECK(after.maps == before.maps);
	CHECK(after.entries == before.entries);
	CHECK(after.name_bytes == before.name_bytes);
	keymap_reset();
}

static void test_name_pool_full_refusals_are_atomic(void)
{
	char command[3070];
	struct keymap_usage full, after;
	struct keymap_match match;
	struct keymap *map;

	keymap_reset();
	map = keymap_create("m", KEYMAP_LAYER_GLOBAL);
	CHECK(map != NULL);
	memset(command, 'a', sizeof(command) - 1);
	command[sizeof(command) - 1] = '\0';
	CHECK(strlen(command) == 3069);
	CHECK(keymap_bind(map, "M-a", command) == 0);

	/* "m" costs two bytes including its terminator, and the unique
	 * command costs 3070: this is the exact name-pool boundary. */
	full = keymap_test_usage();
	CHECK(full.maps == 1);
	CHECK(full.entries == 1);
	CHECK(full.name_bytes == 3072);
	match = lookup("M-a");
	CHECK(match.result == KEYMAP_UNRESOLVED);
	CHECK(strcmp(match.command, command) == 0);

	/* A new entry cannot leak either its entry or its rejected name. */
	CHECK(keymap_bind(map, "M-b", "new-command") != 0);
	after = keymap_test_usage();
	CHECK(after.maps == full.maps);
	CHECK(after.entries == full.entries);
	CHECK(after.name_bytes == full.name_bytes);
	CHECK(lookup("M-b").result == KEYMAP_NO_MATCH);

	/* Nor may a failed exact rebind discard the old leaf. */
	CHECK(keymap_bind(map, "M-a", "replacement-command") != 0);
	after = keymap_test_usage();
	CHECK(after.maps == full.maps);
	CHECK(after.entries == full.entries);
	CHECK(after.name_bytes == full.name_bytes);
	match = lookup("M-a");
	CHECK(match.result == KEYMAP_UNRESOLVED);
	CHECK(strcmp(match.command, command) == 0);
	keymap_reset();
}

/* Every built-in binding has to parse and name a command that exists.
 * The editor installs the global map on its first keystroke, so this
 * asks kbd.c to install it and then reads the result. */
static void test_builtin_global_map_resolves(void)
{
	struct keymap_usage usage;
	static const char *const sequences[]
	    = { "C-f", "C-b", "C-n", "C-p", "C-a", "C-e", "M-f", "M-b", "M-<",
		      "M->", "C-v", "M-v", "C-l", "M-r", "M-g g", "M-g M-g",
		      "M-g n", "M-g p", "<left>", "<right>", "<up>", "<down>",
		      "<home>", "<end>", "<prior>", "<next>", "C-<left>",
		      "C-<right>", "C-<up>", "C-<down>", "C-<home>", "C-<end>",
		      /* The three xref keys.  They are here for a bound the
		       * sample above does not reach: every command name a
		       * binding uses is interned in keymap.c's name pool, and a
		       * name that does not fit makes the bind refuse without a
		       * word -- so a full pool shows up as a key that silently
		       * stopped working, and these are the newest three. */
		      "M-.", "M-?", "M-," };
	size_t i;

	keymap_reset();
	key_install_builtin_maps();
	usage = keymap_test_usage();
	CHECKF(
	    usage.maps == 10, "built-ins use %d maps, expected 10", usage.maps);
	CHECKF(usage.entries == 168, "built-ins use %d entries, expected 168",
	    usage.entries);
	CHECKF(usage.name_bytes == 2165,
	    "built-ins use %d name bytes, expected 2165", usage.name_bytes);
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
	/* M-g alone is a prefix now, not goto-line: every leaf below it is
	 * two keys long, which is what makes the shorter probe report
	 * KEYMAP_PREFIX (see keymap.c's map_probe()) without a bare "M-g"
	 * prefix declaration. */
	CHECK(lookup("M-g").result == KEYMAP_PREFIX);
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

#ifdef KG_USE_DAP
/* dap_init() runs before the init file is read, and what it leaves behind
 * is three maps that are switched OFF: `define-key` creates a map it cannot
 * find at KEYMAP_LAYER_MAJOR (src/lisp_cmd.c), so an init.el that ran first
 * would claim `dap` and `dap-breakpoint` in the wrong layer permanently,
 * and a map that was active from startup would shadow global keys before a
 * session exists.  Both halves of that rule are asserted here, together
 * with the layer each map was created in -- asked by what the layers do,
 * since a map does not report its own.
 *
 * Since subplan 02 the maps arrive with their default table in them
 * (doc/plans/dap/02-ui.md): the gud-style F-keys, the arrows and the four
 * keys of the info panes.  The counts below are what that costs, measured,
 * and they are here because the name pool is historically the tightest of
 * the three keymap bounds -- a table that grew past it would leave the
 * debugger's keys silently unbound. */
static void test_dap_maps_are_created_inactive(void)
{
	static const char *const names[]
	    = { "dap-breakpoint", "dap", "dap-info" };
	struct keymap *info, *session, *breakpoints;
	struct keymap_usage usage;
	size_t i;

	keymap_reset();
	dap_init();
	usage = keymap_test_usage();
	CHECKF(usage.maps == 3, "dap_init() created %d maps, expected 3",
	    usage.maps);
	CHECKF(usage.entries == 22, "dap_init() bound %d keys, expected 22",
	    usage.entries);
	/* The three map names plus every command name they interned. */
	CHECKF(usage.name_bytes == 364,
	    "the debugger's maps cost %d name bytes, expected 364",
	    usage.name_bytes);
	CHECKF(keymap_unresolved_count() == 0,
	    "%d debugger bindings name a command that does not exist",
	    keymap_unresolved_count());
	for (i = 0; i < sizeof(names) / sizeof(*names); i++) {
		struct keymap *map = keymap_find(names[i]);

		CHECKF(map != NULL, "%s was not created", names[i]);
		CHECKF(!keymap_is_active(map), "%s is active before a session",
		    names[i]);
	}

	/* dap-info is the major map of the debugger's own buffers: activate
	 * it and it is the major map in effect. */
	info = keymap_find("dap-info");
	CHECK(keymap_active_major() == NULL);
	keymap_set_active(info, 1);
	CHECK(keymap_active_major() == info);
	CHECK(keymap_bind(info, "M-a", "forward-word") == 0);

	/* The other two are minor, which is what makes them win over the
	 * active major's binding of the same sequence. */
	session = keymap_find("dap");
	breakpoints = keymap_find("dap-breakpoint");
	keymap_set_active(session, 1);
	CHECK(keymap_bind(session, "M-a", "forward-char") == 0);
	CHECK(lookup("M-a").map == session);
	keymap_set_active(session, 0);
	keymap_set_active(breakpoints, 1);
	CHECK(keymap_bind(breakpoints, "M-a", "backward-char") == 0);
	CHECK(lookup("M-a").map == breakpoints);
	keymap_reset();
}
#else
/* The disabled build creates nothing: a map is a claim on key sequences and
 * on the map table, and `kg -V` says -dap.  An init.el binding into a dap
 * map in this build gets what define-key makes for any unknown name, a
 * major map nothing activates. */
static void test_dap_stub_creates_no_maps(void)
{
	struct keymap *fallback;

	keymap_reset();
	dap_init();
	CHECK(keymap_test_usage().maps == 0);
	CHECK(keymap_find("dap") == NULL);
	CHECK(keymap_find("dap-breakpoint") == NULL);
	CHECK(keymap_find("dap-info") == NULL);
	/* What `(define-key "dap" ...)` falls back to here: an ordinary major
	 * map, in the layer src/lisp_cmd.c creates unknown names in.  Nothing
	 * else in this build claims the name or the layer, which is the whole
	 * of why it is harmless. */
	fallback = keymap_create("dap", KEYMAP_LAYER_MAJOR);
	CHECK(fallback != NULL);
	CHECK(keymap_bind(fallback, "M-a", "forward-char") == 0);
	CHECK(keymap_active_major() == fallback);
	CHECK(lookup("M-a").map == fallback);
	keymap_reset();
}
#endif /* KG_USE_DAP */

/* The minor map the headroom case exercises: the real one when the
 * debugger is built -- `dap` is KEYMAP_LAYER_MINOR's first shipped
 * consumer, and dap_init() leaves it switched off -- and a stand-in
 * otherwise. */
static struct keymap *session_minor_map(void)
{
#ifdef KG_USE_DAP
	struct keymap *map = keymap_find("dap");

	keymap_set_active(map, 1);
	return map;
#else
	return keymap_create("session-minor", KEYMAP_LAYER_MINOR);
#endif
}

/* Ten built-ins, the debugger's three, keybind.c's lazy global user map and
 * four configuration maps is exactly the map table; the build without a
 * debugger creates its own minor map instead and stops two short. */
#ifdef KG_USE_DAP
#define KEYMAP_HEADROOM_MAPS 18
#else
#define KEYMAP_HEADROOM_MAPS 16
#endif

/* The ten production maps, the debugger's three, keybind.c's lazy global
 * user map and four configuration maps all coexist, and the four are bound
 * in: capacity exists for user configuration, not merely for kg to start.
 * The case also exercises how a minor prefix meets the user's C-c map.
 *
 * Run last, and deliberately: keybind.c caches its map in a static that
 * keymap_reset() cannot clear, so nothing may bind a C-c key after the
 * reset this ends with. */
static void test_minor_prefix_and_configuration_headroom(void)
{
	static const char *const names[]
	    = { "my-one", "my-two", "my-three", "my-four" };
	static const char *const keys[] = { "M-a", "M-b", "M-c", "M-d" };
	struct keymap *minor, *configured[4];
	struct keymap_match match;
	struct key_event quit[]
	    = { { 'c', KEY_MOD_CTRL }, { 'g', KEY_MOD_CTRL } };
	struct keymap_usage usage;
	size_t i;

	keymap_reset();
	key_install_builtin_maps();
	dap_init();
	CHECKF(
	    keymap_find("xref") != NULL, "the xref mode map was not created");
	CHECKF(keymap_find("diagnostics") != NULL,
	    "the diagnostics mode map was not created");
	CHECKF(keybind_bind("C-c x", "forward-char") == 0,
	    "no room for the user's map");
	match = lookup("C-c x");
	CHECKF(match.result == KEYMAP_COMMAND,
	    "C-c x does not resolve (result %d)", (int)match.result);
	CHECK(match.command && strcmp(match.command, "forward-char") == 0);

	/* A minor prefix may shadow one C-c branch without swallowing a
	 * different branch in the global user map. */
	minor = session_minor_map();
	CHECK(minor != NULL);
	CHECK(keymap_bind(minor, "C-c C-k", "kill-compilation") == 0);
	CHECK(lookup("C-c").result == KEYMAP_PREFIX);
	CHECK(lookup("C-c C-k").map == minor);
	CHECK(lookup("C-c x").map == keymap_find("user"));
	keymap_set_active(minor, 0);
	CHECK(lookup("C-c x").map == keymap_find("user"));
	keymap_set_active(minor, 1);

	/* The emergency key remains unbindable inside a minor prefix. */
	CHECK(keymap_is_reserved(quit, 2));
	CHECK(keymap_bind(minor, "C-c C-g", "forward-char") != 0);
	CHECK(lookup("C-c C-g").result == KEYMAP_NO_MATCH);

	for (i = 0; i < sizeof(configured) / sizeof(*configured); i++) {
		configured[i] = keymap_create(names[i], KEYMAP_LAYER_MAJOR);
		CHECKF(
		    configured[i] != NULL, "no room for user map %s", names[i]);
		CHECKF(keymap_bind(configured[i], keys[i], "forward-char") == 0,
		    "no room for a binding in user map %s", names[i]);
	}
	usage = keymap_test_usage();
	CHECKF(usage.maps == KEYMAP_HEADROOM_MAPS,
	    "built-ins, debugger, user, minor and four maps use %d maps",
	    usage.maps);
	CHECK(keybind_unbind("C-c x") == 0);
	keymap_reset();
}

int main(void)
{
	RUN(test_parse_sequence);
	RUN(test_bind_rebind_and_unbind);
	RUN(test_a_node_is_a_command_or_a_prefix);
	RUN(test_bind_refuses_and_changes_nothing);
	RUN(test_unresolved_names);
	RUN(test_minor_precedence_and_activation_churn);
	RUN(test_format_sequence);
	RUN(test_enumerate_bindings);
	RUN(test_lookup_shadowed);
	RUN(test_layers_advance_together);
	RUN(test_ambiguous_configuration);
	RUN(test_prefix_without_a_leaf);
	RUN(test_storage_is_bounded);
	RUN(test_name_pool_full_refusals_are_atomic);
	RUN(test_builtin_global_map_resolves);
#ifdef KG_USE_DAP
	RUN(test_dap_maps_are_created_inactive);
#else
	RUN(test_dap_stub_creates_no_maps);
#endif
	RUN(test_minor_prefix_and_configuration_headroom);
	return test_summary();
}
