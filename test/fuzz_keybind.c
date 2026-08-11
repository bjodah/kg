/* fuzz_keybind.c -- arbitrary strings through the user key-binding seam.
 *
 * keybind.c is the one canonical parser for the sequences a user may
 * bind, and its input arrives from init.el and M-x -- user-authored
 * strings with no shape guaranteed.  The input is cut at NULs into
 * strings; each one's first byte picks an operation on the rest:
 * keybind_parse(), bind, unbind, lookup, or the underlying key_parse().
 *
 * Oracles, beyond ASan on the parsing itself:
 *  - a sequence keybind_parse() accepts must canonicalise to a spelling
 *    it accepts again, and to the same spelling (idempotence);
 *  - after a successful bind, lookup on the same sequence returns the
 *    command; after a successful unbind, it returns NULL;
 *  - a key key_parse() accepts must survive key_format() and parse back
 *    to the same event.
 *
 * The keymap's storage is a bounded static pool and is deliberately
 * never reset: keybind.c caches its map in a static that keymap_reset()
 * would invalidate (see test_keymap.c, which runs its keybind case last
 * for the same reason).  A full pool makes bind fail with 2, which the
 * oracles treat as an answer, not an error. */

#include "../src/def.h"
#include "../src/keybind.h"
#include "../src/keyevent.h"
#include "../src/keymap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cmd.h"

/* keymap.c resolves a bound command's name at lookup time; this target
 * links no command table, and what matters here is that the name text
 * survives, not what it dispatches to. */
command_id cmd_id_by_name(const char *name)
{
	return name && name[0] ? CMD_ID_STATIC_BASE : CMD_ID_NONE;
}

#define FUZZ_KEYBIND_MAX_STR 256

static void op_parse(const char *s)
{
	char canon[KEY_FORMAT_MAX * 2 + 2];
	char again[KEY_FORMAT_MAX * 2 + 2];

	if (keybind_parse(s, canon, sizeof(canon)) != 0) {
		return;
	}
	if (keybind_parse(canon, again, sizeof(again)) != 0
	    || strcmp(canon, again) != 0) {
		abort();
	}
}

static void op_bind(const char *s)
{
	int rc = keybind_bind(s, "fuzz-command");

	if (rc == 0
	    && (!keybind_lookup(s)
		|| strcmp(keybind_lookup(s), "fuzz-command") != 0)) {
		abort();
	}
}

static void op_unbind(const char *s)
{
	if (keybind_unbind(s) == 0 && keybind_lookup(s) != NULL) {
		abort();
	}
}

static void op_key_roundtrip(const char *s)
{
	struct key_event ev, back;
	char text[KEY_FORMAT_MAX];

	if (key_parse(s, &ev) != 0) {
		return;
	}
	if (key_format(ev, text, sizeof(text)) != 0) {
		return;
	}
	if (key_parse(text, &back) != 0 || !key_event_equal(ev, back)) {
		abort();
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char str[FUZZ_KEYBIND_MAX_STR + 1];
	size_t pos = 0;

	while (pos < size) {
		const uint8_t *end = memchr(data + pos, 0, size - pos);
		size_t len = end ? (size_t)(end - (data + pos)) : size - pos;
		uint8_t op;

		if (len == 0) {
			pos++;
			continue;
		}
		if (len > FUZZ_KEYBIND_MAX_STR) {
			len = FUZZ_KEYBIND_MAX_STR;
		}
		op = data[pos];
		memcpy(str, data + pos + 1, len - 1);
		str[len - 1] = '\0';
		switch (op % 5) {
		case 0:
			op_parse(str);
			break;
		case 1:
			op_bind(str);
			break;
		case 2:
			op_unbind(str);
			break;
		case 3:
			(void)keybind_lookup(str);
			break;
		case 4:
			op_key_roundtrip(str);
			break;
		}
		pos += len + (end ? 1 : 0);
	}
	return 0;
}
