/* keybind.c - The key sequences a user may bind, and where they go.
 *
 * The parsing is key_parse()'s; this file is the restricted subset kg
 * accepts from configuration, which is three shapes:
 *
 *   "C-c <key>"   two keys, the first C-c, the second a printable
 *                 character or a control letter other than C-c, C-g or
 *                 C-x -- so a C-c binding can never shadow a prefix, the
 *                 emergency quit, or the key that finishes a git commit.
 *                 C-c is the user's prefix by the Emacs convention, so
 *                 nothing built in has to be checked against.
 *   "<f1>"        one function key, F1 through F12, with any of C-, M-
 *                 and S- on it.  Emacs reserves the early function keys
 *                 for users too, and these are keys kg's own decoder
 *                 produces from one exact byte table, so a binding made
 *                 on one terminal means the same on the next.  Unlike
 *                 C-c, a function key MAY shadow a built-in one (F3 and
 *                 F4 are the macro keys): the newest map in a layer
 *                 answers first, and the user's map is made after the
 *                 built-ins.
 *   "ESC <f1>"    the same key reached through the ESC prefix, which is
 *                 how a terminal keyboard with no Meta sends it.
 *
 * The last two are ONE binding with two spellings, and binding either
 * installs both -- kg's ESC is a keymap prefix, and its decoder hands
 * back ESC and the function key as two events whatever the typing speed,
 * so "M-<f5>" alone would be a binding that never fires on such a
 * keyboard.  src/kbd.c's built-in table spells out the same pair by hand
 * for ESC % / M-%.
 *
 * The bindings themselves live in a keymap of their own, in the global
 * layer: mode maps shadow them, which is what the commit and rebase C-c
 * keys did when this file kept its own table.
 */

#include <stdint.h>
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

/* Whether `key` is one of the twelve function keys, with any modifiers.
 * The bases are contiguous by construction (keyevent.h), and asking that
 * way rather than with a table is what keeps this a range test. */
static int is_function_key(struct key_event key)
{
	return key.base >= KEY_BASE_F1 && key.base <= KEY_BASE_F12;
}

/* "C-c <key>": the shape, and the keys inside it kg keeps for itself. */
static int parse_c_c_sequence(
    const struct key_event *keys, char *out, size_t size)
{
	char second[KEY_FORMAT_MAX];

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

int keybind_parse(const char *sequence, char *out, size_t size)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	struct key_event prefix = { 'c', KEY_MOD_CTRL };
	struct key_event esc = { KEY_BASE_ESC, 0 };
	int count = keymap_parse_sequence(sequence, keys, KEYMAP_SEQUENCE_MAX);

	/* One function key, spelled as itself.  Canonical either way: the
	 * ESC prefix IS Meta, so "ESC <f5>" and "M-<f5>" reduce to the one
	 * binding keybind_bind() then installs under both spellings. */
	if (count == 1 && is_function_key(keys[0])) {
		return key_format(keys[0], out, size);
	}
	if (count == 2 && key_event_equal(keys[0], esc)
	    && is_function_key(keys[1])) {
		struct key_event meta = keys[1];

		meta.mods |= (uint8_t)KEY_MOD_META;
		return key_format(meta, out, size);
	}
	if (count == 2 && key_event_equal(keys[0], prefix)) {
		return parse_c_c_sequence(keys, out, size);
	}
	return 1;
}

/* The ESC-prefix spelling of a Meta function key, written into `out`
 * (KEYMAP_SEQUENCE_FORMAT_MAX is always enough).  Returns 0 when there
 * is one, non-zero for every other key -- which is the caller's signal
 * that this binding has a single spelling. */
static int esc_spelling(const char *canonical, char *out, size_t size)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	char bare[KEY_FORMAT_MAX];

	if (keymap_parse_sequence(canonical, keys, KEYMAP_SEQUENCE_MAX) != 1) {
		return 1;
	}
	if (!is_function_key(keys[0]) || !(keys[0].mods & KEY_MOD_META)) {
		return 1;
	}
	keys[0].mods &= (uint8_t)~KEY_MOD_META;
	if (key_format(keys[0], bare, sizeof(bare)) != 0) {
		return 1;
	}
	(void)snprintf(out, size, "ESC %s", bare);
	return 0;
}

int keybind_bind(const char *sequence, const char *command)
{
	char canonical[KEYMAP_SEQUENCE_FORMAT_MAX];
	char esc[KEYMAP_SEQUENCE_FORMAT_MAX];

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
	/* The second spelling of the same key.  A refusal here leaves the
	 * first one installed rather than rolling it back: half a binding
	 * is a key that works on the keyboards that send the other form,
	 * and none is a key that works nowhere. */
	if (esc_spelling(canonical, esc, sizeof(esc)) == 0
	    && keymap_bind(user_map(), esc, command) != 0) {
		return 2;
	}
	return 0;
}

int keybind_unbind(const char *sequence)
{
	char canonical[KEYMAP_SEQUENCE_FORMAT_MAX];
	char esc[KEYMAP_SEQUENCE_FORMAT_MAX];

	if (keybind_parse(sequence, canonical, sizeof(canonical))) {
		return 1;
	}
	if (esc_spelling(canonical, esc, sizeof(esc)) == 0) {
		(void)keymap_unbind(user_map(), esc);
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
	char canonical[KEYMAP_SEQUENCE_FORMAT_MAX];
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
