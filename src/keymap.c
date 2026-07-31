/* ======================= Layered key sequence maps ======================= */

#include <string.h>

#include "keymap.h"

/* Bounded, and shared: every map's entries live in one pool, so a mode
 * map with three keys costs three entries rather than a whole table.  A
 * bind that would not fit fails and leaves the map as it was. */
static constexpr int keymap_max_maps = 8;
static constexpr int keymap_max_entries = 256;
static constexpr int keymap_name_pool = 2048;

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

struct keymap *keymap_create(const char *name, enum keymap_layer layer)
{
	struct keymap *map;

	if (!name || layer >= KEYMAP_LAYER_COUNT
	    || map_count == keymap_max_maps) {
		return nullptr;
	}
	map = &maps[map_count++];
	map->name = name;
	map->layer = layer;
	map->active = 1;
	return map;
}

const char *keymap_name(const struct keymap *map)
{
	return map ? map->name : nullptr;
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

/* C-g cancels whatever is going on, including a keymap that has been
 * configured into a corner, so no layer may claim it. */
int keymap_is_reserved(const struct key_event *keys, int count)
{
	struct key_event quit = { 'g', KEY_MOD_CTRL };

	return count > 0 && key_event_equal(keys[0], quit);
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
			*found = entry;
			return KEYMAP_COMMAND;
		}
		result = KEYMAP_PREFIX;
	}
	return result;
}

int keymap_bind(struct keymap *map, const char *sequence, const char *command)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	const char *name;
	int count, index, i;

	if (!map || !command || !command[0]) {
		return -1;
	}
	count = keymap_parse_sequence(sequence, keys, KEYMAP_SEQUENCE_MAX);
	if (count < 0 || keymap_is_reserved(keys, count)) {
		return -1;
	}
	index = (int)(map - maps);
	for (i = 0; i < entry_count; i++) {
		struct keymap_entry *entry = &entries[i];
		int shared = entry->count < count ? entry->count : count;

		if (entry->map != index
		    || !keys_equal(entry->keys, keys, shared)) {
			continue;
		}
		/* Same sequence: a rebind.  Otherwise one of the two is a
		 * prefix of the other, and a node cannot be both. */
		if (entry->count != count) {
			return -1;
		}
		name = intern(command);
		if (!name) {
			return -1;
		}
		entry->command = name;
		return 0;
	}
	name = intern(command);
	if (!name || entry_count == keymap_max_entries) {
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
