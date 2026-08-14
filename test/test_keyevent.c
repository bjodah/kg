/* test_keyevent.c -- keys as a base plus modifiers.
 *
 * Three properties, checked rather than sampled:
 *
 *  - key_event_from_byte() is total and injective over one plain,
 *    undecoded byte (0..255): every byte has an event, and no two share
 *    one.  Escape sequences -- arrows, modified keys, function keys,
 *    Meta -- are multiple bytes and are decoded in tty.c instead (see
 *    test_tty.c's test_escape_sequences_decode_to_key_events());
 *  - format is canonical and parse is its inverse, over every named key
 *    and every modifier combination each accepts;
 *  - parse rejects what it does not document: a repeated modifier, a
 *    trailing byte, an unknown name, Shift on a character.
 *
 * This binary links keyevent.o and width.o only: the module answers
 * questions about keys and reaches for nothing else. */

#include "../src/def.h"
#include "../src/keyevent.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

static void test_byte_decode_is_total_and_injective(void)
{
	int a, b;

	for (a = 0; a <= 0xFF; a++) {
		struct key_event ea = key_event_from_byte(a);

		CHECKF(ea.base != 0, "byte %d has no event", a);
		CHECKF((ea.mods & ~(unsigned)KEY_MOD_ALL) == 0,
		    "byte %d has an unknown modifier", a);
		for (b = a + 1; b <= 0xFF; b++) {
			struct key_event eb = key_event_from_byte(b);

			CHECKF(!key_event_equal(ea, eb),
			    "bytes %d and %d are the same event", a, b);
		}
	}
}

/* The mapping the rest of the editor will be read against. */
static void test_byte_decode_spellings(void)
{
	static const struct {
		int byte;
		const char *spelling;
	} expected[] = {
		{ KEY_NULL, "C-SPC" },
		{ CTRL_A, "C-a" },
		{ CTRL_Z, "C-z" },
		{ TAB, "TAB" },
		{ ENTER, "RET" },
		{ ESC, "ESC" },
		{ CTRL_UNDERSCORE, "C-_" },
		{ BACKSPACE, "DEL" },
	};
	size_t i;

	for (i = 0; i < sizeof(expected) / sizeof(*expected); i++) {
		char text[KEY_FORMAT_MAX];
		struct key_event parsed;

		CHECK(key_format(key_event_from_byte(expected[i].byte), text,
			  sizeof(text))
		    == 0);
		CHECKF(strcmp(text, expected[i].spelling) == 0,
		    "byte %d formats as %s, want %s", expected[i].byte, text,
		    expected[i].spelling);
		CHECK(key_parse(expected[i].spelling, &parsed) == 0);
		CHECK(key_event_equal(
		    parsed, key_event_from_byte(expected[i].byte)));
	}
}

/* A character key is the character, whatever its byte length. */
static void test_characters_round_trip(void)
{
	static const char *const characters[]
	    = { "a", "Z", "0", "%", "-", "\\", "~", "é", "€", "𝄞" };
	size_t i;
	int c;

	for (c = 0x21; c < 0x7F; c++) {
		char text[KEY_FORMAT_MAX];
		struct key_event event = { c, 0 };
		struct key_event parsed;

		CHECK(key_format(event, text, sizeof(text)) == 0);
		CHECKF(key_parse(text, &parsed) == 0, "%s does not parse back",
		    text);
		CHECK(key_event_equal(event, parsed));
		CHECK(key_event_equal(event, key_event_from_byte(c)));
	}
	for (i = 0; i < sizeof(characters) / sizeof(*characters); i++) {
		char text[KEY_FORMAT_MAX];
		struct key_event parsed, again;

		CHECKF(key_parse(characters[i], &parsed) == 0, "%s: no parse",
		    characters[i]);
		CHECK(key_format(parsed, text, sizeof(text)) == 0);
		CHECKF(strcmp(text, characters[i]) == 0, "%s formats as %s",
		    characters[i], text);
		CHECK(key_parse(text, &again) == 0);
		CHECK(key_event_equal(parsed, again));
	}
	/* A space is only ever spelled SPC: a sequence is split on spaces. */
	CHECK(key_parse(" ", &(struct key_event) { 0, 0 }) != 0);
}

/* Every base with every modifier set it accepts, formatted and parsed
 * back.  Shift is only for the named keys the terminal reports it on. */
static void test_every_base_and_modifier_round_trips(void)
{
	static const char *const bases[] = { "RET", "TAB", "ESC", "DEL", "SPC",
		"<left>", "<right>", "<up>", "<down>", "<home>", "<end>",
		"<prior>", "<next>", "<insert>", "<delete>", "<f1>", "<f2>",
		"<f3>", "<f4>", "<f5>", "<f6>", "<f7>", "<f8>", "<f9>", "<f10>",
		"<f11>", "<f12>", "a", "9", ";" };
	size_t i;
	unsigned mods;

	for (i = 0; i < sizeof(bases) / sizeof(*bases); i++) {
		struct key_event base_event;

		CHECKF(key_parse(bases[i], &base_event) == 0, "%s: no parse",
		    bases[i]);
		for (mods = 0; mods <= KEY_MOD_ALL; mods++) {
			struct key_event event
			    = { base_event.base, (uint8_t)mods };
			struct key_event parsed;
			char text[KEY_FORMAT_MAX];

			if ((mods & KEY_MOD_SHIFT)
			    && base_event.base <= KEY_BASE_LAST_SCALAR) {
				continue;
			}
			CHECKF(key_format(event, text, sizeof(text)) == 0,
			    "%s with mods %u does not format", bases[i], mods);
			CHECKF(key_parse(text, &parsed) == 0,
			    "%s does not parse back", text);
			CHECKF(key_event_equal(event, parsed),
			    "%s does not round trip", text);
		}
	}
}

static void test_parse_rejects_what_it_does_not_document(void)
{
	static const char *const rejected[] = {
		"",
		"C-",
		"M-",
		"C-C-a",
		"M-M-x",
		"S-a", /* Shift is not how a capital letter is spelled */
		"S-9",
		"ab", /* two characters is not one key */
		"C-ab",
		"RETURN",
		"<foo>",
		"<left",
		"C-<left>x",
		"x-a", /* not a modifier */
		" ",
	};
	size_t i;

	for (i = 0; i < sizeof(rejected) / sizeof(*rejected); i++) {
		struct key_event event = { -7, 99 };

		CHECKF(key_parse(rejected[i], &event) != 0, "%s parsed",
		    rejected[i]);
		CHECKF(event.base == -7 && event.mods == 99,
		    "%s was rejected but wrote to the output", rejected[i]);
	}
	CHECK(key_parse(NULL, &(struct key_event) { 0, 0 }) != 0);
	CHECK(key_parse("a", NULL) != 0);
	/* Capital letters are themselves, and are not Shift plus a letter. */
	CHECK(key_parse("A", &(struct key_event) { 0, 0 }) == 0);
}

static void test_format_reports_a_buffer_that_is_too_small(void)
{
	struct key_event event
	    = { KEY_BASE_LEFT, KEY_MOD_CTRL | KEY_MOD_META | KEY_MOD_SHIFT };
	char text[KEY_FORMAT_MAX];
	size_t size;

	CHECK(key_format(event, text, sizeof(text)) == 0);
	CHECK(strcmp(text, "C-M-S-<left>") == 0);
	for (size = 0; size < strlen("C-M-S-<left>") + 1; size++) {
		char small[KEY_FORMAT_MAX] = { 0 };

		CHECKF(key_format(event, small, size) != 0,
		    "%zu bytes was accepted for a 12-column key", size);
	}
	CHECK(key_format(event, NULL, sizeof(text)) != 0);
	/* Not a key: no base. */
	CHECK(key_format((struct key_event) { 0, 0 }, text, sizeof(text)) != 0);
	CHECK(key_format(
		  (struct key_event) { 0x110000 + 999, 0 }, text, sizeof(text))
	    != 0);
}

static void test_key_in_list(void)
{
	static const struct key_event keys[] = { { 'a', KEY_MOD_CTRL },
		{ 'f', KEY_MOD_META }, { KEY_BASE_UP, 0 } };
	static const struct key_event one[] = { { KEY_BASE_RET, 0 } };
	struct key_event a = { 'a', KEY_MOD_CTRL };
	struct key_event up = { KEY_BASE_UP, 0 };
	struct key_event b = { 'b', KEY_MOD_CTRL };
	struct key_event ret = { KEY_BASE_RET, 0 };
	struct key_event zero = { 0, 0 };

	CHECK(KEY_IN_LIST(keys, a));
	CHECK(KEY_IN_LIST(keys, up));
	CHECK(!KEY_IN_LIST(keys, b));
	CHECK(KEY_IN_LIST(one, ret));
	CHECK(!KEY_IN_LIST(one, zero));
	CHECK(!key_in_list(keys, 0, a));
}

int main(void)
{
	RUN(test_byte_decode_is_total_and_injective);
	RUN(test_byte_decode_spellings);
	RUN(test_characters_round_trip);
	RUN(test_every_base_and_modifier_round_trips);
	RUN(test_parse_rejects_what_it_does_not_document);
	RUN(test_format_reports_a_buffer_that_is_too_small);
	RUN(test_key_in_list);
	return test_summary();
}
