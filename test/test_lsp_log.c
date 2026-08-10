/* test_lsp_log.c — the `*lsp-log*` buffer and the bounded store under it
 * (src/lsp_log.c).
 *
 * Two levels, because the module has two.  The store is pure -- bytes in,
 * whole lines evicted from the head, a flag saying whether any were -- so
 * it is driven directly with a small cap, which is the only way to see the
 * eviction policy without writing 64 KiB per assertion.
 *
 * The buffer half is driven through lsp_log_line() against the real
 * bufmgr.c, because what it promises is about a buffer: that it is created
 * on the first line and not before, that it is never selected, and that a
 * store which has evicted its head leaves the buffer showing what the
 * store kept rather than what it dropped.
 */

#include "../src/def.h"
#include "../src/lsp_log.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------- the store ---------------------------- */

static bool push(
    struct lsp_log_store *s, const char *line, size_t cap, bool *trimmed)
{
	return lsp_log_store_push(s, line, strlen(line), cap, trimmed);
}

/* Under the cap nothing moves: the store is the lines, in order. */
static void test_store_keeps_what_fits(void)
{
	struct lsp_log_store s = { 0 };
	bool trimmed = true;

	CHECK(push(&s, "[a] one\n", 64, &trimmed));
	CHECK(!trimmed);
	CHECK(push(&s, "[a] two\n", 64, &trimmed));
	CHECK(!trimmed);
	CHECK(s.len == 16);
	CHECK(memcmp(s.data, "[a] one\n[a] two\n", 16) == 0);
	lsp_log_store_reset(&s);
	CHECK(s.data == NULL && s.len == 0);
}

/* Over the cap the oldest lines go, whole ones only, until it fits.  A
 * store that cut mid-line would leave a first line starting inside a word,
 * which reads as corruption rather than as eviction. */
static void test_store_evicts_whole_lines_from_the_head(void)
{
	struct lsp_log_store s = { 0 };
	bool trimmed = false;

	CHECK(push(&s, "aaaa\n", 12, &trimmed)); /* 5 */
	CHECK(push(&s, "bbbb\n", 12, &trimmed)); /* 10 */
	CHECK(!trimmed);
	CHECK(push(&s, "cccc\n", 12, &trimmed)); /* 15 > 12 */
	CHECK(trimmed);
	CHECK(s.len == 10);
	CHECK(memcmp(s.data, "bbbb\ncccc\n", 10) == 0);
	lsp_log_store_reset(&s);
}

/* One line longer than the whole cap evicts itself: there is no prefix of
 * it worth keeping, and half a line is not a line. */
static void test_store_drops_a_line_larger_than_the_cap(void)
{
	struct lsp_log_store s = { 0 };
	bool trimmed = false;

	CHECK(push(&s, "keep\n", 8, &trimmed));
	CHECK(
	    push(&s, "a much longer line than the cap allows\n", 8, &trimmed));
	CHECK(trimmed);
	CHECK(s.len == 0);
	lsp_log_store_reset(&s);
}

/* ------------------------------ the buffer ---------------------------- */

static void setup(void)
{
	free_all_rows();
	reset_current_buffer();
	memset(&editor, 0, sizeof(editor));
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
}

/* The slot holding the log, or -1: the same question a user asks with
 * `C-x b`, and the only handle this file has on the module's own state. */
static int log_slot(void)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename
		    && strcmp(buflist[i].filename, LSP_LOG_BUFFER_NAME) == 0) {
			return i;
		}
	}
	return -1;
}

/* Row `row` of the log as a NUL-terminated string, or "" -- the rows are
 * what a reader sees, which is what the assertions are about. */
static const char *log_row(int row)
{
	int slot = log_slot();

	if (slot < 0 || row < 0 || row >= buflist[slot].numrows) {
		return "";
	}
	return buflist[slot].row[row].chars;
}

/* Whether any row of the log holds `needle`. */
static bool log_contains(const char *needle)
{
	int slot = log_slot();
	int i;

	for (i = 0; slot >= 0 && i < buflist[slot].numrows; i++) {
		if (strstr(buflist[slot].row[i].chars, needle)) {
			return true;
		}
	}
	return false;
}

/* Created on the first line and not before, prefixed with the server that
 * said it, appended to afterwards -- and never selected: a log that stole
 * the window would be an interruption, and the whole point of it is that
 * it is a record. */
static void test_the_buffer_is_lazy_prefixed_and_never_selected(void)
{
	int current_before;

	setup();
	CHECK(log_slot() < 0);
	current_before = buf_current;

	lsp_log_line("clangd", "cannot find compile_commands.json",
	    strlen("cannot find compile_commands.json"));
	CHECK(log_slot() >= 0);
	CHECK(buf_current == current_before);
	CHECK(strcmp(log_row(0), "[clangd] cannot find compile_commands.json")
	    == 0);

	lsp_log_line("ty", "no reply to textDocument/definition after 30s",
	    strlen("no reply to textDocument/definition after 30s"));
	CHECK(strcmp(log_row(1),
		  "[ty] no reply to textDocument/definition after 30s")
	    == 0);
	/* Read-only, like every other buffer kg writes for the user. */
	CHECK(buflist[log_slot()].readonly);
}

/* Past the cap the buffer is rewritten from the store, so what is on
 * screen is what was kept: the first line written is gone, the last one is
 * there, and the buffer never grows past the store it mirrors. */
static void test_the_buffer_follows_the_stores_eviction(void)
{
	char line[128];
	char last[128];
	size_t written = 0;
	int i;

	setup();
	lsp_log_line("clangd", "FIRST-LINE", strlen("FIRST-LINE"));
	/* Well past LSP_LOG_MAX_BYTES of filler, in lines long enough that
	 * the count stays small. */
	last[0] = '\0';
	for (i = 0; written <= LSP_LOG_MAX_BYTES + 4096; i++) {
		int n = snprintf(line, sizeof(line),
		    "filler %06d ........................................", i);

		lsp_log_line("clangd", line, (size_t)n);
		snprintf(last, sizeof(last), "%s", line);
		written += (size_t)n + 10;
	}
	CHECK(log_slot() >= 0);
	CHECK(!log_contains("FIRST-LINE"));
	CHECK(strncmp(log_row(0), "[clangd] filler ", 16) == 0);
	CHECK(log_contains(last));
	/* The mirror is bounded by the store, which is bounded by the cap. */
	CHECK((size_t)buflist[log_slot()].numrows < LSP_LOG_MAX_BYTES / 16 + 2);
}

int main(void)
{
	RUN(test_store_keeps_what_fits);
	RUN(test_store_evicts_whole_lines_from_the_head);
	RUN(test_store_drops_a_line_larger_than_the_cap);
	RUN(test_the_buffer_is_lazy_prefixed_and_never_selected);
	RUN(test_the_buffer_follows_the_stores_eviction);
	return test_summary();
}
