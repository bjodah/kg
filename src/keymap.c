/* ======================= Layered key sequence maps ======================= */

#include <stdio.h>
#include <string.h>

#include "keyevent.h"
#include "keymap.h"

/* Bounded, and shared: every map's entries live in one pool, so a mode
 * map with three keys costs three entries rather than a whole table.  A
 * bind that would not fit fails and leaves the map as it was.
 *
 * Fifteen maps: the editor installs eleven of them itself -- one global
 * map, nine mode maps, and the one map keybind.c creates the first time a
 * user binds a C-c key -- and a table with no room past its own built-ins
 * is one where every `(define-key "my-mode-map" ...)` fails to create its
 * map at all, silently, because keymap_create() answering nullptr is how a
 * full table has always been reported.  Fifteen leaves four for
 * configuration; the cost is 15 * sizeof(struct keymap). */
static constexpr int keymap_max_maps = 15;
static constexpr int keymap_max_entries = 256;
/* The name pool holds every map name AND every command name a binding
 * interns, which is what made it the tightest of the three bounds: at 2048
 * the built-in maps used 2035 of it, and the thirteen bytes left over were
 * exactly the "user" and "my-mode" a configuration test creates.  Adding
 * one global binding (M-, -> xref-go-back, 13 bytes) filled it to the byte,
 * and the first map a user's Lisp asked for then failed to be created --
 * silently, because keymap_create() answering nullptr is how a full table
 * has always been reported.  The failure mode is worth spelling out: a
 * command name that does not fit makes keymap_bind() refuse, which leaves
 * that key to the dispatch switch rather than to an error anybody sees.
 * 2560 leaves ~500 bytes, some 30 names, for configuration -- the same
 * reasoning as the twelve maps above, on the axis that ran out first. */
static constexpr int keymap_name_pool = 2560;

struct keymap {
	const char *name;
	enum keymap_layer layer;
	int active;
};

struct keymap_entry {
	int map; /* index into maps[] */
	int count;
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	const char *command; /* interned; never a handler pointer */
};

static struct keymap maps[keymap_max_maps];
static int map_count;
static struct keymap_entry entries[keymap_max_entries];
static int entry_count;
/* Command names are copied here rather than borrowed: a runtime command
 * that is removed frees the storage its name lived in, and a leaf that
 * kept pointing at it would name a command by reading freed memory. */
static char names[keymap_name_pool];
static int names_used;

void keymap_reset(void)
{
	map_count = 0;
	entry_count = 0;
	names_used = 0;
}

static const char *intern(const char *name);

/* The name is interned rather than borrowed, for the same reason command
 * names are (see `names[]`): the built-in maps pass string literals, but a
 * runtime `(define-key "my-mode-map" ...)` passes a stack buffer that is
 * gone the moment the native returns, and a map holding that pointer names
 * itself by reading dead stack. */
struct keymap *keymap_create(const char *name, enum keymap_layer layer)
{
	struct keymap *map;
	const char *owned;

	if (!name || layer >= KEYMAP_LAYER_COUNT
	    || map_count == keymap_max_maps) {
		return nullptr;
	}
	owned = intern(name);
	if (!owned) {
		return nullptr;
	}
	map = &maps[map_count++];
	map->name = owned;
	map->layer = layer;
	map->active = 1;
	return map;
}

const char *keymap_name(const struct keymap *map)
{
	return map ? map->name : nullptr;
}

/* Exact-name lookup: one pass, no aliasing. */
static struct keymap *keymap_find_exact(const char *name, size_t length)
{
	int i;

	for (i = 0; i < map_count; i++) {
		if (strncmp(maps[i].name, name, length) == 0
		    && maps[i].name[length] == '\0') {
			return &maps[i];
		}
	}
	return nullptr;
}

/* Whether `name` (of `length` bytes) ends with `suffix`. */
static bool keymap_name_ends_with(
    const char *name, size_t length, const char *suffix)
{
	size_t suffix_length = strlen(suffix);

	return length > suffix_length
	    && memcmp(name + length - suffix_length, suffix, suffix_length)
	    == 0;
}

/* kg's maps are named "global", "dired", "compilation"; Emacs Lisp spells
 * the same things "global-map" and "dired-mode-map".  Accept both by
 * trying the name as given, then once more with an Emacs suffix removed --
 * the longer suffix first, so "dired-mode-map" reaches "dired" rather than
 * stopping at "dired-mode".  Deliberately iterative: an earlier version
 * recursed on the stripped name, and `keymap_find("global")` with no
 * "global" map created yet recursed on itself forever, which hung
 * test_lisp and any `(define-key "global" ...)` evaluated before the
 * built-in maps exist. */
struct keymap *keymap_find(const char *name)
{
	static const char *const suffixes[] = { "-mode-map", "-map" };
	size_t length;
	struct keymap *map;
	size_t i;

	if (!name) {
		return nullptr;
	}
	length = strlen(name);
	map = keymap_find_exact(name, length);
	if (map) {
		return map;
	}
	for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
		if (keymap_name_ends_with(name, length, suffixes[i])) {
			return keymap_find_exact(
			    name, length - strlen(suffixes[i]));
		}
	}
	return nullptr;
}

void keymap_set_active(struct keymap *map, int active)
{
	if (map) {
		map->active = active ? 1 : 0;
	}
}

int keymap_is_active(const struct keymap *map) { return map && map->active; }

int keymap_parse_sequence(const char *text, struct key_event *out, int max)
{
	int count = 0;

	if (!text || !out) {
		return -1;
	}
	while (*text) {
		char token[KEY_FORMAT_MAX];
		size_t len;

		if (*text == ' ') {
			text++;
			continue;
		}
		len = strcspn(text, " ");
		if (len >= sizeof(token) || count >= max) {
			return -1;
		}
		memcpy(token, text, len);
		token[len] = '\0';
		if (key_parse(token, &out[count]) != 0) {
			return -1;
		}
		count++;
		text += len;
	}
	return count > 0 ? count : -1;
}

int keymap_format_sequence(
    const struct key_event *keys, int count, char *out, size_t size)
{
	size_t used = 0;
	int i;

	if (!out || !size) {
		return -1;
	}
	out[0] = '\0';
	for (i = 0; i < count; i++) {
		char text[KEY_FORMAT_MAX];
		int written;

		if (key_format(keys[i], text, sizeof(text)) != 0) {
			out[0] = '\0';
			return -1;
		}
		written = snprintf(
		    out + used, size - used, "%s%s", used ? " " : "", text);
		if (written < 0 || used + (size_t)written >= size) {
			out[0] = '\0';
			return -1;
		}
		used += (size_t)written;
	}
	return 0;
}

int keymap_binding_count(void) { return entry_count; }

int keymap_binding_at(int index, struct keymap_binding *out)
{
	const struct keymap_entry *entry;

	if (!out || index < 0 || index >= entry_count) {
		return -1;
	}
	entry = &entries[index];
	memcpy(out->keys, entry->keys, sizeof(out->keys));
	out->count = entry->count;
	out->command = entry->command;
	out->map = &maps[entry->map];
	return 0;
}

/* C-g cancels whatever is going on, including a keymap that has been
 * configured into a corner, so no layer may claim it. */
int keymap_is_reserved(const struct key_event *keys, int count)
{
	struct key_event quit = { 'g', KEY_MOD_CTRL };
	int i;

	/* At any depth: prefix traversal cancels on C-g before it looks
	 * anything up, so a binding containing one could never fire. */
	for (i = 0; i < count; i++) {
		if (key_event_equal(keys[i], quit)) {
			return 1;
		}
	}
	return 0;
}

static int keys_equal(
    const struct key_event *a, const struct key_event *b, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		if (!key_event_equal(a[i], b[i])) {
			return 0;
		}
	}
	return 1;
}

static const char *intern(const char *name)
{
	int i;
	size_t len = strlen(name);

	for (i = 0; i < names_used; i += (int)strlen(&names[i]) + 1) {
		if (strcmp(&names[i], name) == 0) {
			return &names[i];
		}
	}
	if (names_used + (int)len + 1 > keymap_name_pool) {
		return nullptr;
	}
	memcpy(&names[names_used], name, len + 1);
	names_used += (int)len + 1;
	return &names[names_used - (int)len - 1];
}

/* What `map` has to say about `count` keys: the entry when the sequence
 * is complete there, PREFIX when a longer binding starts with it. */
static enum keymap_result map_probe(int map, const struct key_event *keys,
    int count, const struct keymap_entry **found)
{
	enum keymap_result result = KEYMAP_NO_MATCH;
	int i;

	for (i = 0; i < entry_count; i++) {
		const struct keymap_entry *entry = &entries[i];

		if (entry->map != map || entry->count < count
		    || !keys_equal(entry->keys, keys, count)) {
			continue;
		}
		if (entry->count == count) {
			/* A prefix declared with no leaf keeps waiting. */
			if (!entry->command) {
				result = KEYMAP_PREFIX;
				continue;
			}
			*found = entry;
			return KEYMAP_COMMAND;
		}
		result = KEYMAP_PREFIX;
	}
	return result;
}

/* What in `map` would conflict with `keys`: the index of the node for
 * exactly this sequence (or -1), and whether anything longer starts with
 * it.  Returns non-zero when a leaf already covers a prefix of `keys`,
 * which no binding may grow children under. */
static int bind_survey(int index, const struct key_event *keys, int count,
    int *exact, int *children)
{
	int i;

	*exact = -1;
	*children = 0;
	for (i = 0; i < entry_count; i++) {
		const struct keymap_entry *entry = &entries[i];
		int shared = entry->count < count ? entry->count : count;

		if (entry->map != index
		    || !keys_equal(entry->keys, keys, shared)) {
			continue;
		}
		if (entry->count == count) {
			*exact = i;
		} else if (entry->count < count) {
			if (entry->command) {
				return -1;
			}
		} else {
			*children = 1;
		}
	}
	return 0;
}

/* The shared half of binding: a leaf with a name, or a prefix with
 * none.  `command` is NULL for the latter. */
static int keymap_bind_name(
    struct keymap *map, const char *sequence, const char *command)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	const char *name;
	int count, index, exact, children;

	if (!map) {
		return -1;
	}
	count = keymap_parse_sequence(sequence, keys, KEYMAP_SEQUENCE_MAX);
	if (count < 0 || keymap_is_reserved(keys, count)) {
		return -1;
	}
	index = (int)(map - maps);
	if (bind_survey(index, keys, count, &exact, &children) != 0) {
		return -1;
	}
	/* A command may not be hung where children already are.  A bare
	 * prefix declaration over either is what C-c is, and is fine. */
	if (command && children) {
		return -1;
	}
	if (exact >= 0) {
		if (!command) {
			return 0; /* already known to be a prefix */
		}
		name = intern(command);
		if (!name) {
			return -1;
		}
		entries[exact].command = name;
		return 0;
	}
	name = command ? intern(command) : nullptr;
	if ((command && !name) || entry_count == keymap_max_entries) {
		return -1;
	}
	entries[entry_count].map = index;
	entries[entry_count].count = count;
	memcpy(
	    entries[entry_count].keys, keys, sizeof(keys[0]) * (size_t)count);
	entries[entry_count].command = name;
	entry_count++;
	return 0;
}

int keymap_bind(struct keymap *map, const char *sequence, const char *command)
{
	if (!command || !command[0]) {
		return -1;
	}
	return keymap_bind_name(map, sequence, command);
}

int keymap_bind_prefix(struct keymap *map, const char *sequence)
{
	return keymap_bind_name(map, sequence, nullptr);
}

int keymap_unbind(struct keymap *map, const char *sequence)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	int count, index, i;

	if (!map) {
		return -1;
	}
	count = keymap_parse_sequence(sequence, keys, KEYMAP_SEQUENCE_MAX);
	if (count < 0) {
		return -1;
	}
	index = (int)(map - maps);
	for (i = 0; i < entry_count; i++) {
		if (entries[i].map != index || entries[i].count != count
		    || !keys_equal(entries[i].keys, keys, count)) {
			continue;
		}
		entries[i] = entries[--entry_count];
		return 0;
	}
	return -1;
}

/* Maps of one layer are searched newest first, and every layer is
 * searched: the first layer with anything to say decides, and two maps
 * of that layer that disagree about being finished report it. */
static enum keymap_result layer_probe(enum keymap_layer layer,
    const struct key_event *keys, int count, const struct keymap_entry **found,
    const struct keymap **winner)
{
	enum keymap_result result = KEYMAP_NO_MATCH;
	int i;

	for (i = map_count - 1; i >= 0; i--) {
		const struct keymap_entry *entry = nullptr;
		enum keymap_result probed;

		if (maps[i].layer != layer || !maps[i].active) {
			continue;
		}
		probed = map_probe(i, keys, count, &entry);
		if (probed == KEYMAP_NO_MATCH) {
			continue;
		}
		if (result == KEYMAP_NO_MATCH) {
			result = probed;
			*found = entry;
			*winner = &maps[i];
		} else if (result != probed) {
			result = KEYMAP_AMBIGUOUS;
		}
	}
	return result;
}

int keymap_unresolved_count(void)
{
	int i, unresolved = 0;

	for (i = 0; i < entry_count; i++) {
		if (entries[i].command
		    && cmd_id_by_name(entries[i].command) == CMD_ID_NONE) {
			unresolved++;
		}
	}
	return unresolved;
}

void keymap_lookup(
    const struct key_event *keys, int count, struct keymap_match *out)
{
	int layer;

	out->result = KEYMAP_NO_MATCH;
	out->command = nullptr;
	out->id = CMD_ID_NONE;
	out->map = nullptr;
	if (!keys || count <= 0 || count > KEYMAP_SEQUENCE_MAX) {
		return;
	}
	for (layer = 0; layer < KEYMAP_LAYER_COUNT; layer++) {
		const struct keymap_entry *entry = nullptr;
		enum keymap_result result;

		result = layer_probe(
		    (enum keymap_layer)layer, keys, count, &entry, &out->map);
		if (result == KEYMAP_NO_MATCH) {
			continue;
		}
		out->result = result;
		if (result != KEYMAP_COMMAND) {
			return;
		}
		out->command = entry->command;
		out->id = cmd_id_by_name(entry->command);
		if (out->id == CMD_ID_NONE) {
			out->result = KEYMAP_UNRESOLVED;
		}
		return;
	}
}

/* What `keys` means in one named map, ignoring layers and whether that
 * map is active.  keymap_lookup() answers "what would the editor do now",
 * which is the wrong question for `(lookup-key MAP KEY)`: that asks what
 * one map says, and must answer the same whether or not the map is
 * currently in effect. */
void keymap_lookup_in(const struct keymap *map, const struct key_event *keys,
    int count, struct keymap_match *out)
{
	const struct keymap_entry *entry = nullptr;

	out->result = KEYMAP_NO_MATCH;
	out->command = nullptr;
	out->id = CMD_ID_NONE;
	out->map = nullptr;
	if (!map || !keys || count <= 0 || count > KEYMAP_SEQUENCE_MAX) {
		return;
	}
	out->result = map_probe((int)(map - maps), keys, count, &entry);
	if (out->result != KEYMAP_COMMAND) {
		return;
	}
	out->map = map;
	out->command = entry->command;
	out->id = cmd_id_by_name(entry->command);
	if (out->id == CMD_ID_NONE) {
		out->result = KEYMAP_UNRESOLVED;
	}
}

/* The major-mode map that is active now, or NULL when none is: the honest
 * answer to `(current-local-map)`, which must name a map that exists
 * rather than a name spelled from the syntax. */
const struct keymap *keymap_active_major(void)
{
	int i;

	for (i = 0; i < map_count; i++) {
		if (maps[i].layer == KEYMAP_LAYER_MAJOR && maps[i].active) {
			return &maps[i];
		}
	}
	return nullptr;
}

/* Deactivating the winner and asking again is the whole implementation:
 * a shadow report that reasoned about layers instead would be a second
 * copy of the precedence rule, and the two would drift. */
void keymap_lookup_shadowed(const struct key_event *keys, int count,
    const struct keymap *hidden, struct keymap_match *out)
{
	struct keymap *map = (struct keymap *)hidden;
	int was_active;

	if (!map) {
		keymap_lookup(nullptr, 0, out);
		return;
	}
	was_active = map->active;
	map->active = 0;
	keymap_lookup(keys, count, out);
	map->active = was_active;
}
