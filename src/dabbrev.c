/* ==================== Dynamic abbreviation expansion ===================== */

#include <stdlib.h>
#include <string.h>

#include "cmdstate.h"
#include "dabbrev.h"
#include "def.h"
#include "edit.h"

/* What kg already calls a word: ASCII alphanumerics and '_', the same
 * set M-f and M-d move over (src/word.c).  ASCII by definition, so it is
 * an ascii_is_* test rather than a <ctype.h> one. */
static int dabbrev_word_char(int c)
{
	return ascii_is_digit(c) || (c >= 'a' && c <= 'z')
	    || (c >= 'A' && c <= 'Z') || c == '_';
}

/* The byte length of row `r`, or -1 when the array has no such row. */
static int dabbrev_rowsize(const erow *rows, int numrows, int r)
{
	if (!rows || r < 0 || r >= numrows) {
		return -1;
	}
	return rows[r].size;
}

int dabbrev_word_at(const erow *rows, int numrows, struct dabbrev_pos at)
{
	int size = dabbrev_rowsize(rows, numrows, at.row);
	const char *s;
	int n = 0;

	if (at.col < 0 || at.col >= size) {
		return 0;
	}
	s = rows[at.row].chars;
	if (!dabbrev_word_char((unsigned char)s[at.col])) {
		return 0;
	}
	if (at.col > 0 && dabbrev_word_char((unsigned char)s[at.col - 1])) {
		return 0;
	}
	while (at.col + n < size
	    && dabbrev_word_char((unsigned char)s[at.col + n])) {
		n++;
	}
	return n;
}

int dabbrev_abbrev_before(const erow *rows, int numrows, struct dabbrev_pos at,
    struct dabbrev_pos *start)
{
	int size = dabbrev_rowsize(rows, numrows, at.row);
	const char *s;
	int col;

	if (size < 0) {
		return 0;
	}
	if (at.col > size) {
		at.col = size;
	}
	s = rows[at.row].chars;
	col = at.col;
	while (col > 0 && dabbrev_word_char((unsigned char)s[col - 1])) {
		col--;
	}
	*start = (struct dabbrev_pos) { at.row, col };
	return at.col - col;
}

/* The length of the candidate beginning exactly at `at`, or 0 when there
 * is none there.  Longer than the abbreviation, not merely equal to it:
 * an occurrence of the abbreviation itself expands to nothing, which is
 * what makes "foo bar" offer no expansion for "foo". */
static int dabbrev_candidate_at(const erow *rows, int numrows,
    struct dabbrev_pos at, const char *abbrev, int len)
{
	int n = dabbrev_word_at(rows, numrows, at);

	if (n <= len) {
		return 0;
	}
	return memcmp(rows[at.row].chars + at.col, abbrev, (size_t)len) == 0
	    ? n
	    : 0;
}

int dabbrev_search_backward(const erow *rows, int numrows,
    struct dabbrev_pos from, const char *abbrev, int len,
    struct dabbrev_pos *found)
{
	struct dabbrev_pos p = from;
	int n;

	if (len <= 0) {
		return 0;
	}
	if (p.row >= numrows) {
		p.row = numrows - 1;
		p.col = dabbrev_rowsize(rows, numrows, p.row);
	}
	/* Every row is scanned from the column before where the previous
	 * one's scan started -- from.col on `from`'s row, the end on each
	 * earlier row -- so the occurrence at `from` is never a candidate
	 * for its own expansion. */
	for (; p.row >= 0; p.row--) {
		for (p.col--; p.col >= 0; p.col--) {
			n = dabbrev_candidate_at(rows, numrows, p, abbrev, len);
			if (n > 0) {
				*found = p;
				return n;
			}
		}
		p.col = dabbrev_rowsize(rows, numrows, p.row - 1);
	}
	return 0;
}

int dabbrev_search_forward(const erow *rows, int numrows,
    struct dabbrev_pos from, const char *abbrev, int len,
    struct dabbrev_pos *found)
{
	struct dabbrev_pos p = from;
	int size;
	int n;

	if (len <= 0) {
		return 0;
	}
	if (p.row < 0) {
		p.row = 0;
		p.col = 0;
	}
	for (; p.row < numrows; p.row++, p.col = 0) {
		size = dabbrev_rowsize(rows, numrows, p.row);
		for (; p.col < size; p.col++) {
			n = dabbrev_candidate_at(rows, numrows, p, abbrev, len);
			if (n > 0) {
				*found = p;
				return n;
			}
		}
	}
	return 0;
}

/* ---- One cycle of M-/ ----
 *
 * Everything below lives from the first M-/ to the first keystroke that
 * runs some other command.  Nothing here watches the keyboard for that:
 * cmd_transient_value() answers "did I run last" (src/cmdstate.h), and
 * answering no is what starts a fresh cycle.
 *
 * `back` and `fwd` are where each direction resumes.  Backward positions
 * are all strictly before `start`, so an expansion never moves them;
 * forward ones on `start.row` shift by what the expansion added, which
 * dabbrev_apply() is the one place that knows. */
static struct {
	char *abbrev; /* what the user typed, NUL-terminated */
	int abbrev_len;
	struct dabbrev_pos start; /* where the abbreviation begins */
	int cur_len; /* bytes standing in its place right now */
	struct dabbrev_pos back;
	struct dabbrev_pos fwd;
	int back_done;
	/* Every expansion already shown, so a word the buffer holds twice
	 * is offered once. */
	char **tried;
	int ntried;
} dab;

void dabbrev_reset(void)
{
	int i;

	for (i = 0; i < dab.ntried; i++) {
		free(dab.tried[i]);
	}
	free(dab.tried);
	free(dab.abbrev);
	memset(&dab, 0, sizeof(dab));
}

/* Remember `len` bytes as shown.  On success the copy is
 * dab.tried[dab.ntried - 1] and outlives the row array, which is what
 * lets the caller hand it straight to the edit gateway. */
static int dabbrev_remember(const char *s, int len)
{
	char **grown;
	char *copy;

	grown = realloc(dab.tried, (size_t)(dab.ntried + 1) * sizeof(*grown));
	if (!grown) {
		return 0;
	}
	dab.tried = grown;
	copy = malloc((size_t)len + 1);
	if (!copy) {
		return 0;
	}
	memcpy(copy, s, (size_t)len);
	copy[len] = '\0';
	dab.tried[dab.ntried++] = copy;
	return 1;
}

static int dabbrev_seen(const char *s, int len)
{
	int i;

	for (i = 0; i < dab.ntried; i++) {
		if ((int)strlen(dab.tried[i]) == len
		    && memcmp(dab.tried[i], s, (size_t)len) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Start a cycle at point.  Returns 0, having said why, when there is no
 * abbreviation before point to expand. */
static int dabbrev_begin(void)
{
	struct dabbrev_pos at;
	int len;

	dabbrev_reset();
	at.row = editor_current_filerow_or_eof();
	at.col = editor_current_filecol();
	len = dabbrev_abbrev_before(
	    bcur()->row, bcur()->numrows, at, &dab.start);
	if (len <= 0) {
		editor_set_status_message(
		    "No possible abbreviation preceding point");
		return 0;
	}
	dab.abbrev = malloc((size_t)len + 1);
	if (!dab.abbrev) {
		return 0;
	}
	memcpy(dab.abbrev, bcur()->row[dab.start.row].chars + dab.start.col,
	    (size_t)len);
	dab.abbrev[len] = '\0';
	dab.abbrev_len = len;
	dab.cur_len = len;
	dab.back = dab.start;
	/* Forward scanning starts past the whole word point sits in, not
	 * past point: expanding "x" in "x|tra" must not offer the "xtra"
	 * it is standing in the middle of. */
	dab.fwd = dab.start;
	dab.fwd.col += dabbrev_word_at(bcur()->row, bcur()->numrows, dab.start);
	return 1;
}

/* The next expansion this cycle has not shown, or 0 when both directions
 * are spent. */
static int dabbrev_next(struct dabbrev_pos *at)
{
	const erow *rows = bcur()->row;
	int numrows = bcur()->numrows;
	int n;

	while (!dab.back_done) {
		n = dabbrev_search_backward(
		    rows, numrows, dab.back, dab.abbrev, dab.abbrev_len, at);
		if (n == 0) {
			dab.back_done = 1;
			break;
		}
		dab.back = *at;
		if (!dabbrev_seen(rows[at->row].chars + at->col, n)) {
			return n;
		}
	}
	for (;;) {
		n = dabbrev_search_forward(
		    rows, numrows, dab.fwd, dab.abbrev, dab.abbrev_len, at);
		if (n == 0) {
			return 0;
		}
		dab.fwd = *at;
		dab.fwd.col++;
		if (!dabbrev_seen(rows[at->row].chars + at->col, n)) {
			return n;
		}
	}
}

/* Put the candidate at `at` in place of whatever stands there now. */
static void dabbrev_apply(struct dabbrev_pos at, int len)
{
	const char *text;

	if (!dabbrev_remember(bcur()->row[at.row].chars + at.col, len)) {
		return;
	}
	text = dab.tried[dab.ntried - 1];
	if (!editor_row_replace_range(dab.start.row, dab.start.col, dab.cur_len,
		text, len, KG_EDIT_USER)) {
		return;
	}
	if (dab.fwd.row == dab.start.row) {
		dab.fwd.col += len - dab.cur_len;
	}
	dab.cur_len = len;
	editor_cursor_goto(dab.start.row, dab.start.col + len);
	cmd_set_transient_value(1);
}

/* Nothing (more) to offer.  Emacs puts the abbreviation back before it
 * complains, so a spent cycle leaves what the user typed rather than the
 * last expansion, and the next M-/ starts over. */
static void dabbrev_exhausted(void)
{
	int further = dab.ntried > 0;

	if (further) {
		editor_row_replace_range(dab.start.row, dab.start.col,
		    dab.cur_len, dab.abbrev, dab.abbrev_len, KG_EDIT_USER);
		editor_cursor_goto(
		    dab.start.row, dab.start.col + dab.abbrev_len);
	}
	editor_set_status_message("No %sdynamic expansion for `%s' found",
	    further ? "further " : "", dab.abbrev);
	dabbrev_reset();
	cmd_clear_transient();
}

void editor_dabbrev_expand(void)
{
	struct dabbrev_pos at;
	int n;

	if (!cmd_transient_value() && !dabbrev_begin()) {
		return;
	}
	n = dabbrev_next(&at);
	if (n == 0) {
		dabbrev_exhausted();
		return;
	}
	dabbrev_apply(at, n);
}
