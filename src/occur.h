/* occur.h — M-x occur: every line matching a regexp, listed in *Occur*
 *
 * The listing is a results buffer of exactly the shape *xref* and
 * *compilation* already are: read-only text built with
 * buf_prepare_special_text(), shown in another window while the source
 * stays visible, and navigated with RET out of it and with
 * next-error/previous-error from anywhere.  What it adds is that its
 * results are places in a BUFFER rather than places in a file: the source
 * may never have been saved, and re-opening it by path is not an option,
 * so each match is a marker in the source buffer with the line and column
 * it was found at as the fallback for when that marker has gone.
 *
 * The listing is Emacs' listing: a header line, then one row per matching
 * source line, `%7d:` and the line's own text, with every match inside it
 * highlighted through src/decor.h.  A line with several matches appears
 * once and carries several highlights -- and several stops for
 * next-error, which walks matches rather than rows.
 */

#ifndef KG_OCCUR_H
#define KG_OCCUR_H

#include <stddef.h>
#include <stdint.h>

/* The listing buffer.  Named here rather than spelled again in kbd.c,
 * for the reason XREF_BUFFER_NAME is: the mode map that gives it RET, n,
 * p and q is keyed on the name, and two spellings of it would be two
 * chances for one of them to be wrong. */
#define OCCUR_BUFFER_NAME "*Occur*"

/* `M-x occur`.  Reads a regexp ("List lines matching regexp: "; empty
 * input cancels) and lists every line of the current buffer matching it.
 * No default key binding, because Emacs has none. */
void editor_occur(int fd);

/* `M-x occur-mode-goto-occurrence` (RET in *Occur*).  Visits the first
 * occurrence on the row point is on, leaving the listing's window the way
 * xref and next-error do; says so and does nothing anywhere else. */
void editor_occur_goto_occurrence(int fd);

/* `M-x occur-next` / `M-x occur-prev` (n and p in *Occur*).  Move point
 * between the listing's own rows, skipping the header, and report "No
 * more matches" / "No earlier matches" at the ends, as Emacs does.  They
 * move point in the listing only: visiting is RET's and next-error's. */
void editor_occur_next(int fd);
void editor_occur_prev(int fd);

/* The command, minus the prompt: search the current buffer for `pattern`,
 * rebuild the listing, and take the next-error keys.  Returns how many
 * matches were found -- 0 leaves every previous occur exactly as it was,
 * which is what makes a search that finds nothing free of consequences --
 * or -1 when the pattern would not compile, when the current buffer is
 * the listing itself, or when there was no room for the listing.
 *
 * Public because it is the whole of this module that a native test can
 * drive: everything above it needs a minibuffer, and everything below it
 * is where the interesting decisions are. */
int occur_run(const char *pattern);

/* ---- Test-only introspection of the match store ----
 *
 * Nothing outside test/ calls these.  They report what occur_run() built
 * without needing a window, a prompt, or point to actually move. */
size_t occur_test_count(void);
size_t occur_test_row_count(void);
/* Match `index`: its 0-based source row, its byte offset in that row (a
 * **chars** offset, see doc/coordinates.md), and the 1-based listing row
 * it is printed on. */
bool occur_test_match(
    size_t index, int *out_line, int *out_col, size_t *out_row);
/* The generation the store is at, which every rerun bumps, and the cursor
 * next-error is holding into it (-1 = nothing visited this run). */
uint32_t occur_test_generation(void);
int occur_test_cursor_index(void);
/* Step the next-error cursor `direction` (+1/-1) without visiting
 * anything, and report where it lands.  This is the half of the walk a
 * test binary can observe: the visit itself moves point in a real
 * window. */
int occur_test_cursor_step(int direction);

#endif /* KG_OCCUR_H */
