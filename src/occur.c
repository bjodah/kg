/* occur.c — see occur.h. */

#include "occur.h"

#include "bufhandle.h"
#include "bufmgr.h"
#include "cmd.h"
#include "compile_nav.h"
#include "decor.h"
#include "def.h"
#include "localvars.h"
#include "marker.h"
#include "next_error.h"
#include "regex.h"
#include "visit.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Each command's name, as every message it prints spells it: the name a
 * user would type at M-x, so a refusal can be looked up from what it
 * said. */
#define OCCUR_WHO "occur"
#define OCCUR_WHO_GOTO "occur-mode-goto-occurrence"

/* How many matches one listing holds.  Bounded for the reason *xref*'s
 * results are: a two-letter regexp over a large buffer has tens of
 * thousands of matches, and a listing nobody scrolls to the end of is not
 * worth that many markers relocated on every edit to the source.  The cap
 * is visible in the listing when it is reached, so a reader is never
 * shown a complete-looking answer that is not one. */
#define OCCUR_MATCH_MAX 500

/* The longest regexp the prompt accepts.  Emacs' is unbounded; kg's
 * minibuffer readers all take a fixed buffer, and 256 bytes is past any
 * regexp this engine will compile. */
#define OCCUR_PATTERN_MAX 256

/* One match: where it is in the source, where it is in the listing, and
 * the marker that keeps the first of those true across edits.
 *
 * `marker_valid` is what tells a match whose marker was never created
 * apart from one whose marker has gone, the distinction compile_nav.c's
 * source markers need for the same reason: a zeroed handle and a deleted
 * one both resolve as gone, and only the flag says which of the two owns
 * a marker to delete. */
struct occur_match {
	int line; /* 0-based source row */
	int start; /* byte offset in that row's chars */
	int end;
	int row_col; /* byte offset of the match in the listing row */
	/* 1-based row of the listing it is printed on.  Not `row`: the
	 * mutation-gateway census reads `->row =` as a write to a buffer's
	 * row array, and this is a line number in a listing. */
	size_t listing_row;
	struct kg_marker_handle marker;
	bool marker_valid;
};

struct occur_run {
	struct occur_match matches[OCCUR_MATCH_MAX];
	size_t count;
	size_t row_count; /* listing rows, i.e. distinct source lines */
	bool truncated;
	bool too_complex;
	size_t rendered; /* bytes appended to the listing so far */
	struct kg_buffer_handle source_buffer;
	struct kg_buffer_handle occur_buffer;
};

static struct occur_run g_run;
/* Bumped by every rerun; a cursor from an earlier generation is treated
 * as never having visited anything (src/compile_nav.h). */
static uint32_t g_generation;
/* The command-owned cursor: next-error/previous-error's place in
 * g_run.matches, not a field of it.  The type and its step come from
 * compile_nav.h, whose header comment put them outside the compilation
 * store on purpose, for exactly this second store. */
static struct compile_nav_cursor g_cursor = { -1, 0 };

static struct minibuf_history occur_history;

/* ------------------------------- the store ---------------------------- */

/* Release the previous run and start a new generation.  The listing's own
 * markers and decorations need no releasing here: every rerun goes
 * through buf_prepare_special_text(), whose row adoption detaches
 * everything anchored in *Occur* (see buffer.c's kg_buffer_adopt_rows()).
 * The source-buffer markers are this module's to free -- they live in a
 * different buffer, which adoption never touches. */
static void occur_reset(struct kg_buffer_handle source)
{
	size_t i;

	for (i = 0; i < g_run.count; i++) {
		if (g_run.matches[i].marker_valid) {
			kg_marker_delete(g_run.matches[i].marker);
		}
	}
	memset(&g_run, 0, sizeof(g_run));
	g_run.source_buffer = source;

	g_generation++;
	g_cursor = (struct compile_nav_cursor) { -1, g_generation };
}

/* Record one match.  `first` says it is the first on its source line, and
 * so opens a new listing row.  False once the store is full, which stops
 * the scan: a listing that silently dropped a match in the middle would
 * be one whose rows no longer say where the next-error walk is. */
static bool occur_store_add(
    struct editor_buffer *b, int line, int start, int end, bool first)
{
	struct occur_match *m;
	size_t pos;

	if (g_run.count == OCCUR_MATCH_MAX) {
		g_run.truncated = true;
		return false;
	}
	if (first) {
		g_run.row_count++;
	}
	m = &g_run.matches[g_run.count++];
	*m = (struct occur_match) { 0 };
	m->line = line;
	m->start = start;
	m->end = end;
	m->listing_row = g_run.row_count;
	pos = buffer_row_col_to_position(b, line, start);
	m->marker = kg_marker_create(b, pos, KG_MARKER_GRAV_LEFT);
	m->marker_valid = m->marker.id != 0;
	return true;
}

/* Every match in one row, left to right, from `*col` onward.  The scan
 * position comes from kg_regex_next_offset() rather than from the match's
 * end: an empty match consumed nothing, and adding a byte to it lands
 * inside a glyph and never terminates (src/regex.h).  Returns false when
 * the row is done, with `*complex` set if the engine gave up on it --
 * that row may well hold a match nothing here can see, which is worth
 * saying out loud rather than reporting as an absence. */
static bool occur_scan_row(struct editor_buffer *b, const struct kg_regex *rx,
    int line, int *col, bool *first, bool *complex_out)
{
	erow *row = &b->row[line];
	struct kg_match m;
	int status;

	if (*col < 0 || *col > row->size) {
		return false;
	}
	status = kg_regex_match_forward(rx, row->chars, *col, &m);
	if (status == KG_REGEX_TOO_COMPLEX) {
		*complex_out = true;
		return false;
	}
	if (status != KG_REGEX_OK) {
		return false;
	}
	if (!occur_store_add(
		b, line, m.spans[0].start, m.spans[0].end, *first)) {
		return false;
	}
	*first = false;
	*col = kg_regex_next_offset(row->chars, row->size, &m.spans[0]);
	return true;
}

/* Whether `rx` matches anywhere in `b`, stopping at the first one.  Asked
 * before the store is touched, which is what lets a search that finds
 * nothing leave the previous listing, its markers and the next-error keys
 * exactly as they were -- Emacs kills its *Occur* buffer in that case,
 * and leaving the old answer intact is the more useful of the two. */
static bool occur_any_match(struct editor_buffer *b, const struct kg_regex *rx)
{
	struct kg_match m;
	int line;

	for (line = 0; line < b->numrows; line++) {
		if (kg_regex_match_forward(rx, b->row[line].chars, 0, &m)
		    == KG_REGEX_OK) {
			return true;
		}
	}
	return false;
}

/* Fill the store, in buffer order: rows top to bottom, matches within a
 * row left to right.  That is the order the listing prints and the order
 * next-error walks, so neither has to sort anything. */
static void occur_collect(struct editor_buffer *b, const struct kg_regex *rx)
{
	int line;

	for (line = 0; line < b->numrows; line++) {
		bool first = true;
		int col = 0;

		while (occur_scan_row(
		    b, rx, line, &col, &first, &g_run.too_complex)) {
			/* occur_scan_row() ends the row itself; the store
			 * running full ends the whole scan. */
		}
		if (g_run.truncated) {
			return;
		}
	}
}

/* ------------------------------ the listing --------------------------- */

/* Append to the listing, tracking the flat byte offset the next append
 * lands at.  That running total is what the decorations below are placed
 * against: the same bookkeeping compile.c does with its committed_len,
 * and for the same reason -- a position in a buffer being built cannot be
 * asked for after the fact without walking every row again. */
static bool occur_append(int slot, const char *text, size_t len)
{
	if (len == 0) {
		return true;
	}
	if (buf_append_special_text(slot, text, len) != 0) {
		return false;
	}
	g_run.rendered += len;
	return true;
}

/* Emacs' header, to the letter: "3 matches in 2 lines for "x" in buffer:
 * foo.c".  The line count appears only when it differs from the match
 * count, since saying both for one match per line is saying it twice. */
static bool occur_append_header(
    int slot, const char *pattern, const char *bufname)
{
	char lines[48] = "";
	char header[OCCUR_PATTERN_MAX + 256];
	int len;

	if (g_run.row_count != g_run.count) {
		snprintf(lines, sizeof(lines), " in %zu line%s",
		    g_run.row_count, g_run.row_count == 1 ? "" : "s");
	}
	len = snprintf(header, sizeof(header),
	    "%zu %s%s for \"%s\" in buffer: %s\n", g_run.count,
	    g_run.count == 1 ? "match" : "matches", lines, pattern, bufname);
	if (len < 0) {
		return false;
	}
	if ((size_t)len >= sizeof(header)) {
		len = (int)sizeof(header) - 1;
	}
	return occur_append(slot, header, (size_t)len);
}

/* One listing row: the line number, a colon, and the source line itself,
 * appended whole rather than through a fixed buffer -- a source line has
 * no length limit, and a truncated one would be a row whose highlights no
 * longer sit over the text they were measured against.  `*prefix_len`
 * receives the width of the number field, which is where a match's offset
 * inside the row is counted from. */
static bool occur_append_row(
    int slot, const erow *row, int line, int *prefix_len)
{
	char prefix[16];
	int len = snprintf(prefix, sizeof(prefix), "%7d:", line + 1);

	if (len < 0) {
		return false;
	}
	if ((size_t)len >= sizeof(prefix)) {
		len = (int)sizeof(prefix) - 1;
	}
	*prefix_len = len;
	return occur_append(slot, prefix, (size_t)len)
	    && occur_append(slot, row->chars, (size_t)row->size)
	    && occur_append(slot, "\n", 1);
}

/* Build (or rebuild) the listing from the store, and highlight every
 * match inside it.  Returns the listing's slot, or -1 when there was no
 * room for it.
 *
 * `text_syntax` for xref's reason: the compilation record carries no
 * highlighting at all, so reusing it would change nothing but the mode
 * line, which would then say "Compilation" in a buffer no compilation
 * produced.
 *
 * The highlight is a decoration rather than a syntax rule because it is
 * not a property of the text: it says where the answer to *this* question
 * is, and the same bytes in the same buffer stop being highlighted the
 * moment the next occur runs. */
static int occur_render(
    struct editor_buffer *b, const char *pattern, const char *bufname)
{
	int slot = buf_prepare_special_text(OCCUR_BUFFER_NAME, &text_syntax, 1);
	struct editor_buffer *ob;
	size_t row_start = 0;
	size_t printed = 0;
	int prefix_len = 0;
	size_t i;

	g_run.rendered = 0;
	if (slot < 0 || !occur_append_header(slot, pattern, bufname)) {
		return -1;
	}
	ob = buf_resolve(buf_handle(slot));
	for (i = 0; i < g_run.count; i++) {
		struct occur_match *m = &g_run.matches[i];

		if (m->listing_row != printed) {
			row_start = g_run.rendered;
			if (!occur_append_row(
				slot, &b->row[m->line], m->line, &prefix_len)) {
				return -1;
			}
			printed = m->listing_row;
		}
		m->row_col = prefix_len + m->start;
		if (ob) {
			size_t at = row_start + (size_t)m->row_col;

			(void)kg_decor_create(ob, at,
			    at + (size_t)(m->end - m->start),
			    KG_MARKER_GRAV_RIGHT, KG_MARKER_GRAV_LEFT,
			    KG_DECOR_FACE_MATCH, 0, true);
		}
	}
	if (g_run.truncated) {
		char note[64];
		int len = snprintf(note, sizeof(note),
		    "… truncated at %d matches\n", OCCUR_MATCH_MAX);

		if (len > 0 && (size_t)len < sizeof(note)) {
			(void)occur_append(slot, note, (size_t)len);
		}
	}
	return slot;
}

/* ------------------------------ navigation ---------------------------- */

/* Put point on `m` in the source buffer, which is already selected.
 *
 * The marker first: it followed the text through every edit made since
 * the search, so it is where the match now is rather than where it was.
 * The recorded line and column are the fallback, clamped to what the
 * buffer currently holds -- a source shortened since the search still has
 * somewhere to land, and it is better to be on the right line's end than
 * to refuse to move at all. */
static void occur_place_point(const struct occur_match *m)
{
	struct editor_buffer *b = bcur();
	size_t pos;
	int row, col;

	if (m->marker_valid
	    && kg_marker_resolve(m->marker, &pos) == KG_MARKER_OK
	    && buffer_position_to_row_col(b, pos, &row, &col) == 1) {
		editor_goto_line_direct(row + 1, col + 1);
		return;
	}
	row = m->line;
	if (row >= b->numrows) {
		row = b->numrows - 1;
	}
	if (row < 0) {
		return; /* an empty buffer has no line to land on */
	}
	col = m->start;
	if (col > b->row[row].size) {
		col = b->row[row].size;
	}
	editor_goto_line_direct(row + 1, col + 1);
}

/* Visit match `index`: leave the listing's window when there is another
 * one to land in, select the source buffer, place point, and move the
 * listing's own point to the row just visited so the two agree about
 * where the walk is. */
static void occur_visit(size_t index)
{
	int listing = buf_handle_slot(g_run.occur_buffer);

	if (!buf_resolve(g_run.source_buffer)) {
		editor_set_status_message(
		    OCCUR_WHO ": the source buffer is gone");
		return;
	}
	editor_visit_escape_window(g_run.occur_buffer);
	if (!buf_select(buf_handle_slot(g_run.source_buffer))) {
		return; /* refused under event pressure; still handled */
	}
	occur_place_point(&g_run.matches[index]);
	if (listing >= 0) {
		win_position_at_row(
		    listing, (int)g_run.matches[index].listing_row);
	}
}

static int occur_cursor_step(int step, bool *clamped)
{
	g_cursor = compile_nav_cursor_advance(
	    g_cursor, g_generation, g_run.count, step, clamped);
	return g_cursor.index;
}

/* next-error/previous-error, once occur owns the keys.  The shape is
 * compile_nav.c's nav_move() exactly -- prefix argument as a step count,
 * a message when it ran past an end, then the visit -- because the two
 * are the same command over two stores. */
static void occur_move(int direction)
{
	bool clamped;
	int step;

	if (g_run.count == 0) {
		editor_set_status_message("No occur matches");
		return;
	}
	step = editor.current_prefix.supplied ? editor.current_prefix.value : 1;
	(void)occur_cursor_step(step * direction, &clamped);
	if (clamped) {
		editor_set_status_message(
		    direction > 0 ? "No more matches" : "No earlier matches");
	}
	occur_visit((size_t)g_cursor.index);
}

static const struct next_error_source g_next_error_source
    = { "occur", occur_move };

/* ------------------------------ in the listing ------------------------ */

/* Whether point is in the listing this store describes.  The buffer
 * handle rather than the name: a *Occur* buffer the user killed and a new
 * buffer that took its slot are both called *Occur*, and only the handle
 * tells them apart. */
static bool occur_in_listing(void)
{
	return g_run.occur_buffer.id != 0
	    && buf_handle_slot(g_run.occur_buffer) == buf_current;
}

/* The first match on the listing row point is on.  Row 0 is the header
 * and anything past the last listing row is the truncation notice, so
 * both answer "no occurrence here" rather than the nearest one. */
static bool occur_index_at_point(size_t *out)
{
	int row = wcur()->rowoff + wcur()->cy;
	size_t i;

	if (row < 1 || (size_t)row > g_run.row_count) {
		return false;
	}
	for (i = 0; i < g_run.count; i++) {
		if (g_run.matches[i].listing_row == (size_t)row) {
			*out = i;
			return true;
		}
	}
	return false;
}

void editor_occur_goto_occurrence(int fd)
{
	size_t index;

	(void)fd;
	if (!occur_in_listing()) {
		editor_set_status_message(
		    OCCUR_WHO_GOTO ": not in " OCCUR_BUFFER_NAME);
		return;
	}
	if (!occur_index_at_point(&index)) {
		editor_set_status_message(
		    OCCUR_WHO_GOTO ": no occurrence on this line");
		return;
	}
	/* A visit is where the next-error walk now is, so that M-g n after a
	 * RET continues from the match just visited rather than from
	 * wherever the walk had reached (Emacs' next-error-found). */
	g_cursor = (struct compile_nav_cursor) { (int)index, g_generation };
	occur_visit(index);
}

/* n and p: move between the listing's rows without visiting anything.
 * Point lands on the row's first match rather than in its number field,
 * so the highlight under point is the thing the row is about. */
static void occur_step_line(const char *who, int direction)
{
	int row = wcur()->rowoff + wcur()->cy;
	long target = (long)row + direction;
	size_t index = 0;

	if (!occur_in_listing()) {
		editor_set_status_message("%s: not in " OCCUR_BUFFER_NAME, who);
		return;
	}
	if (target < 1 || (size_t)target > g_run.row_count) {
		editor_set_status_message(
		    direction > 0 ? "No more matches" : "No earlier matches");
		return;
	}
	editor_goto_line_direct((int)target + 1, 1);
	if (occur_index_at_point(&index)) {
		editor_goto_line_direct(
		    (int)target + 1, g_run.matches[index].row_col + 1);
	}
}

void editor_occur_next(int fd)
{
	(void)fd;
	occur_step_line("occur-next", 1);
}

void editor_occur_prev(int fd)
{
	(void)fd;
	occur_step_line("occur-prev", -1);
}

/* -------------------------------- the command ------------------------- */

/* Whether the pattern has an upper-case letter, and so should be matched
 * case-sensitively.  This is search.c's convention -- Emacs'
 * case-fold-search plus search-upper-case, which occur reads too -- and
 * it is spelled again here rather than shared because search.c keeps its
 * copy static and there is no search.h to put one in.  isupper() takes an
 * unsigned char, and says nothing useful about a byte >= 0x80 in the "C"
 * locale, which is the same thing it says for search.c. */
static int occur_has_upper(const char *pattern)
{
	size_t i;

	for (i = 0; pattern[i]; i++) {
		if (isupper((unsigned char)pattern[i])) {
			return 1;
		}
	}
	return 0;
}

int occur_run(const char *pattern)
{
	struct editor_buffer *b = bcur();
	struct kg_regex rx;
	char bufname[128];
	int slot;

	if (!pattern || !pattern[0]) {
		return -1;
	}
	/* Rebuilding the listing clears the buffer the rows are being read
	 * from, so a search of the listing itself would read freed rows.
	 * Emacs renames the old *Occur* out of the way; kg has no rename,
	 * and refusing is the honest half of that. */
	if (bcur()->filename
	    && strcmp(bcur()->filename, OCCUR_BUFFER_NAME) == 0) {
		editor_set_status_message(
		    OCCUR_WHO ": cannot search " OCCUR_BUFFER_NAME);
		return -1;
	}
	if (kg_regex_compile(
		&rx, pattern, occur_has_upper(pattern) ? 0 : KG_REGEX_ICASE)
	    != KG_REGEX_OK) {
		editor_set_status_message("Invalid regular expression");
		return -1;
	}
	if (!occur_any_match(b, &rx)) {
		editor_set_status_message("No matches for \"%s\"", pattern);
		return 0;
	}
	occur_reset(buf_handle(buf_current));
	occur_collect(b, &rx);
	buf_display_name(buf_current, bufname, sizeof(bufname));
	slot = occur_render(b, pattern, bufname);
	if (slot < 0) {
		editor_set_status_message(
		    OCCUR_WHO ": no room for the listing");
		return -1;
	}
	g_run.occur_buffer = buf_handle(slot);
	next_error_set_source(&g_next_error_source);
	win_display_buffer_other_window(slot);
	editor_set_status_message("%zu %s for \"%s\"%s — RET visits, q closes",
	    g_run.count, g_run.count == 1 ? "match" : "matches", pattern,
	    g_run.too_complex ? "; some lines were too complex" : "");
	return (int)g_run.count;
}

void editor_occur(int fd)
{
	char pattern[OCCUR_PATTERN_MAX];

	pattern[0] = '\0';
	if (editor_read_line_with_history(fd, "List lines matching regexp: ",
		pattern, sizeof(pattern), &occur_history)
		!= MINIBUF_ACCEPTED
	    || !pattern[0]) {
		return;
	}
	(void)occur_run(pattern);
}

/* ------------------------------ test seams ---------------------------- */

size_t occur_test_count(void) { return g_run.count; }

size_t occur_test_row_count(void) { return g_run.row_count; }

bool occur_test_match(
    size_t index, int *out_line, int *out_col, size_t *out_row)
{
	if (index >= g_run.count) {
		return false;
	}
	if (out_line) {
		*out_line = g_run.matches[index].line;
	}
	if (out_col) {
		*out_col = g_run.matches[index].start;
	}
	if (out_row) {
		*out_row = g_run.matches[index].listing_row;
	}
	return true;
}

uint32_t occur_test_generation(void) { return g_generation; }

int occur_test_cursor_index(void) { return g_cursor.index; }

int occur_test_cursor_step(int direction)
{
	bool clamped;

	return occur_cursor_step(direction, &clamped);
}
