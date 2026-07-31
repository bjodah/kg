/* ===================== Keys as a base plus modifiers ===================== */

#include <string.h>

#include "keyevent.h"

#include "def.h"

int key_in_list(const int *keys, size_t count, int key)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (keys[i] == key) {
			return 1;
		}
	}
	return 0;
}

int key_event_equal(struct key_event a, struct key_event b)
{
	return a.base == b.base && a.mods == b.mods;
}

/* Every decoder integer that is not a bare character, and the event it
 * stands for.  Control letters (1..26) and the scalars 0x20..0x10FFFF
 * follow the rules below instead of taking a row each. */
static const struct {
	int legacy;
	int32_t base;
	uint8_t mods;
} legacy_keys[] = {
	{ KEY_NULL, ' ', KEY_MOD_CTRL },
	{ TAB, KEY_BASE_TAB, 0 },
	{ ENTER, KEY_BASE_RET, 0 },
	{ ESC, KEY_BASE_ESC, 0 },
	{ 28, '\\', KEY_MOD_CTRL },
	{ 29, ']', KEY_MOD_CTRL },
	{ 30, '^', KEY_MOD_CTRL },
	{ CTRL_UNDERSCORE, '_', KEY_MOD_CTRL },
	{ BACKSPACE, KEY_BASE_DEL, 0 },

	{ ARROW_LEFT, KEY_BASE_LEFT, 0 },
	{ ARROW_RIGHT, KEY_BASE_RIGHT, 0 },
	{ ARROW_UP, KEY_BASE_UP, 0 },
	{ ARROW_DOWN, KEY_BASE_DOWN, 0 },
	{ DEL_KEY, KEY_BASE_DELETE, 0 },
	{ HOME_KEY, KEY_BASE_HOME, 0 },
	{ END_KEY, KEY_BASE_END, 0 },
	{ PAGE_UP, KEY_BASE_PRIOR, 0 },
	{ PAGE_DOWN, KEY_BASE_NEXT, 0 },
	{ CTRL_ARROW_LEFT, KEY_BASE_LEFT, KEY_MOD_CTRL },
	{ CTRL_ARROW_RIGHT, KEY_BASE_RIGHT, KEY_MOD_CTRL },
	{ CTRL_ARROW_UP, KEY_BASE_UP, KEY_MOD_CTRL },
	{ CTRL_ARROW_DOWN, KEY_BASE_DOWN, KEY_MOD_CTRL },
	{ CTRL_HOME, KEY_BASE_HOME, KEY_MOD_CTRL },
	{ CTRL_END, KEY_BASE_END, KEY_MOD_CTRL },
	{ SHIFT_ARROW_LEFT, KEY_BASE_LEFT, KEY_MOD_SHIFT },
	{ SHIFT_ARROW_RIGHT, KEY_BASE_RIGHT, KEY_MOD_SHIFT },
	{ SHIFT_ARROW_UP, KEY_BASE_UP, KEY_MOD_SHIFT },
	{ SHIFT_ARROW_DOWN, KEY_BASE_DOWN, KEY_MOD_SHIFT },
	{ SHIFT_HOME, KEY_BASE_HOME, KEY_MOD_SHIFT },
	{ SHIFT_END, KEY_BASE_END, KEY_MOD_SHIFT },
	{ INSERT_KEY, KEY_BASE_INSERT, 0 },
	{ SHIFT_INSERT, KEY_BASE_INSERT, KEY_MOD_SHIFT },
	{ SHIFT_DELETE, KEY_BASE_DELETE, KEY_MOD_SHIFT },
	{ CTRL_INSERT, KEY_BASE_INSERT, KEY_MOD_CTRL },
	{ KEY_F3, KEY_BASE_F3, 0 },
	{ KEY_F4, KEY_BASE_F4, 0 },

	{ ALT_F, 'f', KEY_MOD_META },
	{ ALT_B, 'b', KEY_MOD_META },
	{ ALT_D, 'd', KEY_MOD_META },
	{ ALT_G, 'g', KEY_MOD_META },
	{ ALT_H, 'h', KEY_MOD_META },
	{ ALT_V, 'v', KEY_MOD_META },
	{ ALT_W, 'w', KEY_MOD_META },
	{ ALT_Q, 'q', KEY_MOD_META },
	{ ALT_BACKSPACE, KEY_BASE_DEL, KEY_MOD_META },
	{ ALT_PCT, '%', KEY_MOD_META },
	{ ALT_AT, '@', KEY_MOD_META },
	{ ALT_SEMICOLON, ';', KEY_MOD_META },
	{ ALT_COLON, ':', KEY_MOD_META },
	{ ALT_X, 'x', KEY_MOD_META },
	{ ALT_CARET, '^', KEY_MOD_META },
	{ ALT_U, 'u', KEY_MOD_META },
	{ ALT_L, 'l', KEY_MOD_META },
	{ ALT_C, 'c', KEY_MOD_META },
	{ ALT_BANG, '!', KEY_MOD_META },
	{ ALT_PIPE, '|', KEY_MOD_META },
	{ ALT_LT, '<', KEY_MOD_META },
	{ ALT_GT, '>', KEY_MOD_META },
	{ ALT_LBRACE, '{', KEY_MOD_META },
	{ ALT_RBRACE, '}', KEY_MOD_META },
	{ ALT_M, 'm', KEY_MOD_META },
	{ ALT_A, 'a', KEY_MOD_META },
	{ ALT_E, 'e', KEY_MOD_META },
	{ ALT_R, 'r', KEY_MOD_META },
	{ ALT_BACKSLASH, '\\', KEY_MOD_META },
	{ ALT_SPACE, ' ', KEY_MOD_META },
	{ ALT_Z, 'z', KEY_MOD_META },
	{ ALT_P, 'p', KEY_MOD_META },
	{ ALT_N, 'n', KEY_MOD_META },
	{ ALT_ENTER, KEY_BASE_RET, KEY_MOD_META },
	{ ALT_0, '0', KEY_MOD_META },
	{ ALT_1, '1', KEY_MOD_META },
	{ ALT_2, '2', KEY_MOD_META },
	{ ALT_3, '3', KEY_MOD_META },
	{ ALT_4, '4', KEY_MOD_META },
	{ ALT_5, '5', KEY_MOD_META },
	{ ALT_6, '6', KEY_MOD_META },
	{ ALT_7, '7', KEY_MOD_META },
	{ ALT_8, '8', KEY_MOD_META },
	{ ALT_9, '9', KEY_MOD_META },
	{ ALT_CTRL_S, 's', KEY_MOD_CTRL | KEY_MOD_META },
	{ ALT_CTRL_R, 'r', KEY_MOD_CTRL | KEY_MOD_META },
};

struct key_event key_event_from_legacy(int key)
{
	struct key_event event = { 0, 0 };
	size_t i;

	for (i = 0; i < sizeof(legacy_keys) / sizeof(*legacy_keys); i++) {
		if (legacy_keys[i].legacy == key) {
			event.base = legacy_keys[i].base;
			event.mods = legacy_keys[i].mods;
			return event;
		}
	}
	/* The rest of the control range is the letter it holds down. */
	if (key >= CTRL_A && key <= CTRL_Z) {
		event.base = 'a' + key - CTRL_A;
		event.mods = KEY_MOD_CTRL;
		return event;
	}
	/* A bare character, and (during the transition) a raw byte above
	 * ASCII, which is the same number as the scalar it is written as. */
	if (key > 0 && key <= KEY_BASE_LAST_SCALAR) {
		event.base = key;
	}
	return event;
}

/* Named bases, canonical spelling first.  "SPC" is here rather than in
 * the character path because a sequence is split on spaces, so a literal
 * space could never be a token. */
static const struct {
	const char *name;
	int32_t base;
} key_names[] = {
	{ "RET", KEY_BASE_RET },
	{ "TAB", KEY_BASE_TAB },
	{ "ESC", KEY_BASE_ESC },
	{ "DEL", KEY_BASE_DEL },
	{ "SPC", ' ' },
	{ "<left>", KEY_BASE_LEFT },
	{ "<right>", KEY_BASE_RIGHT },
	{ "<up>", KEY_BASE_UP },
	{ "<down>", KEY_BASE_DOWN },
	{ "<home>", KEY_BASE_HOME },
	{ "<end>", KEY_BASE_END },
	{ "<prior>", KEY_BASE_PRIOR },
	{ "<next>", KEY_BASE_NEXT },
	{ "<insert>", KEY_BASE_INSERT },
	{ "<delete>", KEY_BASE_DELETE },
	{ "<f3>", KEY_BASE_F3 },
	{ "<f4>", KEY_BASE_F4 },
};

static const char *key_base_name(int32_t base)
{
	size_t i;

	for (i = 0; i < sizeof(key_names) / sizeof(*key_names); i++) {
		if (key_names[i].base == base) {
			return key_names[i].name;
		}
	}
	return NULL;
}

static uint8_t key_mod_bit(char c)
{
	switch (c) {
	case 'C':
		return KEY_MOD_CTRL;
	case 'M':
		return KEY_MOD_META;
	case 'S':
		return KEY_MOD_SHIFT;
	default:
		return 0;
	}
}

/* Shift is only ever reported for the named keys the terminal sends a
 * modified sequence for; a shifted character is just the other
 * character, and accepting "S-a" would give one key two spellings. */
static int key_base_takes_shift(int32_t base)
{
	return base > KEY_BASE_LAST_SCALAR;
}

static int32_t key_parse_base(const char *text)
{
	uint32_t scalar;
	int span = 0;
	size_t i;

	for (i = 0; i < sizeof(key_names) / sizeof(*key_names); i++) {
		if (strcmp(key_names[i].name, text) == 0) {
			return key_names[i].base;
		}
	}
	scalar = utf8_codepoint_at(text, (int)strlen(text), 0, &span);
	/* A literal space is not a token -- a sequence is split on spaces --
	 * so the space key is only ever spelled "SPC", which the table
	 * above already answered. */
	if (span <= 0 || text[span] != '\0' || scalar == 0 || scalar == ' ') {
		return -1;
	}
	return (int32_t)scalar;
}

int key_parse(const char *text, struct key_event *out)
{
	struct key_event event = { 0, 0 };
	uint8_t mod;

	if (!text || !out) {
		return -1;
	}
	/* A modifier is a letter, a dash, and something left to modify, so
	 * "M--" is Meta-minus and "-" is the key itself.  A repeated
	 * modifier stops the loop and then fails to name a base. */
	while (text[0] && text[1] == '-' && (mod = key_mod_bit(text[0])) != 0
	    && !(event.mods & mod)) {
		event.mods |= mod;
		text += 2;
	}
	event.base = key_parse_base(text);
	if (event.base < 0) {
		return -1;
	}
	if ((event.mods & KEY_MOD_SHIFT) && !key_base_takes_shift(event.base)) {
		return -1;
	}
	*out = event;
	return 0;
}

/* UTF-8 for one scalar, which is how a character base is spelled back. */
static int key_encode_scalar(int32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

int key_format(struct key_event key, char *out, size_t size)
{
	static const struct {
		uint8_t mod;
		char letter;
	} order[] = {
		{ KEY_MOD_CTRL, 'C' },
		{ KEY_MOD_META, 'M' },
		{ KEY_MOD_SHIFT, 'S' },
	};
	char text[KEY_FORMAT_MAX];
	const char *name;
	size_t used = 0, i;
	int len;

	if (!out || size == 0) {
		return -1;
	}
	for (i = 0; i < sizeof(order) / sizeof(*order); i++) {
		if (key.mods & order[i].mod) {
			text[used++] = order[i].letter;
			text[used++] = '-';
		}
	}
	name = key_base_name(key.base);
	if (name) {
		len = (int)strlen(name);
		if (used + (size_t)len >= sizeof(text)) {
			return -1;
		}
		memcpy(text + used, name, (size_t)len);
	} else {
		if (key.base <= 0 || key.base > KEY_BASE_LAST_SCALAR) {
			return -1;
		}
		len = key_encode_scalar(key.base, text + used);
	}
	used += (size_t)len;
	if (used >= size) {
		return -1;
	}
	memcpy(out, text, used);
	out[used] = '\0';
	return 0;
}
