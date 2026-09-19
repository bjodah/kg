#ifndef KG_SPELL_INTERNAL_H
#define KG_SPELL_INTERNAL_H

/* What src/spell.c and src/spell_cmd.c share: the scanner's word bound
 * and navigation budget, and the three helpers the commands reuse from
 * the highlighting half.  Private to the module -- editor code reaches
 * the spell checker through src/spell.h, never here -- but a header
 * rather than duplicated definitions, so the prose/code policy and the
 * dictionary callback have exactly one spelling. */

#include "spell.h"

/* Words are what the scanner says: runs of ASCII letters and digits,
 * `_', apostrophes and non-ASCII bytes (UTF-8 text in any language
 * passes through to the dictionaries rather than being split on).
 * Longer than this is a line, not a word, and no dictionary is asked --
 * spell_word_candidate()'s rule, and the bound spell_check_word()
 * enforces on its own copy. */
#define SPELL_SCAN_WORD_MAX 64

/* Navigation bound: rows searched from point in either direction.  Past
 * it the search reports none rather than walking a whole large file on
 * one keystroke (show-paren's 100k-byte give-up, the same policy). */
#define SPELL_NAV_ROW_MAX 5000
#define SPELL_NAV_SPANS 16

/* One byte's answer to "does a word run through here". */
int spell_word_byte(unsigned char c);

/* Prose buffers check every word; code buffers only check comments and
 * strings.  Text and Markdown are prose; every other mode is code. */
int spell_check_all(const struct editor_buffer *b);

/* spell_check_word() as a spell_is_ok_fn. */
int spell_dict_ok(const char *word, size_t len, void *ctx);

/* The tag the cached dictionary was opened for ("" when none is): what
 * spell_update()'s stamp compares, so a frame with spell buffers on
 * costs the one interpreter read spell_available() already paid, and a
 * frame without them costs none at all. */
const char *spell_open_language(void);

/* Retire every misspelling published for `b' (and invalidate the
 * background pass's stamp for it), so the next frame rechecks from
 * scratch -- what session-accept needs the moment a word stops
 * flagging. */
void spell_drop(struct editor_buffer *b);

#endif /* KG_SPELL_INTERNAL_H */
