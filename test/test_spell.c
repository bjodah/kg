/* test_spell.c -- the Enchant spell checker: candidate rules, the row
 * scanner and its face gating, session words, and the update seam.
 *
 * The scanner is pure over one row with the dictionary verdict injected,
 * so most of this suite runs in every configuration with a stub.  The
 * dictionary-backed assertions run only where a dictionary loads (a
 * WITH_ENCHANT=1 build on a box that has one); everywhere else the suite
 * asserts the disabled half answers "no check ran".  That conditional is
 * spelled out per test rather than as a SKIP: the harness counts passes,
 * and a box without dictionaries still proves the fallback.
 */

#include "../src/decor.h"
#include "../src/def.h"
#include "../src/edit.h"
#include "../src/lisp.h"
#include "../src/marker.h"
#include "../src/spell.h"
#include "../src/syntax.h"
#include "test.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void setup(void)
{
	free_all_rows();
	kg_decor_store_free(bcur());
	kg_marker_store_free(bcur());
	reset_current_buffer();
	reset_current_view();
	bcur()->active = 1;
	bcur()->syntax = NULL;
	bcur()->display.tab_width = KG_TAB_WIDTH;
	memset(&editor, 0, sizeof(editor));
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	/* spell_update() walks the windows the way the refresh loop does,
	 * so the window must show this buffer or the seam sees nothing. */
	buf_attach_view(wcur(), buf_current);
	undo_free();
	undo_init();
}

static void teardown(void)
{
	bcur()->spell_mode = 0;
	spell_update();
	undo_free();
	free_all_rows();
	kg_decor_store_free(bcur());
	kg_marker_store_free(bcur());
	bcur()->row = NULL;
	bcur()->numrows = 0;
}

static void fill(const char *const *lines, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		editor_insert_row(bcur(), i, lines[i], (int)strlen(lines[i]));
	}
	bcur()->dirty = 0;
}

static void fill_one(const char *line)
{
	const char *lines[1] = { line };

	fill(lines, 1);
}

/* Paint the whole first row one face: the scanner only reads hl, never
 * how it got there. */
static void paint_row(unsigned char face)
{
	erow *row = &bcur()->row[0];

	memset(row->hl, face, (size_t)row->rsize);
}

/* The stub verdict: only these three words are misspelled. */
static int stub_is_ok(const char *word, size_t len, void *ctx)
{
	(void)ctx;
	if ((len == 3 && memcmp(word, "teh", 3) == 0)
	    || (len == 4 && memcmp(word, "wrod", 4) == 0)
	    || (len == 9 && memcmp(word, "mispelled", 9) == 0)) {
		return 0;
	}
	return 1;
}

static void test_candidate_rules(void)
{
	char overlong[66];

	setup();
	CHECK(spell_word_candidate("hello", 5));
	CHECK(spell_word_candidate("Hello", 5));
	CHECK(spell_word_candidate("I", 1));
	CHECK(spell_word_candidate("a", 1));
	CHECK(!spell_word_candidate("", 0));
	CHECK(!spell_word_candidate(NULL, 0));
	CHECK(!spell_word_candidate("abc123", 6));
	CHECK(!spell_word_candidate("h264", 4));
	CHECK(!spell_word_candidate("NASA", 4));
	CHECK(!spell_word_candidate("HTML", 4));
	memset(overlong, 'x', sizeof(overlong) - 1);
	overlong[sizeof(overlong) - 1] = '\0';
	CHECK(!spell_word_candidate(overlong, sizeof(overlong) - 1));
	overlong[64] = '\0';
	CHECK(spell_word_candidate(overlong, 64));
	teardown();
}

static void test_scan_finds_words(void)
{
	struct spell_span spans[8];
	int n;

	setup();
	fill_one("the teh quick");
	paint_row(HL_COMMENT);
	n = spell_scan_row(
	    &bcur()->row[0], &bcur()->display, 0, stub_is_ok, NULL, spans, 8);
	CHECK(n == 1);
	CHECK(spans[0].start == 4);
	CHECK(spans[0].end == 7);
	teardown();
}

static void test_scan_splits_hyphens(void)
{
	struct spell_span spans[8];
	int n;

	setup();
	fill_one("teh-mail wrod");
	paint_row(HL_COMMENT);
	n = spell_scan_row(
	    &bcur()->row[0], &bcur()->display, 0, stub_is_ok, NULL, spans, 8);
	CHECK(n == 2);
	CHECK(spans[0].start == 0);
	CHECK(spans[0].end == 3);
	CHECK(spans[1].start == 9);
	CHECK(spans[1].end == 13);
	teardown();
}

static void test_scan_trims_quotes(void)
{
	struct spell_span spans[8];
	int n;

	setup();
	fill_one("'teh' \"wrod\"");
	paint_row(HL_COMMENT);
	n = spell_scan_row(
	    &bcur()->row[0], &bcur()->display, 0, stub_is_ok, NULL, spans, 8);
	CHECK(n == 2);
	CHECK(spans[0].start == 1);
	CHECK(spans[0].end == 4);
	CHECK(spans[1].start == 7);
	CHECK(spans[1].end == 11);
	teardown();
}

static void test_scan_gates_on_faces(void)
{
	struct spell_span spans[8];

	setup();
	fill_one("teh wrod");
	paint_row(HL_NORMAL);
	CHECK(spell_scan_row(&bcur()->row[0], &bcur()->display, 0, stub_is_ok,
		  NULL, spans, 8)
	    == 0);
	paint_row(HL_KEYWORD1);
	CHECK(spell_scan_row(&bcur()->row[0], &bcur()->display, 0, stub_is_ok,
		  NULL, spans, 8)
	    == 0);
	paint_row(HL_STRING);
	CHECK(spell_scan_row(&bcur()->row[0], &bcur()->display, 0, stub_is_ok,
		  NULL, spans, 8)
	    == 2);
	/* Prose buffers check everything, faces or not. */
	paint_row(HL_NORMAL);
	CHECK(spell_scan_row(&bcur()->row[0], &bcur()->display, 1, stub_is_ok,
		  NULL, spans, 8)
	    == 2);
	teardown();
}

static void test_scan_maps_tabs(void)
{
	struct spell_span spans[8];
	erow *row;
	int n;

	setup();
	fill_one("a\tteh");
	row = &bcur()->row[0];
	CHECK(row->rsize == 11); /* `a' plus a tab stop of 7 plus `teh' */
	memset(row->hl, HL_NORMAL, (size_t)row->rsize);
	memset(row->hl + 8, HL_COMMENT, 3);
	n = spell_scan_row(
	    row, &bcur()->display, 0, stub_is_ok, NULL, spans, 8);
	CHECK(n == 1);
	CHECK(spans[0].start == 2);
	CHECK(spans[0].end == 5);
	teardown();
}

static void test_scan_is_bounded(void)
{
	struct spell_span spans[2];
	int n;

	setup();
	fill_one("teh wrod mispelled");
	paint_row(HL_COMMENT);
	n = spell_scan_row(
	    &bcur()->row[0], &bcur()->display, 0, stub_is_ok, NULL, spans, 2);
	CHECK(n == 2);
	CHECK(spans[0].start == 0);
	CHECK(spans[1].start == 4);
	teardown();
}

static void test_session_words(void)
{
	setup();
	CHECK(!spell_session_has("teh", 3));
	CHECK(spell_session_accept("teh", 3) == 0);
	CHECK(spell_session_has("teh", 3));
	CHECK(!spell_session_has("tehX", 4));
	/* Accepting twice is a no-op success. */
	CHECK(spell_session_accept("teh", 3) == 0);
	/* Empty and missing words are refused, not stored. */
	CHECK(spell_session_accept("", 0) != 0);
	CHECK(spell_session_accept(NULL, 0) != 0);
	spell_shutdown();
	CHECK(!spell_session_has("teh", 3));
	spell_init();
	teardown();
}

static void test_language_default(void)
{
	setup();
	/* No interpreter runs in this binary, so no variable is bound and
	 * the default answers. */
	CHECK(strcmp(spell_language(), "en_US") == 0);
	teardown();
}

static void test_language_lisp_roundtrip(void)
{
	char result[128] = "";
	const char *src = "(setq spell-language \"sv\")";
	const char *saved_xdg;

	setup();
	if (!kg_lisp_active()) {
		CHECK(strcmp(spell_language(), "en_US") == 0);
		teardown();
		return;
	}
	saved_xdg = getenv("XDG_CONFIG_HOME");
	setenv("XDG_CONFIG_HOME", "/nonexistent", 1);
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string(src, strlen(src), result, sizeof(result))
	    == 0);
	CHECK(strcmp(spell_language(), "sv") == 0);
	src = "(setq spell-language 42)";
	CHECK(kg_lisp_eval_string(src, strlen(src), result, sizeof(result))
	    == 0);
	CHECK(strcmp(spell_language(), "en_US") == 0);
	kg_lisp_shutdown();
	if (saved_xdg) {
		setenv("XDG_CONFIG_HOME", saved_xdg, 1);
	} else {
		unsetenv("XDG_CONFIG_HOME");
	}
	teardown();
}

static void test_commands_without_dict(void)
{
	setup();
	if (!spell_supported()) {
		/* The disabled half answers without reaching a prompt,
		 * a dictionary or a replacement. */
		spell_cmd_mode(0);
		CHECK(!spell_mode_on(bcur()));
		spell_cmd_next(0);
		spell_cmd_previous(0);
		spell_cmd_correct(0);
		CHECK(!spell_mode_on(bcur()));
	} else if (spell_available()) {
		spell_cmd_mode(0);
		CHECK(spell_mode_on(bcur()));
		spell_cmd_mode(0);
		CHECK(!spell_mode_on(bcur()));
	}
	teardown();
}

static void test_backend_word(void)
{
	setup();
	if (spell_available()) {
		CHECK(spell_supported());
		CHECK(spell_check_word("the", 3) == 1);
		CHECK(spell_check_word("teh", 3) == 0);
		CHECK(spell_session_accept("teh", 3) == 0);
		CHECK(spell_check_word("teh", 3) == 1);
		spell_shutdown();
		spell_init();
	} else {
		CHECK(spell_check_word("the", 3) == -1);
	}
	teardown();
}

static void test_backend_suggest(void)
{
	char **list = NULL;
	size_t n, i;
	int found_the = 0;

	setup();
	n = spell_suggest("teh", 3, &list);
	if (spell_available()) {
		CHECK(n > 0);
		CHECK(list != NULL);
		for (i = 0; i < n; i++) {
			if (strcmp(list[i], "the") == 0) {
				found_the = 1;
			}
		}
		CHECK(found_the);
	} else {
		CHECK(n == 0);
		CHECK(list == NULL);
	}
	spell_free_suggestions(list, n);
	spell_free_suggestions(NULL, 0);
	teardown();
}

static size_t decor_count(void)
{
	struct kg_decor_query q;
	struct kg_decor_query_span s;
	size_t n = 0;

	kg_decor_query_begin(&q, bcur(), 0, (size_t)-1);
	while (kg_decor_query_next(&q, &s)) {
		if (s.face == KG_DECOR_FACE_SPELL) {
			n++;
		}
	}
	return n;
}

static void test_update_paints_misspellings(void)
{
	const char *lines[2] = { "the teh quick", "a wrod indeed" };

	setup();
	if (!spell_available()) {
		/* Nothing may be published without a dictionary. */
		bcur()->spell_mode = 1;
		fill(lines, 2);
		spell_update();
		CHECK(decor_count() == 0);
		teardown();
		return;
	}
	bcur()->spell_mode = 1;
	CHECK(spell_mode_on(bcur()));
	fill(lines, 2);
	spell_update();
	spell_update();
	CHECK(decor_count() == 2);
	/* A still frame republishes nothing new. */
	spell_update();
	CHECK(decor_count() == 2);
	/* Leaving the mode retires the decorations. */
	bcur()->spell_mode = 0;
	spell_update();
	CHECK(decor_count() == 0);
	teardown();
}

int main(void)
{
	RUN(test_candidate_rules);
	RUN(test_scan_finds_words);
	RUN(test_scan_splits_hyphens);
	RUN(test_scan_trims_quotes);
	RUN(test_scan_gates_on_faces);
	RUN(test_scan_maps_tabs);
	RUN(test_scan_is_bounded);
	RUN(test_session_words);
	RUN(test_language_default);
	RUN(test_language_lisp_roundtrip);
	RUN(test_commands_without_dict);
	RUN(test_backend_word);
	RUN(test_backend_suggest);
	RUN(test_update_paints_misspellings);
	return test_summary();
}
