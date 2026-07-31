/* keybind.c - User key bindings on the C-c prefix.
 *
 * What a user may bind, and where it goes.  The parsing is
 * key_parse()'s; this file is the restricted subset kg accepts today:
 * exactly two keys, the first C-c, the second a printable character or a
 * control letter other than C-c, C-g or C-x -- so a user binding can
 * never shadow a prefix, the emergency quit, or the key that finishes a
 * git commit.  C-c is the user's prefix by the Emacs convention, so
 * nothing built in has to be checked against.
 *
 * The bindings themselves live in a keymap of their own, in the global
 * layer: mode maps shadow them, which is what the commit and rebase C-c
 * keys did when this file kept its own table.
 */

#include <stdio.h>
#include <string.h>

#include "keybind.h"

#include "def.h"
#include "keyevent.h"
#include "keymap.h"

/* Created on the first binding.  keymap_reset() is a test facility and
 * invalidates this along with everything else; nothing in the editor
 * calls it. */
static struct keymap *bindings;

static struct keymap *user_map(void)
{
	if (!bindings) {
		bindings = keymap_create("user", KEYMAP_LAYER_GLOBAL);
	}
	return bindings;
}

/* The keys kg refuses to let a user take: the two prefixes, and the
 * emergency quit that no map may bind at all. */
static int second_key_is_reserved(struct key_event key)
{
	static const char *const reserved[] = { "C-c", "C-g", "C-x" };
	char text[KEY_FORMAT_MAX];
	size_t i;

	if (key_format(key, text, sizeof(text)) != 0) {
		return 1;
	}
	for (i = 0; i < sizeof(reserved) / sizeof(*reserved); i++) {
		if (strcmp(reserved[i], text) == 0) {
			return 1;
		}
	}
	return 0;
}

int keybind_parse(const char *sequence, char *out, size_t size)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	struct key_event prefix = { 'c', KEY_MOD_CTRL };
	char second[KEY_FORMAT_MAX];

	if (keymap_parse_sequence(sequence, keys, KEYMAP_SEQUENCE_MAX) != 2) {
		return 1;
	}
	if (!key_event_equal(keys[0], prefix)) {
		return 1;
	}
	/* A printable character, or a control letter.  Not Meta, not a
	 * named key: what the terminal reports for those is not settled
	 * enough to promise a user it stays bound. */
	if (keys[1].mods & ~(unsigned)KEY_MOD_CTRL) {
		return 1;
	}
	if (!ascii_is_print((int)keys[1].base) || keys[1].base == ' ') {
		return 1;
	}
	if (second_key_is_reserved(keys[1])
	    || key_format(keys[1], second, sizeof(second)) != 0) {
		return 1;
	}
	(void)snprintf(out, size, "C-c %s", second);
	return 0;
}

int keybind_bind(const char *sequence, const char *command)
{
	char canonical[KEY_FORMAT_MAX * 2 + 2];

	if (keybind_parse(sequence, canonical, sizeof(canonical))) {
		return 1;
	}
	if (!command || !command[0]) {
		return 1;
	}
	/* Storage is the keymap's shared, bounded pool now, and a bind
	 * that will not fit fails without changing the map. */
	if (keymap_bind(user_map(), canonical, command) != 0) {
		return 2;
	}
	return 0;
}

int keybind_unbind(const char *sequence)
{
	char canonical[KEY_FORMAT_MAX * 2 + 2];

	if (keybind_parse(sequence, canonical, sizeof(canonical))) {
		return 1;
	}
	if (keymap_unbind(user_map(), canonical) != 0) {
		return 2;
	}
	return 0;
}

const char *keybind_lookup(const char *sequence)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	struct keymap_match match;
	char canonical[KEY_FORMAT_MAX * 2 + 2];
	int count;

	if (keybind_parse(sequence, canonical, sizeof(canonical))) {
		return NULL;
	}
	count = keymap_parse_sequence(canonical, keys, KEYMAP_SEQUENCE_MAX);
	keymap_lookup(keys, count, &match);
	if (match.map != user_map()) {
		return NULL;
	}
	return match.command;
}
