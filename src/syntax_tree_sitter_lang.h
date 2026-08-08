#ifndef KG_SYNTAX_TREE_SITTER_LANG_H
#define KG_SYNTAX_TREE_SITTER_LANG_H

/* The tree-sitter backend's grammar and highlight-query registry: which
 * editor mode is parsed by which grammar, with which kg-owned query, and
 * how a capture of that query becomes an HL_* face.
 *
 * Separate from src/syntax_tree_sitter.c, which is the parser/tree
 * lifetime and the painting, because the two grow for different reasons:
 * that file is done once it is right, and this one gains a row per
 * language (doc/plans/kg-tree-sitter-plan.md, Phase 8 -- batch 2).
 *
 * Grammars are dlopen'd by soname at run time, Emacs' own model
 * (Refinement decision 1): nothing here is linked, so a box with no
 * grammar for a mode simply edits that mode's files as plain text. */

#include <stddef.h> /* size_t, for the testing seam's error buffer */

#include "syntax.h" /* enum kg_mode_id */

/* Opaque tree-sitter handles, re-declared rather than pulled in from
 * <tree_sitter/api.h>.  This header is compiled standalone by `make
 * header-check` in EVERY configuration, including the default one, whose
 * include path has no tree-sitter on it at all; and the alternative --
 * an #ifdef around the include -- is the shape src/lisp.h deliberately
 * does not have.  These are api.h's own spellings, and a repeated typedef
 * of the same type is legal C11 and later, so a translation unit that has
 * both sees no conflict in either order. */
typedef struct TSLanguage TSLanguage;
typedef struct TSQuery TSQuery;

/* How many distinct capture names one language's query may use.  The
 * registry's per-capture tables are fixed-size arrays of this length
 * rather than allocations indexed by tree-sitter's capture id: five names
 * is the whole palette below, the arrays are flat, and a query that grew
 * past this is rejected at load time like any other broken query. */
#define KG_TS_MAX_CAPTURES 16

/* One language: its mode, the grammar to load for it, the query text to
 * highlight it with, and the results of having tried -- resolved once per
 * process and then cached, negative results included.
 *
 * `state` is a kg_ts_lang_state.  `language`, `query`, `capture_hl` and
 * `capture_priority` are meaningful only in the READY state; the last two
 * are indexed by tree-sitter's capture id, which is the order the capture
 * names first appear in `query_text`. */
struct kg_ts_language {
	enum kg_mode_id mode;
	const char *grammar; /* libtree-sitter-<grammar>.so */
	const char *query_text;
	int state;
	const TSLanguage *language;
	TSQuery *query;
	unsigned int capture_count;
	unsigned char capture_hl[KG_TS_MAX_CAPTURES];
	unsigned char capture_priority[KG_TS_MAX_CAPTURES];
};

enum kg_ts_lang_state {
	KG_TS_LANG_UNTRIED = 0,
	KG_TS_LANG_READY,
	/* No grammar on the search path, an ABI kg cannot speak, or a query
	 * that does not compile against the grammar that was found.  All
	 * three mean the same thing to the editor -- this mode is plain text
	 * for the rest of the process -- and all three say so once, in the
	 * status line, the first time a buffer asks. */
	KG_TS_LANG_UNAVAILABLE,
};

/* The registry entry for `mode`, ready to use, or NULL when this build
 * cannot highlight that mode: it has no entry, its grammar is not
 * installed, or its query did not compile.  The first call for a mode does
 * the dlopen and the query compile and reports a failure once through
 * editor_set_status_message(); every later call is a table lookup. */
struct kg_ts_language *kg_ts_language_for_mode(enum kg_mode_id mode);

/* Testing seam.  Resolve one grammar through an explicit colon-separated
 * search path instead of $KG_TS_GRAMMAR_PATH / the compiled-in default,
 * touching no cache and reporting nothing to the status line: it is how a
 * test asks what a missing grammar, or a grammar found somewhere else,
 * does.  Returns the language on success, else NULL with a human-readable
 * reason in err[0..errsz).  The returned language belongs to a shared
 * object this process never unloads, exactly as the cached path's does. */
const TSLanguage *kg_ts_grammar_load(
    const char *grammar, const char *search_path, char *err, size_t errsz);

/* The search path a plain kg_ts_language_for_mode() would use:
 * $KG_TS_GRAMMAR_PATH when it is set and non-empty, else the compiled-in
 * KG_TS_GRAMMAR_DEFAULT_PATH.  Never NULL. */
const char *kg_ts_grammar_search_path(void);

#endif /* KG_SYNTAX_TREE_SITTER_LANG_H */
