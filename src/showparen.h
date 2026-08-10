/* showparen.h — Emacs' show-paren-mode: the paren at point and its partner
 *
 * Two halves, deliberately separated.  show_paren_compute() is pure: it
 * reads a row array and a point and says what would be highlighted, so
 * every rule below is testable without a terminal, a window or a
 * decoration store.  show_paren_update() is the editor side: it asks the
 * pure half about the current window's point and publishes the answer as
 * decorations (src/decor.h), which is the only channel that paints -- no
 * byte kg reads is ever written into a string a renderer interpolates.
 *
 * The rules, measured against `emacs -q -nw` 31.0.90 rather than assumed
 * (show-paren--default with the stock defaults, show-paren-when-point-
 * inside-paren and show-paren-when-point-in-periphery both nil):
 *
 *   - Point immediately AFTER a closer highlights that closer and scans
 *     backward for its opener; point ON an opener highlights it and scans
 *     forward.  When both could apply -- `)|(` -- the closer before point
 *     wins, which is what Emacs does.
 *   - Nesting is counted across all three pair kinds together, and the
 *     KIND is checked only once the partner is found: `(a]` pairs `(`
 *     with `]` and reports a mismatch, exactly as Emacs does.
 *   - No partner within KG_SHOW_PAREN_MAX_SCAN is the same answer as no
 *     partner at all: the paren at point alone, in the mismatch face.
 *     Emacs' truncated scan does this too (measured at
 *     blink-matching-paren-distance + 1).
 *   - A paren counts only if it is in the same syntactic class -- code,
 *     string, or comment -- as the paren at point, read from row->hl.
 *     See show_paren_compute()'s comment in showparen.c for where that
 *     diverges from Emacs and why.
 */

#ifndef KG_SHOWPAREN_H
#define KG_SHOWPAREN_H

struct erow;

/* On by default, as in Emacs 28.1 and later.  `show-paren-mode' (M-x) is
 * the way off; there is no key. */
extern int show_paren_mode;

/* How many bytes of row->render one computation may examine before it
 * gives up and reports "no partner".  Emacs bounds the same search with
 * blink-matching-paren-distance, whose default is 102400 characters, so
 * that is the number: the bound exists to keep a keystroke off O(whole
 * file), and matching Emacs' means a buffer that highlights there
 * highlights here. */
#define KG_SHOW_PAREN_MAX_SCAN 102400

enum show_paren_status {
	SHOW_PAREN_NONE = 0, /* nothing to highlight at point */
	SHOW_PAREN_MATCH, /* both ends, same kind */
	SHOW_PAREN_MISMATCH, /* both ends, wrong kind */
	SHOW_PAREN_UNMATCHED, /* the paren at point only; no partner */
};

/* Both positions are doc/coordinates.md's **chars** space -- a row index
 * and a byte offset into that row's `chars` -- because that is what a
 * buffer position converts from.  `there_*` is meaningful only for
 * SHOW_PAREN_MATCH and SHOW_PAREN_MISMATCH.  `scanned` is the number of
 * row->render bytes the scan examined, which is what test/test_perf.c
 * asserts a shape on. */
struct show_paren_result {
	enum show_paren_status status;
	int here_row;
	int here_col;
	int there_row;
	int there_col;
	long scanned;
};

/* What show-paren would highlight for point (`row`, `col`) of `rows`.
 * Pure: reads rows and nothing else, allocates nothing, and never looks
 * at show_paren_mode -- the caller decides whether the mode is on. */
void show_paren_compute(struct erow *rows, int numrows, int row, int col,
    struct show_paren_result *out);

/* Retire the previous highlight and publish the current one, for the
 * current window's point.  Called once per repaint (src/display.c), which
 * is what keeps a highlight from outliving the cursor position that
 * produced it.  A no-op beyond the retire when the mode is off. */
void show_paren_update(void);

/* Toggle the mode and report through the echo area; the body of the
 * `show-paren-mode' command. */
void show_paren_toggle(void);

#endif /* KG_SHOWPAREN_H */
