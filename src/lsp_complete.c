/* lsp_complete.c — see lsp_complete.h.
 *
 * Compiled in both configurations, the way src/xref.c is.  The three
 * pure helpers below the includes are outside the KG_USE_LSP guard on
 * purpose: they are string arithmetic with no protocol in them, and a
 * header that declares a function only half the builds define is a
 * header that lies.
 */

#include "lsp_complete.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------ pure helpers -------------------------- */

static const char *lsp_complete_key(const struct lsp_complete_item *item)
{
	if (item->sort_key && item->sort_key[0]) {
		return item->sort_key;
	}
	return item->label ? item->label : "";
}

static int lsp_complete_cmp(const void *a, const void *b)
{
	const struct lsp_complete_item *x = a;
	const struct lsp_complete_item *y = b;
	int order = strcmp(lsp_complete_key(x), lsp_complete_key(y));

	if (order != 0) {
		return order;
	}
	return strcmp(x->label ? x->label : "", y->label ? y->label : "");
}

void lsp_complete_sort(struct lsp_complete_item *items, size_t n)
{
	if (n > 1) {
		qsort(items, n, sizeof(*items), lsp_complete_cmp);
	}
}

bool lsp_complete_matches(const char *text, const char *prefix, size_t len)
{
	if (!text) {
		return false;
	}
	if (len == 0) {
		return true;
	}
	return strlen(text) >= len && memcmp(text, prefix, len) == 0;
}

size_t lsp_complete_common_prefix(const char *const *texts, size_t n)
{
	size_t len;
	size_t i;

	if (n == 0 || !texts[0]) {
		return 0;
	}
	len = strlen(texts[0]);
	for (i = 1; i < n; i++) {
		size_t k = 0;

		while (k < len && texts[i][k] && texts[i][k] == texts[0][k]) {
			k++;
		}
		len = k;
	}
	/* Never half a character: a continuation byte on its own is a byte
	 * the display would have to stand in for, and inserting one would
	 * be inserting text no candidate contains. */
	while (len > 0 && ((unsigned char)texts[0][len] & 0xC0) == 0x80) {
		len--;
	}
	return len;
}

#ifdef KG_USE_LSP

#include "bufhandle.h"
#include "dabbrev.h"
#include "def.h"
#include "edit.h"
#include "event.h"
#include "json.h"
#include "lsp_edit.h"
#include "lsp_req.h"
#include "lsp_sync.h"

#include <stdio.h>

struct kg_json_value;

/* The name every message this prints spells. */
#define COMPLETE_WHO "completion-at-point"

/* How wide a listing row may get.  A label and a detail are a server's
 * text; the row is cut rather than allowed to grow without bound, and
 * the bytes that reach it are neutralised first (comp_sanitise). */
#define COMPLETE_ROW_MAX 160

/* One answer, alive only for the callback that reads it: the items, and
 * the range the server said it was completing.
 *
 * The range is the first `textEdit` one seen.  A server sends the same
 * range on every item of one answer -- it is where the completion goes,
 * not a property of the candidate -- and taking the first is what lets
 * the prefix be the server's idea of it rather than kg's guess. */
static struct {
	struct lsp_complete_item items[LSP_COMPLETE_MAX_ITEMS];
	size_t count;
	size_t raw; /* items the answer held, readable or not */
	bool have_range;
	struct lsp_edit_range range;
} g_comp;

/* Where the completion goes: the row point is on, and the byte columns
 * it begins and ends at (chars space, doc/coordinates.md).
 *
 * `line`, not `row`: the mutation-gateway census reads `->row =` as a
 * write to a buffer's row array, and this is a line number.  src/occur.c
 * spells its listing row the same way and for the same reason. */
struct comp_target {
	int line;
	int start;
	int end;
};

/* What the listing now on screen is a listing of -- Emacs'
 * completion-in-region--data, spelled for one buffer and one line.  This
 * outlives an answer (g_comp above is freed at the end of the callback
 * that read it); it is what lsp_complete_post_command() consults to
 * decide the listing has stopped being about where point is. */
static struct {
	bool showing;
	struct kg_buffer_handle listing; /* the *Completions* buffer */
	struct kg_buffer_handle source; /* the buffer completed in */
	int line;
	int start;
} g_listing;

static void comp_reset(void)
{
	size_t i;

	for (i = 0; i < g_comp.count; i++) {
		free(g_comp.items[i].label);
		free(g_comp.items[i].insert);
		free(g_comp.items[i].sort_key);
		free(g_comp.items[i].detail);
	}
	memset(&g_comp, 0, sizeof(g_comp));
}

/* ------------------------------ the answer ---------------------------- */

/* The text edit a candidate carries, in either of its two shapes: a
 * plain TextEdit has a `range`, an InsertReplaceEdit has `insert` and
 * `replace` and kg takes the insert one -- replacing more than what was
 * typed is a completion that eats the text after point. */
static const struct kg_json_value *comp_edit_range(
    const struct kg_json_value *edit)
{
	const struct kg_json_value *range = kg_json_get(edit, "range");

	if (kg_json_kind_of(range) == KG_JSON_OBJECT) {
		return range;
	}
	return kg_json_get(edit, "insert");
}

static void comp_note_range(const struct kg_json_value *edit)
{
	struct lsp_edit_range range;

	if (g_comp.have_range
	    || !lsp_edit_range_read(comp_edit_range(edit), &range)) {
		return;
	}
	g_comp.range = range;
	g_comp.have_range = true;
}

/* Copy the four strings a candidate is made of.  False -- with nothing
 * stored -- when the item has no label, when the store is full, or when
 * a copy could not be made. */
static bool comp_add(const struct kg_json_value *node)
{
	const struct kg_json_value *edit = kg_json_get(node, "textEdit");
	const char *label = kg_json_str(kg_json_get(node, "label"), NULL);
	const char *insert = kg_json_str(kg_json_get(edit, "newText"), NULL);
	const char *sort = kg_json_str(kg_json_get(node, "sortText"), NULL);
	const char *detail = kg_json_str(kg_json_get(node, "detail"), NULL);
	struct lsp_complete_item *item;

	if (!label || !label[0] || g_comp.count == LSP_COMPLETE_MAX_ITEMS) {
		return false;
	}
	if (!insert) {
		insert = kg_json_str(kg_json_get(node, "insertText"), NULL);
	}
	item = &g_comp.items[g_comp.count];
	*item = (struct lsp_complete_item) { 0 };
	item->label = strdup(label);
	item->insert = strdup(insert ? insert : label);
	item->sort_key = strdup(sort ? sort : label);
	item->detail = detail ? strdup(detail) : NULL;
	if (!item->label || !item->insert || !item->sort_key) {
		free(item->label);
		free(item->insert);
		free(item->sort_key);
		free(item->detail);
		return false;
	}
	comp_note_range(edit);
	g_comp.count++;
	return true;
}

/* Read the whole answer.  A server sends either a CompletionList, whose
 * items are under `items`, or the bare array; `null` is how it says it
 * has nothing. */
static void comp_collect(const struct kg_json_value *result)
{
	const struct kg_json_value *items = result;
	size_t i;

	comp_reset();
	if (kg_json_kind_of(result) == KG_JSON_OBJECT) {
		items = kg_json_get(result, "items");
	}
	g_comp.raw = kg_json_len(items);
	for (i = 0; i < g_comp.raw; i++) {
		(void)comp_add(kg_json_at(items, i));
	}
}

/* ------------------------------ the target ---------------------------- */

/* What the completion replaces.  The server's own range is preferred --
 * it knows that `foo.` completes past the dot and kg's word scanner does
 * not -- and the word before point is the fallback, which is exactly the
 * prefix M-/ expands (src/dabbrev.h).  A range that starts after point,
 * or on another line, is not one kg can act on and is ignored. */
static bool comp_target_at(const struct lsp_req_answer *answer,
    const struct editor_buffer *b, struct comp_target *out)
{
	struct dabbrev_pos at;
	struct dabbrev_pos start;
	const erow *row;

	if (answer->point.row < 0 || answer->point.row >= b->numrows) {
		return false;
	}
	row = &b->row[answer->point.row];
	out->line = answer->point.row;
	out->end
	    = answer->point.col > row->size ? row->size : answer->point.col;
	out->start = out->end;
	if (g_comp.have_range && g_comp.range.start_line == out->line) {
		size_t col = lsp_pos_decode(answer->encoding, row->chars,
		    (size_t)row->size, (size_t)g_comp.range.start_char);

		if ((int)col <= out->end) {
			out->start = (int)col;
			return true;
		}
	}
	at.row = out->line;
	at.col = out->end;
	if (dabbrev_abbrev_before(b->row, b->numrows, at, &start) > 0) {
		out->start = start.col;
	}
	return true;
}

/* The candidates that complete what is there, newest filtering rule
 * first: a server is entitled to return its whole set and leave the
 * filtering to the client, and several do. */
static size_t comp_select(
    const char *prefix, size_t len, size_t *index, size_t max)
{
	size_t n = 0;
	size_t i;

	for (i = 0; i < g_comp.count && n < max; i++) {
		if (lsp_complete_matches(g_comp.items[i].insert, prefix, len)) {
			index[n++] = i;
		}
	}
	return n;
}

/* ------------------------------ the listing --------------------------- */

/* One row's worth of a server's text, with nothing in it that a terminal
 * could read as anything but text: control bytes become spaces, the rest
 * is passed through for display_glyph_at() to decide about (src/width.h).
 */
static void comp_sanitise(char *out, size_t size, const char *text)
{
	size_t n = 0;

	while (text && text[n] && n + 1 < size) {
		unsigned char c = (unsigned char)text[n];

		out[n] = c < 0x20 || c == 0x7F ? ' ' : text[n];
		n++;
	}
	out[n] = '\0';
}

static bool comp_append_row(int slot, const struct lsp_complete_item *item)
{
	char label[COMPLETE_ROW_MAX];
	char detail[COMPLETE_ROW_MAX];
	char line[2 * COMPLETE_ROW_MAX + 8];
	int len;

	comp_sanitise(label, sizeof(label), item->label);
	comp_sanitise(detail, sizeof(detail), item->detail);
	len = snprintf(line, sizeof(line), "%s%s%s\n", label,
	    detail[0] ? "  " : "", detail);
	if (len < 0) {
		return false;
	}
	if ((size_t)len >= sizeof(line)) {
		len = (int)sizeof(line) - 1;
	}
	return buf_append_special_text(slot, line, (size_t)len) == 0;
}

/* Build the listing and show it beside the buffer being completed in,
 * leaving point where it was.  The listing is passive: it has no mode
 * map of its own, so there is nothing to press in it -- Emacs' RET on a
 * completion is a keymap kg has not given this buffer.  It is a buffer
 * like any other and C-x 0 closes its window; what it also does now is
 * close itself once point leaves the field it is a listing of, which is
 * lsp_complete_post_command() below. */
static void comp_show(
    const struct comp_target *target, const size_t *index, size_t n)
{
	int slot = buf_prepare_special_text(
	    LSP_COMPLETE_BUFFER_NAME, &text_syntax, 1);
	char header[64];
	int len;
	size_t i;

	if (slot < 0) {
		editor_set_status_message(
		    COMPLETE_WHO ": no room for the listing");
		return;
	}
	len = snprintf(header, sizeof(header), "%zu completions\n", n);
	if (len > 0
	    && buf_append_special_text(slot, header, (size_t)len) != 0) {
		return;
	}
	for (i = 0; i < n; i++) {
		if (!comp_append_row(slot, &g_comp.items[index[i]])) {
			return;
		}
	}
	win_display_buffer_other_window(slot);
	g_listing.showing = true;
	g_listing.listing = buf_handle(slot);
	g_listing.source = buf_handle(buf_current);
	g_listing.line = target->line;
	g_listing.start = target->start;
	editor_set_status_message(
	    "%zu completions — see " LSP_COMPLETE_BUFFER_NAME, n);
}

/* ------------------------------ the lifetime -------------------------- */

/* Where the field point is in begins, in chars space: the word before
 * point, or point itself when there is no word there.  A completion
 * offered for an empty prefix -- after a `.`, where the server's own
 * range starts and ends at point -- has a field too, and it begins where
 * it was asked for; without this second answer such a listing would close
 * on the very next key, which is the one case a bare word scan gets
 * wrong. */
static int comp_field_start(struct editor_buffer *b, int line, int col)
{
	struct dabbrev_pos at = { .row = line, .col = col };
	struct dabbrev_pos start;

	if (dabbrev_abbrev_before(b->row, b->numrows, at, &start) > 0) {
		return start.col;
	}
	return col;
}

/* Whether the listing is still about where point is: the three clauses
 * completion-in-region--postch has, in its order. */
static bool comp_listing_current(void)
{
	struct editor_buffer *b;
	int line;
	int col;

	/* Standing in the listing keeps it, the way Emacs declines to hide
	 * *Completions* when it is the selected window's buffer. */
	if (buf_handle_slot(g_listing.listing) == buf_current) {
		return true;
	}
	if (buf_handle_slot(g_listing.source) != buf_current) {
		return false;
	}
	b = bcur();
	line = editor_current_filerow();
	if (line != g_listing.line || line >= b->numrows) {
		return false;
	}
	col = editor_current_filecol_in_row(&b->row[line]);
	if (col < g_listing.start) {
		return false;
	}
	return comp_field_start(b, line, col) == g_listing.start;
}

void lsp_complete_post_command(void)
{
	int slot;

	if (!g_listing.showing) {
		return;
	}
	slot = buf_handle_slot(g_listing.listing);
	if (slot < 0) {
		/* Killed from under us; there is no window to take down. */
		g_listing.showing = false;
		return;
	}
	if (comp_listing_current()) {
		return;
	}
	win_undisplay_buffer(slot);
	g_listing.showing = false;
}

/* ------------------------------ the insertion ------------------------- */

/* Put `len` bytes in place of what is being completed, as one edit: one
 * undo record, so C-x u takes the whole completion back. */
static bool comp_insert(
    const struct comp_target *target, const char *text, size_t len)
{
	if (!editor_row_replace_range(target->line, target->start,
		target->end - target->start, text, (int)len, KG_EDIT_USER)) {
		editor_set_status_message(
		    COMPLETE_WHO ": the buffer refused the completion");
		return false;
	}
	editor_cursor_goto(target->line, target->start + (int)len);
	return true;
}

/* Emacs' completion-at-point, in its three outcomes. */
static void comp_act(const struct comp_target *target, const size_t *index,
    size_t n, size_t typed)
{
	const char *texts[LSP_COMPLETE_MAX_ITEMS] = { NULL };
	size_t common;
	size_t i;

	if (n == 0) {
		return; /* the caller reports "No completions" */
	}
	if (n == 1) {
		const char *text = g_comp.items[index[0]].insert;

		if (comp_insert(target, text, strlen(text))) {
			editor_set_status_message("Sole completion");
		}
		return;
	}
	for (i = 0; i < n; i++) {
		texts[i] = g_comp.items[index[i]].insert;
	}
	common = lsp_complete_common_prefix(texts, n);
	if (common > typed) {
		if (comp_insert(target, texts[0], common)) {
			editor_set_status_message("Complete, but not unique");
		}
		return;
	}
	comp_show(target, index, n);
}

/* ------------------------------ the reply ----------------------------- */

/* Whether the answer is one to act on: the buffer it was asked about is
 * still the one point is in, it still says what the server was answering
 * about, and no prompt is up.  Completion inserts text at point, so an
 * answer that arrived after a buffer switch is an answer about somewhere
 * else -- and one that arrived after the user kept typing is an answer
 * about text that is no longer there.  Emacs' completion-at-point cannot
 * race this way at all: it computes and consumes its collection inside
 * one command.  kg's runs from lsp_poll(), so the refusal is the same
 * check src/lsp_edit.h makes for a rename, spelled for one buffer. */
static bool comp_still_here(const struct lsp_req_answer *answer)
{
	if (kg_event_prompt_active()) {
		editor_set_status_message(COMPLETE_WHO ": a prompt is open");
		return false;
	}
	if (!answer->point.resolved
	    || buf_handle_slot(answer->point.buffer) != buf_current) {
		editor_set_status_message(COMPLETE_WHO ": the buffer moved on");
		return false;
	}
	if (answer->sent_generation_valid
	    && bcur()->content_generation != answer->sent_generation) {
		editor_set_status_message(COMPLETE_WHO
		    ": the buffer changed while the server was answering");
		return false;
	}
	return true;
}

static void comp_answer(const struct lsp_req_answer *answer)
{
	size_t index[LSP_COMPLETE_MAX_ITEMS];
	struct comp_target target;
	struct editor_buffer *b;
	size_t typed;
	size_t n;

	if (!comp_still_here(answer)) {
		return;
	}
	b = bcur();
	comp_collect(answer->result);
	if (g_comp.count == 0 || !comp_target_at(answer, b, &target)) {
		editor_set_status_message("No completions");
		comp_reset();
		return;
	}
	typed = (size_t)(target.end - target.start);
	lsp_complete_sort(g_comp.items, g_comp.count);
	n = comp_select(b->row[target.line].chars + target.start, typed, index,
	    LSP_COMPLETE_MAX_ITEMS);
	if (n == 0) {
		editor_set_status_message("No completions");
	} else {
		comp_act(&target, index, n, typed);
	}
	comp_reset();
}

/* ------------------------------ the command --------------------------- */

void editor_completion_at_point(int fd)
{
	(void)fd;
	if (lsp_req_send_position(COMPLETE_WHO, "textDocument/completion", NULL,
		comp_answer, NULL, NULL)) {
		editor_set_status_message("Completing...");
	}
}

#else /* !KG_USE_LSP */

#include "def.h"

void editor_completion_at_point(int fd)
{
	(void)fd;
	editor_set_status_message("kg was built without LSP support");
}

/* No listing without a server to build one from. */
void lsp_complete_post_command(void) { }

#endif /* KG_USE_LSP */
