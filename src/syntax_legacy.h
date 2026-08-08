#ifndef KG_SYNTAX_LEGACY_H
#define KG_SYNTAX_LEGACY_H

/* The legacy row scanners' own predicates, exposed for
 * test/test_syntax_legacy.c (and test_syntax.c's char-signedness case,
 * which pins is_separator() against a negative char).  Everything else
 * src/syntax_legacy.c holds is static: the backend is reached through
 * syntax_backend_update_row() in src/syntax_backend.h. */

/* Rows are spelled with the struct tag rather than def.h's `erow` typedef
 * so this header needs nothing from def.h at all. */
struct erow;

/* True for a byte that ends a word for the generic keyword scanner: NUL,
 * ASCII whitespace, or one of the punctuation bytes in ",.()+-/(star)=~%[];"
 * -- spelled that way here because the literal set contains a comment
 * opener. */
int is_separator(int c);

/* True if the row's last render byte is inside a multi-line comment that
 * does not close on this row -- the cross-row state the generic scanner
 * stores in row->hl_oc. */
int editor_row_has_open_comment(struct erow *row);

#endif /* KG_SYNTAX_LEGACY_H */
