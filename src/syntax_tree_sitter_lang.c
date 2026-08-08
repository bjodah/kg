/* ============ Tree-sitter grammar and highlight-query registry ===========
 *
 * One table row per editor mode kg can parse, plus the run-time machinery
 * that turns a row into a usable (TSLanguage, TSQuery) pair: dlopen the
 * grammar by soname, check its ABI, compile kg's own highlight query
 * against it, and map that query's captures onto kg's HL_* palette.
 *
 * Nothing here is linked at build time.  Grammars are shared objects
 * resolved at run time through a search path, which is Emacs' model and
 * the reason kg can use the very same /opt-9 installs Emacs does
 * (doc/plans/kg-tree-sitter-plan.md, Refinement decision 1).  A grammar
 * that is not installed, speaks an ABI this tree-sitter cannot read, or
 * carries a query that will not compile is not an error: that mode is
 * plain text for the rest of the process, said once in the status line
 * (Refinement decision 4).  There is deliberately no fallback to the
 * legacy scanners -- they are not compiled into this build at all. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tree_sitter/api.h>

#include "def.h"
#include "syntax.h"
#include "syntax_tree_sitter_lang.h"

/* Where a grammar is looked for when $KG_TS_GRAMMAR_PATH says nothing.
 * The Makefile passes the developer box's layout; the fallback here keeps
 * the file compilable on its own. */
#ifndef KG_TS_GRAMMAR_DEFAULT_PATH
#define KG_TS_GRAMMAR_DEFAULT_PATH "/opt-9/tree-sitter-grammar-%s/lib"
#endif

/* Longest search-path entry and longest resolved .so path this loader will
 * build.  A candidate that does not fit is skipped rather than truncated:
 * a truncated path is a path to something else. */
#define KG_TS_PATH_MAX 1024

/* ---- kg-owned highlight queries -----------------------------------------
 *
 * Query text is a C string literal here rather than an upstream
 * highlights.scm read at run time, and it uses no predicates or
 * directives.  That is Refinement decision 2, and the reason is that
 * tree-sitter's C library does not execute predicates -- consuming an
 * upstream query means implementing its predicate language, and silently
 * treating a predicate as true is not acceptable.
 *
 * The plan's Phase 8 also sketches a `.scm`-embedding generator
 * (utils/embed_tree_sitter_queries.py and a generated .inc, the way
 * lisp/prelude.el is embedded).  It is deliberately NOT built while the
 * queries are one language and twenty lines: a generator whose input is
 * shorter than itself is machinery, not leverage.  When batch 2 lands
 * several hundred lines of query text, that is the moment.
 *
 * Capture names are the whole vocabulary of the palette below.  A capture
 * name with no face fails the query at load time (query_compile()), so a
 * typo degrades the mode to plain text loudly rather than colouring
 * nothing quietly. */
static const char C_HIGHLIGHT_QUERY[]
    = "(comment) @comment\n"
      "(string_literal) @string\n"
      "(char_literal) @string\n"
      "(system_lib_string) @string\n"
      "(number_literal) @number\n"
      "(preproc_directive) @keyword\n"
      "[\n"
      "  \"auto\" \"break\" \"case\" \"const\" \"continue\" \"default\"\n"
      "  \"do\" \"else\" \"enum\" \"extern\" \"for\" \"goto\" \"if\"\n"
      "  \"inline\" \"register\" \"restrict\" \"return\" \"sizeof\"\n"
      "  \"static\" \"struct\" \"switch\" \"typedef\" \"union\"\n"
      "  \"volatile\" \"while\"\n"
      "  \"#define\" \"#elif\" \"#else\" \"#endif\" \"#if\" \"#ifdef\"\n"
      "  \"#ifndef\" \"#include\"\n"
      "] @keyword\n"
      "[\n"
      "  (primitive_type)\n"
      "  (sized_type_specifier)\n"
      "  (type_identifier)\n"
      "] @type\n"
      "(function_declarator declarator: (identifier) @type)\n"
      "(call_expression function: (identifier) @type)\n";

/* ---- capture -> face, and the precedence between them -------------------
 *
 * Captures overlap: a block-comment opener inside a string literal, a
 * keyword token inside a sized_type_specifier that is also captured whole
 * ("unsigned" inside "unsigned int").  Which one wins is
 * decided by this table's `priority` and NOT by the order the query cursor
 * happens to walk the tree in, so the same text always comes out the same
 * colour.  Higher wins; ties cannot change the answer, because a tie means
 * two captures with the same priority, which by construction means the
 * same face.
 *
 *   priority  capture    face          beats
 *   --------  ---------  ------------  ---------------------------------
 *   5         @comment   HL_COMMENT    everything
 *   4         @string    HL_STRING     numbers, keywords, types
 *   3         @number    HL_NUMBER     keywords, types
 *   2         @keyword   HL_KEYWORD1   types
 *   1         @type      HL_KEYWORD2   nothing
 *
 * There is no @mlcomment: kg's HL_COMMENT and HL_MLCOMMENT are the same
 * terminal colour (editor_syntax_to_color(), cyan), the distinction exists
 * only so the legacy scanners can carry an open-block-comment state across
 * rows in row->hl_oc, and a parser has no use for it.  Multi-line comments
 * come out HL_COMMENT on every row they cover. */
static const struct {
	const char *name;
	unsigned char hl;
	unsigned char priority;
} ts_capture_faces[] = {
	{ "comment", HL_COMMENT, 5 },
	{ "string", HL_STRING, 4 },
	{ "number", HL_NUMBER, 3 },
	{ "keyword", HL_KEYWORD1, 2 },
	{ "type", HL_KEYWORD2, 1 },
};

#define TS_NFACES                                                              \
	((unsigned int)(sizeof(ts_capture_faces) / sizeof(ts_capture_faces[0])))

/* ---- the registry -------------------------------------------------------
 *
 * Batch 1 of the plan's grammar manifest is C, Python, Markdown (block)
 * and YAML; this slice registers C only, because C is the language whose
 * offsets, tabs and multi-line constructs the painting code has to be
 * proven against first.  Adding a language is one row plus its query. */
static struct kg_ts_language ts_registry[] = {
	{ KG_MODE_C, "c", C_HIGHLIGHT_QUERY, KG_TS_LANG_UNTRIED, NULL, NULL, 0,
	    { 0 }, { 0 } },
};

#define TS_NLANGS ((unsigned int)(sizeof(ts_registry) / sizeof(ts_registry[0])))

const char *kg_ts_grammar_search_path(void)
{
	const char *env = getenv("KG_TS_GRAMMAR_PATH");

	return (env && *env) ? env : KG_TS_GRAMMAR_DEFAULT_PATH;
}

/* Copy the next colon-separated entry of *cursor into buf and advance
 * *cursor past it; 0 when the path is exhausted.  Empty entries are
 * skipped rather than read as "the current directory", which is what a
 * stray leading or doubled colon in an environment variable means. */
static int path_next_entry(const char **cursor, char *buf, size_t bufsz)
{
	const char *p = *cursor, *sep;
	size_t n;

	while (*p == ':') {
		p++;
	}
	if (*p == '\0') {
		*cursor = p;
		return 0;
	}
	sep = strchr(p, ':');
	n = sep ? (size_t)(sep - p) : strlen(p);
	*cursor = sep ? sep + 1 : p + n;
	if (n >= bufsz) {
		buf[0] = '\0';
		return 1; /* skipped by the caller: an unusable entry */
	}
	memcpy(buf, p, n);
	buf[n] = '\0';
	return 1;
}

/* Turn one search-path entry into the .so path to try.  An entry
 * containing "%s" has the grammar name substituted (once, at the first
 * occurrence) -- that is what makes /opt-9's one-prefix-per-grammar layout
 * expressible as a single entry; an entry without one is an ordinary
 * directory.  Either way the file name is Emacs' soname,
 * libtree-sitter-<grammar>.so.  0 when the result would not fit. */
static int grammar_so_path(
    char *out, size_t outsz, const char *entry, const char *grammar)
{
	const char *pct = strstr(entry, "%s");
	char dir[KG_TS_PATH_MAX];
	int n;

	if (entry[0] == '\0') {
		return 0;
	}
	if (pct) {
		n = snprintf(dir, sizeof(dir), "%.*s%s%s", (int)(pct - entry),
		    entry, grammar, pct + 2);
	} else {
		n = snprintf(dir, sizeof(dir), "%s", entry);
	}
	if (n < 0 || (size_t)n >= sizeof(dir)) {
		return 0;
	}
	n = snprintf(out, outsz, "%s/libtree-sitter-%s.so", dir, grammar);
	return n > 0 && (size_t)n < outsz;
}

/* One candidate .so: load it, find its factory, check its ABI.  The handle
 * is deliberately never dlclose()d on success -- the TSLanguage returned
 * lives inside the object and every tree parsed with it outlives any one
 * buffer, so kg keeps grammars for the life of the process, as Emacs does.
 *
 * dlerror()'s text is not propagated into `err` on purpose: `err` reaches
 * the status line, and the reasons kg reports there are its own literals
 * plus the grammar name, never a string the loader built out of the
 * environment. */
static const TSLanguage *grammar_try(
    const char *sopath, const char *grammar, char *err, size_t errsz)
{
	union {
		void *obj;
		const TSLanguage *(*fn)(void);
	} sym;
	char symbol[64];
	const TSLanguage *lang;
	void *handle;
	uint32_t abi;

	handle = dlopen(sopath, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		return NULL; /* not installed here; keep searching */
	}
	snprintf(symbol, sizeof(symbol), "tree_sitter_%s", grammar);
	sym.obj = dlsym(handle, symbol);
	lang = sym.obj ? sym.fn() : NULL;
	if (!lang) {
		snprintf(err, errsz, "libtree-sitter-%s.so has no %s()",
		    grammar, symbol);
		dlclose(handle);
		return NULL;
	}
	abi = ts_language_abi_version(lang);
	if (abi < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION
	    || abi > TREE_SITTER_LANGUAGE_VERSION) {
		snprintf(err, errsz,
		    "libtree-sitter-%s.so is grammar ABI %u; kg reads %d-%d",
		    grammar, (unsigned)abi,
		    TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION,
		    TREE_SITTER_LANGUAGE_VERSION);
		dlclose(handle);
		return NULL;
	}
	return lang;
}

const TSLanguage *kg_ts_grammar_load(
    const char *grammar, const char *search_path, char *err, size_t errsz)
{
	char entry[KG_TS_PATH_MAX], sopath[KG_TS_PATH_MAX];
	const char *cursor = search_path;
	const TSLanguage *lang;

	snprintf(err, errsz,
	    "no libtree-sitter-%s.so on the grammar search "
	    "path (set KG_TS_GRAMMAR_PATH)",
	    grammar);
	while (path_next_entry(&cursor, entry, sizeof(entry))) {
		if (!grammar_so_path(sopath, sizeof(sopath), entry, grammar)) {
			continue;
		}
		lang = grammar_try(sopath, grammar, err, errsz);
		if (lang) {
			return lang;
		}
	}
	return NULL;
}

/* The face and precedence for one capture name, or 0 when the palette has
 * none -- which fails the whole query, because a capture kg cannot paint
 * is a query kg does not understand. */
static int capture_face(
    const char *name, uint32_t len, unsigned char *hl, unsigned char *priority)
{
	unsigned int i;

	for (i = 0; i < TS_NFACES; i++) {
		const char *want = ts_capture_faces[i].name;

		if (strlen(want) == (size_t)len
		    && memcmp(want, name, (size_t)len) == 0) {
			*hl = ts_capture_faces[i].hl;
			*priority = ts_capture_faces[i].priority;
			return 1;
		}
	}
	return 0;
}

/* Compile one registry row's query against its loaded grammar and resolve
 * every capture id to a face.  1 on success, with l->query owned by the
 * registry from then on (queries are immutable and shared by every buffer
 * of the language; only the cursors that execute them are per-buffer). */
static int query_compile(struct kg_ts_language *l, char *err, size_t errsz)
{
	uint32_t off = 0, i, n;
	TSQueryError qerr = TSQueryErrorNone;
	TSQuery *q;

	q = ts_query_new(l->language, l->query_text,
	    (uint32_t)strlen(l->query_text), &off, &qerr);
	if (!q) {
		snprintf(err, errsz, "%s highlight query: error %d at byte %u",
		    l->grammar, (int)qerr, (unsigned)off);
		return 0;
	}
	n = ts_query_capture_count(q);
	for (i = 0; i < n && i < KG_TS_MAX_CAPTURES; i++) {
		uint32_t len = 0;
		const char *name = ts_query_capture_name_for_id(q, i, &len);

		if (!name
		    || !capture_face(name, len, &l->capture_hl[i],
			&l->capture_priority[i])) {
			n = KG_TS_MAX_CAPTURES + 1; /* reject below */
			break;
		}
	}
	if (n > KG_TS_MAX_CAPTURES) {
		snprintf(err, errsz,
		    "%s highlight query: unpaintable or too many captures",
		    l->grammar);
		ts_query_delete(q);
		return 0;
	}
	l->capture_count = n;
	l->query = q;
	return 1;
}

static struct kg_ts_language *registry_find(enum kg_mode_id mode)
{
	unsigned int i;

	for (i = 0; i < TS_NLANGS; i++) {
		if (ts_registry[i].mode == mode) {
			return &ts_registry[i];
		}
	}
	return NULL;
}

struct kg_ts_language *kg_ts_language_for_mode(enum kg_mode_id mode)
{
	struct kg_ts_language *l = registry_find(mode);
	char err[160];

	if (!l || l->state == KG_TS_LANG_UNAVAILABLE) {
		return NULL;
	}
	if (l->state == KG_TS_LANG_READY) {
		return l;
	}
	err[0] = '\0';
	l->language = kg_ts_grammar_load(
	    l->grammar, kg_ts_grammar_search_path(), err, sizeof(err));
	if (l->language && query_compile(l, err, sizeof(err))) {
		l->state = KG_TS_LANG_READY;
		return l;
	}
	/* Negative results are cached too, and the message is printed once:
	 * every buffer of this mode would otherwise repeat the same dlopen
	 * and the same status line for the rest of the session. */
	l->state = KG_TS_LANG_UNAVAILABLE;
	l->language = NULL;
	editor_set_status_message("Plain text: %s", err);
	return NULL;
}
