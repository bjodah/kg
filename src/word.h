#ifndef KG_WORD_H
#define KG_WORD_H

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
