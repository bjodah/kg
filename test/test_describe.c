/* test_describe.c -- the command table and the keymaps describing
 * themselves.
 *
 * describe-bindings is the one of the four that reads no input, so it is
 * the one a native test can drive: it walks the real built-in maps, spells
 * every sequence with the real formatter, and lands the result in a real
 * read-only buffer.  That covers the whole render path -- enumerate,
 * format, page, buffer -- without a terminal.  The three that prompt (and
 * describe-key's shadow report, which needs live mode maps) are covered by
 * the PTY cases named in the file header of each.
 *
 * The point of every check here is that the answer is read out of the two
 * registries rather than out of a table kept beside them: a binding added
 * to kbd.c has to show up here with no edit to this file.
 *
 * Linked against everything but main.c, the way test_cmd is, because the
 * command table reaches most of the editor. */

#include "../src/cmd.h"
#include "../src/def.h"
#include "../src/describe.h"
#include "../src/kbd.h"
#include "../src/keymap.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

/* The *Describe* buffer describe_bindings() just wrote, or NULL. */
static struct editor_buffer *describe_buffer(void)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename
		    && strcmp(buflist[i].filename, DESCRIBE_BUFFER_NAME) == 0) {
			return &buflist[i];
		}
	}
	return NULL;
}

/* Whether some row of the page contains `needle`. */
static int page_has(struct editor_buffer *b, const char *needle)
{
	int i;

	for (i = 0; i < b->numrows; i++) {
		if (b->row[i].chars && strstr(b->row[i].chars, needle)) {
			return 1;
		}
	}
	return 0;
}

/* The row that starts with `key` followed by spaces, or NULL: the key
 * column is left-aligned and padded, so "C-a " matches only the whole
 * key rather than a longer sequence starting with it. */
static const char *page_row_for(struct editor_buffer *b, const char *key)
{
	int i;

	for (i = 0; i < b->numrows; i++) {
		const char *row = b->row[i].chars;
		size_t len = strlen(key);

		if (row && strncmp(row, key, len) == 0 && row[len] == ' ') {
			return row;
		}
	}
	return NULL;
}

static struct editor_buffer *render_bindings(void)
{
	struct editor_buffer *b;

	keymap_reset();
	key_install_builtin_maps();
	describe_bindings(0);
	b = describe_buffer();
	CHECK(b != NULL);
	return b;
}

/* The page is a read-only buffer like any other special buffer, so
 * scrolling, the mode line and q-to-close come from the editor rather
 * than from this module. */
static void test_renders_a_read_only_buffer(void)
{
	struct editor_buffer *b = render_bindings();

	if (!b) {
		return;
	}
	CHECK(b->numrows > 0);
	CHECK(b->readonly);
	CHECK(b->readonly_override == 1);
	/* Wrapping is the display's, not this module's: a summary wider
	 * than the window folds instead of running off it. */
	CHECK(b->visual_line_mode);
	/* Nothing here counts as an edit the user could be asked to save. */
	CHECK(!b->dirty);
}

/* Every binding, read out of the maps at the moment of asking. */
static void test_lists_every_binding_with_its_map(void)
{
	struct editor_buffer *b = render_bindings();
	const char *row;

	if (!b) {
		return;
	}
	/* One row per binding, plus the introduction. */
	CHECK(b->numrows > keymap_binding_count());

	row = page_row_for(b, "C-x C-s");
	CHECKF(row != NULL, "no row for C-x C-s");
	if (row) {
		CHECK(strstr(row, "save-buffer") != NULL);
		CHECK(strstr(row, "global") != NULL);
	}
	/* A mode map's binding says which mode map, so the answer names
	 * the layer rather than implying there is only one. */
	row = page_row_for(b, "C-c C-k");
	CHECKF(row != NULL, "no row for C-c C-k");
	if (row) {
		CHECK(strstr(row, "git-commit") != NULL
		    || strstr(row, "compilation") != NULL);
	}
	/* A prefix declared with no leaf of its own is listed as one,
	 * rather than as a binding to nothing. */
	row = page_row_for(b, "C-c");
	CHECKF(row != NULL, "no row for the C-c prefix");
	if (row) {
		CHECK(strstr(row, "a prefix") != NULL);
	}
}

/* An inactive mode map is still listed: describe-bindings answers "what
 * is bound", and hiding the maps that are not live right now would make
 * the git-rebase keys invisible from every other buffer. */
static void test_lists_inactive_maps_too(void)
{
	struct editor_buffer *b = render_bindings();

	if (!b) {
		return;
	}
	CHECK(page_has(b, "git-rebase-pick"));
	CHECK(page_has(b, "dired-find-file"));
}

/* Asking twice replaces the page rather than appending to it. */
static void test_rendering_again_replaces_the_page(void)
{
	struct editor_buffer *b = render_bindings();
	int first;

	if (!b) {
		return;
	}
	first = b->numrows;
	describe_bindings(0);
	b = describe_buffer();
	CHECK(b != NULL);
	if (b) {
		CHECK(b->numrows == first);
	}
}

/* With no maps installed there is nothing to list, and the command still
 * produces a page saying so rather than an empty buffer or no buffer. */
static void test_no_bindings_still_answers(void)
{
	struct editor_buffer *b;

	keymap_reset();
	describe_bindings(0);
	b = describe_buffer();
	CHECK(b != NULL);
	if (b) {
		CHECK(b->numrows > 0);
	}
	CHECK(keymap_binding_count() == 0);
}

/* The four commands are in the one policy table, with the verdicts the
 * table is the single source of: none of them edits a buffer, and
 * describe-key alone is unreachable from Lisp -- it reads a key
 * SEQUENCE, which only a keyboard has, where the other three prompt for
 * a name or take the window, both of which Phase 17's audit found
 * ordinary.  test_cmd.c pins the audited set; this is the local half. */
static void test_command_table_rows(void)
{
	static const char *const names[] = { "describe-bindings",
		"describe-command", "describe-key", "where-is" };
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(*names); i++) {
		const struct named_cmd *cmd = cmd_lookup(names[i]);

		CHECKF(cmd != NULL, "%s is not in the command table", names[i]);
		if (!cmd) {
			continue;
		}
		CHECKF(!(cmd->flags & CMD_EDITS_BUFFER),
		    "%s claims to edit the buffer", names[i]);
		CHECK(cmd->fn != NULL);
		CHECK(cmd->summary != NULL);
		CHECKF(((cmd->flags & CMD_LISP_CALLABLE) != 0)
			== (strcmp(names[i], "describe-key") != 0),
		    "%s: wrong side of the Lisp-callable audit", names[i]);
		CHECK(cmd_id_by_name(names[i]) != CMD_ID_NONE);
	}
}

int main(void)
{
	RUN(test_renders_a_read_only_buffer);
	RUN(test_lists_every_binding_with_its_map);
	RUN(test_lists_inactive_maps_too);
	RUN(test_rendering_again_replaces_the_page);
	RUN(test_no_bindings_still_answers);
	RUN(test_command_table_rows);
	return test_summary();
}
