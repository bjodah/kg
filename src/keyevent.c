/* ===================== Keys as a base plus modifiers ===================== */

#include <string.h>

#include "keyevent.h"

#include "def.h"

int key_event_equal(struct key_event a, struct key_event b)
{
	return a.base == b.base && a.mods == b.mods;
}

int key_in_list(
    const struct key_event *keys, size_t count, struct key_event key)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (key_event_equal(keys[i], key)) {
			return 1;
		}
	}
	return 0;
}

/* Every plain byte that is not a bare character, and the event it
 * stands for.  Control letters (1..26) and the scalars 0x20..0xFF
 * follow the rules below instead of taking a row each. */
static const struct {
	int byte;
	int32_t base;
	uint8_t mods;
} byte_keys[] = {
	{ KEY_NULL, ' ', KEY_MOD_CTRL },
	{ TAB, KEY_BASE_TAB, 0 },
	{ ENTER, KEY_BASE_RET, 0 },
	{ ESC, KEY_BASE_ESC, 0 },
	{ 28, '\\', KEY_MOD_CTRL },
	{ 29, ']', KEY_MOD_CTRL },
	{ 30, '^', KEY_MOD_CTRL },
	{ CTRL_UNDERSCORE, '_', KEY_MOD_CTRL },
	{ BACKSPACE, KEY_BASE_DEL, 0 },
};

struct key_event key_event_from_byte(int c)
{
	struct key_event event = { 0, 0 };
	size_t i;

	for (i = 0; i < sizeof(byte_keys) / sizeof(*byte_keys); i++) {
		if (byte_keys[i].byte == c) {
			event.base = byte_keys[i].base;
			event.mods = byte_keys[i].mods;
			return event;
		}
	}
	/* The rest of the control range is the letter it holds down. */
	if (c >= CTRL_A && c <= CTRL_Z) {
		event.base = 'a' + c - CTRL_A;
		event.mods = KEY_MOD_CTRL;
		return event;
	}
	/* A bare character, and a raw byte above ASCII -- the lead of a
	 * multi-byte character the terminal sends one byte at a time --
	 * which is the same number as the scalar it is written as. */
	if (c > 0 && c <= 0xFF) {
		event.base = c;
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
	size_t len;
	int span = 0;
	size_t i;

	for (i = 0; i < sizeof(key_names) / sizeof(*key_names); i++) {
		if (strcmp(key_names[i].name, text) == 0) {
			return key_names[i].base;
		}
	}
	len = strlen(text);
	scalar = utf8_codepoint_at(text, (int)len, 0, &span);
	/* The scalar must span the whole text: compared as lengths, not as
	 * text[span] != '\0', because the decoder reports a span of one
	 * even for the empty string a trailing modifier leaves ("C-"), and
	 * indexing by it would read past the string.  A literal space is
	 * not a token -- a sequence is split on spaces -- so the space key
	 * is only ever spelled "SPC", which the table above already
	 * answered. */
	if (span <= 0 || (size_t)span != len || scalar <= ' ') {
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
