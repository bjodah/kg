#ifndef KG_WORD_H
#define KG_WORD_H

#include <stddef.h>

struct editor_buffer;

/* The column both of kg's fills wrap at: the Lisp variable
 * `fill-column' as the current buffer sees it, or the same 70 for a
 * build with no evaluator to ask.  A number of BYTES of `row->chars` compared
 * against a display column, which is the same number for the ASCII a fill
 * column is a statement about. */
[[nodiscard]] int editor_fill_column(void);

/* Fill every paragraph of `b` between the buffer byte positions `beg`
 * and `end`, at `fill_col` -- Emacs' `fill-region', and the only caller
 * is the native of that name.  The two ends are not symmetric, because
 * Emacs' are not: `beg' is rounded back to the start of its own line and
 * `end' is taken exactly, so no paragraph is followed past it.  Blank
 * lines survive; each paragraph is one gateway replacement and therefore
 * one undo step.
 *
 * `*out_prefix' becomes a malloc'd copy of the LAST filled paragraph's
 * indent -- the value Emacs answers -- or nullptr when the region held
 * no paragraph to fill; the caller frees it.  `*out_end' is where the
 * region's end moved to, which is where Emacs leaves point.  False, with
 * both outputs cleared, only when an allocation failed; the buffer may
 * hold the paragraphs already filled by then, exactly as an interrupted
 * M-q would. */
[[nodiscard]] bool editor_fill_region(struct editor_buffer *b, size_t beg,
    size_t end, int fill_col, char **out_prefix, size_t *out_end);

/* Word-level editing.
 *
 * One byte of a word-case transformation: `mode` is 'u' (upcase), 'l'
 * (downcase) or 'c' (capitalize), `ch` is one byte of the word, and
 * `first` says whether it is the word's first, which is the only thing
 * 'c' treats differently.
 *
 * ASCII by definition, in the shape of def.h's ascii_is_*() family: kg
 * never calls setlocale(), so <ctype.h> in the "C" locale says nothing
 * useful about a byte >= 0x80, and this leaves one alone.
 *
 * Shared because the buffer's M-u/M-l/M-c (word.c) and the prompt's
 * (bufmgr.c) have to spell the same rule.  Only the rule: each side
 * keeps its own word boundary and its own way of putting the bytes
 * back, which is where erow and the undo transaction live. */
static inline int kg_word_case_byte(int mode, int ch, int first)
{
	if (mode == 'l' || (mode == 'c' && !first)) {
		return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
	}
	return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch;
}

#endif /* KG_WORD_H */
