/* Just-in-time spell checking behind src/spell.h's facade: the word
 * scanner and the per-frame highlighting.  The four spell-* commands
 * live in src/spell_cmd.c, which is the one file here that prompts,
 * edits and reads the dispatch prefix (lsp_req.c/lsp_edit.c and
 * dap_commands.c are split off for exactly that reason: a test binary
 * linking this half must not have to link the command layer with it).
 * Compiled in every configuration, like src/xref.c: the WITH_ENCHANT=0
 * build publishes nothing, so the refresh hook is unconditional.
 *
 * Checking is synchronous and in-process (the whole point of Enchant
 * over a subprocess checker, and of jinx over ispell): spell_update()
 * scans a bounded slice of rows per frame and resumes on the next one,
 * so a large buffer with spell-mode on costs a fixed slice of each
 * repaint rather than one long pause.  Navigation and correction scan
 * directly from point instead of reading the decorations, so they work
 * on rows the background pass has not reached yet.
 */

#include "spell.h"
#include "bufhandle.h"
#include "decor.h"
#include "def.h"
#include "marker.h"
#include "spell_internal.h"
#include "syntax.h"
#include "vgeom.h"

#include <stdio.h>
#include <string.h>

/* Incremental budget: rows rechecked per frame per buffer, and total
 * decorations published.  Past the cap a buffer's further misspellings
 * stay unpainted (navigation still finds them); a recheck after any
 * content, mode or language change republishes from scratch. */
#define SPELL_ROWS_PER_FRAME 100
#define SPELL_MAX_DECOR 256

/* A word the dictionaries cannot judge.  Digits name hex codes and
 * versions; an all-caps word past one letter names an acronym (a lone
 * "I" still goes to the dictionary); overlong runs are lines.  This is
 * jinx-exclude-regexps' default set, minus the URI/email shapes, which
 * need real regexps and are follow-up work. */
int spell_word_candidate(const char *word, size_t len)
{
	size_t i;
	int has_lower = 0, has_upper = 0;

	if (!word || len == 0 || len > SPELL_SCAN_WORD_MAX) {
		return 0;
	}
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)word[i];

		if (c >= '0' && c <= '9') {
			return 0;
		}
		if (c >= 'a' && c <= 'z') {
			has_lower = 1;
		} else if (c >= 'A' && c <= 'Z') {
			has_upper = 1;
		}
	}
	return !(has_upper && !has_lower && len > 1);
}

int spell_word_byte(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
	    || (c >= '0' && c <= '9') || c == '_' || c == '\'' || c >= 0x80;
}

/* Past one edge apostrophe (ASCII or U+2019): jinx strips both, so
 * "'quoted'" checks as `quoted' rather than flagging the quotes. */
static int spell_trim_left(const char *chars, int start, int end)
{
	while (start < end) {
		if (chars[start] == '\'') {
			start++;
		} else if (start + 3 <= end
		    && memcmp(chars + start, "\xe2\x80\x99", 3) == 0) {
			start += 3;
		} else {
			break;
		}
	}
	return start;
}

static int spell_trim_right(const char *chars, int start, int end)
{
	while (end > start) {
		if (chars[end - 1] == '\'') {
			end--;
		} else if (end - 3 >= start
		    && memcmp(chars + end - 3, "\xe2\x80\x99", 3) == 0) {
			end -= 3;
		} else {
			break;
		}
	}
	return end;
}

static int spell_face_ok(unsigned char hl)
{
	return hl == HL_COMMENT || hl == HL_MLCOMMENT || hl == HL_STRING;
}

/* Whether any render byte of the chars-space word [start, end) carries
 * a comment or string face -- the code-buffer rule.  Converted at this
 * seam (chars_to_render_col), never by indexing row->hl with chars
 * offsets (doc/coordinates.md row 10). */
static int spell_in_prose(struct erow *row,
    const struct kg_display_options *display, int start, int end)
{
	int render_start, render_end, j;

	if (!row->hl || row->rsize <= 0) {
		return 0;
	}
	render_start = chars_to_render_col(row, start, display);
	render_end = chars_to_render_col(row, end, display);
	if (render_start < 0) {
		render_start = 0;
	}
	if (render_end > row->rsize) {
		render_end = row->rsize;
	}
	for (j = render_start; j < render_end; j++) {
		if (spell_face_ok(row->hl[j])) {
			return 1;
		}
	}
	return 0;
}

/* One word's verdict as a span: trim, gate, judge, store.  Returns 1
 * when a span was stored. */
static int spell_scan_word(struct erow *row,
    const struct kg_display_options *display, int check_all,
    spell_is_ok_fn is_ok, void *ctx, int start, int end, struct spell_span *out)
{
	if (end <= start) {
		return 0;
	}
	if (!check_all && !spell_in_prose(row, display, start, end)) {
		return 0;
	}
	if (!spell_word_candidate(row->chars + start, (size_t)(end - start))) {
		return 0;
	}
	/* Nonzero is fine; spell_check_word()'s -1 (no check ran) counts
	 * as fine here because every caller gates on spell_available()
	 * first. */
	if (is_ok(row->chars + start, (size_t)(end - start), ctx)) {
		return 0;
	}
	out->start = start;
	out->end = end;
	return 1;
}

int spell_scan_row(struct erow *row, const struct kg_display_options *display,
    int check_all, spell_is_ok_fn is_ok, void *ctx, struct spell_span *out,
    int max)
{
	int i = 0, n = 0;

	if (!row || !row->chars || !display || !is_ok || !out || max <= 0) {
		return 0;
	}
	while (i < row->size) {
		int start, end;

		while (i < row->size
		    && !spell_word_byte((unsigned char)row->chars[i])) {
			i++;
		}
		start = spell_trim_left(row->chars, i, row->size);
		while (i < row->size
		    && spell_word_byte((unsigned char)row->chars[i])) {
			i++;
		}
		end = spell_trim_right(row->chars, start, i);
		if (n < max
		    && spell_scan_word(row, display, check_all, is_ok, ctx,
			start, end, &out[n])) {
			n++;
		}
	}
	return n;
}

/* ---- Per-frame highlighting ---- */

/* Below the search match and level with the git diagnostics: a
 * misspelling says something about the text whether or not point is on
 * it, and the thing just searched for is the thing being looked at. */
#define SPELL_DECOR_PRIORITY 0

static struct kg_decor_handle spell_decor[SPELL_MAX_DECOR];
static int spell_decor_count;

static struct {
	uint64_t buffer_id;
	uint64_t generation;
	uint64_t content;
	uint64_t layout;
	const struct editor_syntax *syntax;
	char lang[32];
	int next_row;
	int done;
	int valid;
} spell_state;

static int spell_decor_names_buffer(
    struct kg_decor_handle h, struct kg_buffer_handle b)
{
	return h.buffer.slot == b.slot && h.buffer.id == b.id
	    && h.buffer.generation == b.generation;
}

/* Retire every misspelling published for `b', and every one whose buffer
 * has gone in the meantime -- gitdiag_drop()'s rule, for its reason. */
void spell_drop(struct editor_buffer *b)
{
	struct kg_buffer_handle bh = buf_handle_of(b);
	int i, kept = 0;

	for (i = 0; i < spell_decor_count; i++) {
		if (spell_decor_names_buffer(spell_decor[i], bh)
		    || kg_decor_resolve(spell_decor[i], NULL) != KG_DECOR_OK) {
			kg_decor_delete(spell_decor[i]);
			continue;
		}
		spell_decor[kept++] = spell_decor[i];
	}
	spell_decor_count = kept;
	if (spell_state.valid && spell_state.buffer_id == b->id) {
		spell_state.valid = 0;
	}
}

/* Prose buffers check every word; code buffers only check comments and
 * strings (the face rule above).  Text and Markdown are prose; every
 * other mode is code. */
int spell_check_all(const struct editor_buffer *b)
{
	return !b->syntax || b->syntax->id == KG_MODE_TEXT
	    || b->syntax->id == KG_MODE_MARKDOWN;
}

int spell_dict_ok(const char *word, size_t len, void *ctx)
{
	(void)ctx;
	return spell_check_word(word, len);
}

static void spell_publish(
    struct editor_buffer *b, int row, const struct spell_span *span)
{
	size_t start, end;
	struct kg_decor_handle h;

	if (spell_decor_count >= SPELL_MAX_DECOR) {
		return;
	}
	start = buffer_row_col_to_position(b, row, span->start);
	end = buffer_row_col_to_position(b, row, span->end);
	h = kg_decor_create(b, start, end, KG_MARKER_GRAV_RIGHT,
	    KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_SPELL, SPELL_DECOR_PRIORITY,
	    true);
	if (h.id != 0) {
		spell_decor[spell_decor_count++] = h;
	}
}

/* Whether what is published for `b' was computed from exactly this
 * buffer, content, mode and language.  The language is the cached
 * dictionary's, not Lisp's: comparing through spell_language() would
 * bill every frame an interpreter read even when nothing spell-related
 * is on screen. */
static int spell_state_current(const struct editor_buffer *b)
{
	return spell_state.valid && spell_state.buffer_id == b->id
	    && spell_state.generation == b->generation
	    && spell_state.content == b->content_generation
	    && spell_state.layout == b->layout_generation
	    && spell_state.syntax == b->syntax
	    && strcmp(spell_state.lang, spell_open_language()) == 0;
}

static void spell_state_begin(struct editor_buffer *b)
{
	spell_state.buffer_id = b->id;
	spell_state.generation = b->generation;
	spell_state.content = b->content_generation;
	spell_state.layout = b->layout_generation;
	spell_state.syntax = b->syntax;
	snprintf(spell_state.lang, sizeof(spell_state.lang), "%s",
	    spell_open_language());
	spell_state.next_row = 0;
	spell_state.done = b->numrows <= 0;
	spell_state.valid = 1;
}

static void spell_advance(struct editor_buffer *b)
{
	int check_all = spell_check_all(b);
	int stop = spell_state.next_row + SPELL_ROWS_PER_FRAME;
	int row;

	if (stop > b->numrows) {
		stop = b->numrows;
	}
	for (row = spell_state.next_row; row < stop; row++) {
		struct spell_span spans[SPELL_NAV_SPANS];
		int n, k;

		n = spell_scan_row(&b->row[row], &b->display, check_all,
		    spell_dict_ok, NULL, spans, SPELL_NAV_SPANS);
		for (k = 0; k < n; k++) {
			spell_publish(b, row, &spans[k]);
		}
	}
	spell_state.next_row = stop;
	spell_state.done = stop >= b->numrows;
}

static void spell_update_buffer(struct editor_buffer *b, int available)
{
	if (!b || !b->active) {
		return;
	}
	if (!b->spell_mode || !available) {
		spell_drop(b);
		return;
	}
	if (!spell_state_current(b)) {
		spell_drop(b);
		spell_state_begin(b);
	}
	if (!spell_state.done) {
		spell_advance(b);
	}
}

void spell_update(void)
{
	int i, available = -1;
	struct editor_buffer *b;

	/* `available' is resolved lazily and at most once: a frame with
	 * no spell-mode buffer on screen must not bill an interpreter
	 * read (or intern a name) for a feature nobody turned on, and a
	 * WITH_ENCHANT=0 frame must never reach one at all.  Retiring is
	 * free either way -- spell_drop() touches no interpreter -- so a
	 * buffer whose mode just went off is still cleaned up. */
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			continue;
		}
		b = win_buffer(&winlist[i]);
		if (!b || !b->active) {
			continue;
		}
		if (!b->spell_mode) {
			spell_drop(b);
			continue;
		}
		if (available < 0) {
			available = spell_available();
		}
		spell_update_buffer(b, available);
	}
}

int spell_mode_on(const struct editor_buffer *b)
{
	return b != NULL && b->spell_mode;
}
