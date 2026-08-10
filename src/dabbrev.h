#ifndef KG_DABBREV_H
#define KG_DABBREV_H

/* Dynamic abbreviation expansion: Emacs' dabbrev-expand, on M-/.
 *
 * The word before point is a prefix; the expansion is a longer word the
 * buffer already contains.  Candidates come in Emacs' order -- backward
 * from point, nearest first, then forward past the word point is in --
 * and a second M-/ straight after the first replaces the expansion with
 * the next candidate instead of expanding again.  What makes a repeat a
 * repeat is command identity (src/cmdstate.h's transient), not the key,
 * so rebinding it keeps the cycle -- while reaching it through M-x
 * always expands afresh, since a command run through another one is
 * nested and does not become what ran last.
 *
 * The scanner below is the half worth a unit test: it reads a row array
 * and says where the next candidate is, without an editor around it.
 * Positions are in the chars space of a row (doc/coordinates.md): `row`
 * indexes the array, `col` is a byte offset into that row's `chars`.
 *
 * A word here is a run of ASCII alphanumerics and underscores -- kg's
 * own notion, the one M-f and M-d already move over.  Emacs' dabbrev
 * asks the major mode's syntax table instead, which agrees with this in
 * a programming buffer and differs in a text one, where `-` joins words.
 * Matching is exact-case: Emacs' case-fold search and its case-pattern
 * fitting are not implemented (see README.md). */

typedef struct erow erow;

struct dabbrev_pos {
	int row;
	int col;
};

/* The abbreviation ending at `at`: the run of word constituents
 * immediately before it.  Returns its length in bytes, with `*start`
 * saying where it begins, or 0 when there is none -- and `*start` is
 * worth reading only when the return is positive.  A `col` past the end
 * of the row is taken as its end, which is point at end of line, and a
 * row that does not exist has no abbreviation. */
int dabbrev_abbrev_before(const erow *rows, int numrows, struct dabbrev_pos at,
    struct dabbrev_pos *start);

/* The length in bytes of the word beginning exactly at `at`, or 0 when
 * no word begins there -- because `at` is not a word constituent, or
 * because one precedes it, which makes `at` the middle of a word rather
 * than its start. */
int dabbrev_word_at(const erow *rows, int numrows, struct dabbrev_pos at);

/* The nearest candidate strictly before `from` (backward) or at and
 * after `from` (forward): a word beginning with the `len` bytes of
 * `abbrev` and longer than them.  Returns its length in bytes and writes
 * where it begins to `*found`, or 0 when there is none. */
int dabbrev_search_backward(const erow *rows, int numrows,
    struct dabbrev_pos from, const char *abbrev, int len,
    struct dabbrev_pos *found);
int dabbrev_search_forward(const erow *rows, int numrows,
    struct dabbrev_pos from, const char *abbrev, int len,
    struct dabbrev_pos *found);

/* `M-x dabbrev-expand` (M-/): expand, or show the next expansion when
 * this invocation directly follows one of its own. */
void editor_dabbrev_expand(void);

/* Drop the cycle's state.  Only the command itself and its unit test
 * call this; a cycle otherwise ends by itself, when the next keystroke
 * runs some other command. */
void dabbrev_reset(void);

#endif /* KG_DABBREV_H */
