/* fake_ts_grammar.c -- a grammar built to be refused.
 *
 * src/syntax_tree_sitter_lang.c's loader checks every dlopen'd grammar's
 * ABI against the range this tree-sitter reads (13-15 for 0.26) and reports
 * a mismatch instead of parsing with it.  The tree used to pin that guard
 * against a real grammar on the wrong side of it, an ABI-6
 * tree-sitter-bash; every grammar the current install ships is in range, so
 * the mismatch is manufactured here rather than found.  Two objects come
 * out of this file, one below the floor and one above the ceiling, so both
 * arms of the comparison are covered.
 *
 * A TSLanguage is opaque in tree_sitter/api.h, and this file deliberately
 * does not include it.  What the fixture stands on is the one guarantee
 * ts_language_abi_version() rests on -- the ABI is the first uint32_t of
 * the structure, which 0.26.12 compiles to `mov (%rdi),%eax; ret`.  The
 * rest is zero and is never read: the loader rejects the language before
 * anything else touches it.  Should that layout ever change, the ABI the
 * loader reports stops being the number planted here and
 * test_grammar_abi_is_rejected() fails on the message it asserts, which is
 * how the fixture says it has gone stale rather than passing vacuously.
 *
 * KG_FAKE_GRAMMAR is the soname stem, and so the tree_sitter_<name> factory
 * the loader looks for; KG_FAKE_ABI is the ABI to report.  Both come from
 * the Makefile, which builds this with none of kg's CFLAGS -- what it
 * stands in for is a third-party binary, so it is compiled like one. */

#include <stdint.h>

#ifndef KG_FAKE_GRAMMAR
#error "KG_FAKE_GRAMMAR must name the grammar (the tree_sitter_<name> stem)"
#endif
#ifndef KG_FAKE_ABI
#error "KG_FAKE_ABI must give the grammar ABI version to report"
#endif

#define KG_FAKE_CAT2(a, b) a##b
#define KG_FAKE_CAT(a, b) KG_FAKE_CAT2(a, b)
#define KG_FAKE_FACTORY KG_FAKE_CAT(tree_sitter_, KG_FAKE_GRAMMAR)

/* Padded well past any field the guard could plausibly grow to read, so a
 * loader that looks further than the first word stays inside the object. */
static const struct {
	uint32_t abi_version;
	uint32_t rest[63];
} fake_language = { KG_FAKE_ABI, { 0 } };

const void *KG_FAKE_FACTORY(void);

const void *KG_FAKE_FACTORY(void) { return &fake_language; }
