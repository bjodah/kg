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

/* Python.  A string is captured through its PIECES rather than whole: the
 * grammar spells a string as (string (string_start) (string_content)
 * (string_end)), and an f-string puts (interpolation) between them --
 * code, not text, and painting the (string) node whole would colour it as
 * text.  Capturing the three pieces covers every quote style, including
 * the triple-quoted strings that span rows (string_content is one node
 * over all of them), and leaves an interpolation's expression alone.
 *
 * None/True/False are NAMED nodes here ((none), (true), (false)), not
 * anonymous keyword tokens, so they are listed separately from the token
 * list.  The soft keywords `match` and `case` are deliberately absent:
 * they are ordinary identifiers outside a match statement, and the point
 * of this query is the small palette, not completeness.
 *
 * A decorator is @type (HL_KEYWORD2), the same face as the name a def or
 * class introduces, because that is what it is about -- the definition
 * below it -- and HL_KEYWORD1 is already the keyword line noise.  (type)
 * is the annotation node, which costs one pattern and covers parameter
 * annotations, return types and annotated assignments alike. */
static const char PYTHON_HIGHLIGHT_QUERY[]
    = "(comment) @comment\n"
      "(string_start) @string\n"
      "(string_content) @string\n"
      "(string_end) @string\n"
      "(integer) @number\n"
      "(float) @number\n"
      "[\n"
      "  \"and\" \"as\" \"assert\" \"async\" \"await\" \"break\" \"class\"\n"
      "  \"continue\" \"def\" \"del\" \"elif\" \"else\" \"except\"\n"
      "  \"finally\" \"for\" \"from\" \"global\" \"if\" \"import\" \"in\"\n"
      "  \"is\" \"lambda\" \"nonlocal\" \"not\" \"or\" \"pass\" \"raise\"\n"
      "  \"return\" \"try\" \"while\" \"with\" \"yield\"\n"
      "] @keyword\n"
      "[ (none) (true) (false) ] @keyword\n"
      "(function_definition name: (identifier) @type)\n"
      "(class_definition name: (identifier) @type)\n"
      "(decorator) @type\n"
      "(type) @type\n";

/* YAML.  A mapping key is @type (HL_KEYWORD2): a key is the structure of
 * the document, and colouring it is the one thing that makes a YAML file
 * readable at a glance.  The key is captured as the whole (flow_node), so
 * a quoted key comes out one colour rather than half key and half string.
 *
 * Plain (unquoted) scalars in VALUE position are left alone.  The grammar
 * calls them (string_scalar), but in YAML nearly everything is one, and
 * painting them @string would paint most of the file.
 *
 * (block_scalar) is the whole `|` or `>` block including its indicator
 * and every row of its content -- one capture spanning many rows, which
 * is the multi-row construct this language contributes to the
 * differential.  Anchors, aliases and tags are captured whole so their
 * `&`, `*` and `!!` sigils are coloured with the name. */
static const char YAML_HIGHLIGHT_QUERY[]
    = "(comment) @comment\n"
      "(double_quote_scalar) @string\n"
      "(single_quote_scalar) @string\n"
      "(block_scalar) @string\n"
      "(integer_scalar) @number\n"
      "(float_scalar) @number\n"
      "[ (boolean_scalar) (null_scalar) (anchor) (alias) (tag) ] @keyword\n"
      "[ \"---\" \"...\" ] @keyword\n"
      "(block_mapping_pair key: (flow_node) @type)\n"
      "(flow_pair key: (flow_node) @type)\n";

/* Markdown, BLOCK grammar only.  tree-sitter-markdown ships two grammars,
 * and the inline one (libtree-sitter-markdown-inline.so, symbol
 * tree_sitter_markdown_inline) is NOT loaded here: reaching it means
 * language injection, which is out of v1 by Refinement decision 4.  So
 * emphasis, code spans and link destinations are plain text, and
 * everything below is a block-level construct the block grammar owns.
 * That is also why there is no (link_destination) pattern: the block
 * grammar has no such node, it hands a paragraph's contents over as one
 * opaque (inline).
 *
 * Headings are captured whole rather than as marker-plus-content: a
 * heading IS the line, an atx marker on its own is two characters of
 * colour, and a setext heading's underline row belongs with the row above
 * it.  Both nodes end at column 0 of the row AFTER the heading, which
 * paints nothing there -- tree-sitter's ranges are half-open and
 * paint_span() drops an empty span.
 *
 * A block quote is @comment because that is what a quote reads as: text
 * that is in the file without being of it.  Both kinds of code block are
 * @string, fences and info string included, which keeps a fenced block
 * one colour from ``` to ```. */
static const char MARKDOWN_HIGHLIGHT_QUERY[]
    = "(atx_heading) @keyword\n"
      "(setext_heading) @keyword\n"
      "(thematic_break) @keyword\n"
      "(block_quote) @comment\n"
      "(fenced_code_block) @string\n"
      "(indented_code_block) @string\n"
      "[\n"
      "  (list_marker_minus) (list_marker_plus) (list_marker_star)\n"
      "  (list_marker_dot) (list_marker_parenthesis)\n"
      "] @keyword\n";

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
 * Batch 1 of the plan's grammar manifest, complete: C, Python, Markdown
 * (block) and YAML.  Adding a language is one row plus its query.
 *
 * The `grammar` column is the soname stem, so the row also picks WHICH
 * grammar of a family is loaded.  "markdown" is the block grammar; the
 * companion "markdown-inline" install is a separate prefix and a separate
 * soname, and kg does not load it -- inline highlighting inside a
 * paragraph is language injection, which is out of v1 (Refinement
 * decision 4).  A future injection slice adds a second row shape, not a
 * second name in this one.
 *
 * The grammars these rows name are pinned by the /opt-9 environment
 * rather than by kg: c v0.24.2, python v0.25.0, yaml v0.7.2, markdown
 * v0.5.3 (doc/plans/kg-tree-sitter-plan.md, "Grammar manifest"). */
static struct kg_ts_language ts_registry[] = {
	{ KG_MODE_C, "c", C_HIGHLIGHT_QUERY, KG_TS_LANG_UNTRIED, NULL, NULL, 0,
	    { 0 }, { 0 } },
	{ KG_MODE_PYTHON, "python", PYTHON_HIGHLIGHT_QUERY, KG_TS_LANG_UNTRIED,
	    NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_YAML, "yaml", YAML_HIGHLIGHT_QUERY, KG_TS_LANG_UNTRIED, NULL,
	    NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_MARKDOWN, "markdown", MARKDOWN_HIGHLIGHT_QUERY,
	    KG_TS_LANG_UNTRIED, NULL, NULL, 0, { 0 }, { 0 } },
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

/* True when any pattern of `q` carries a predicate or directive -- #eq?,
 * #match?, #set!, anything in parentheses after the pattern.
 *
 * This is the structural half of Refinement decision 2 ("kg-owned minimal
 * queries, no predicate engine").  tree-sitter's C library PARSES
 * predicates and then leaves them entirely to the caller: it hands them
 * back through ts_query_predicates_for_pattern() and matches the pattern
 * regardless.  kg never asks, so a query with a predicate would not fail
 * -- it would silently behave as though every predicate were true, which
 * is a wrong answer wearing the shape of a right one.  A query kg cannot
 * execute as written is therefore treated exactly like one that does not
 * compile: the mode is plain text and the reason is said once. */
static int query_has_predicates(const TSQuery *q)
{
	uint32_t i, n = ts_query_pattern_count(q);

	for (i = 0; i < n; i++) {
		uint32_t steps = 0;

		ts_query_predicates_for_pattern(q, i, &steps);
		if (steps > 0) {
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
	if (query_has_predicates(q)) {
		snprintf(err, errsz,
		    "%s highlight query: predicates are not executed",
		    l->grammar);
		ts_query_delete(q);
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

int kg_ts_query_accepts(
    const TSLanguage *lang, const char *text, char *err, size_t errsz)
{
	struct kg_ts_language probe = { KG_MODE_TEXT, "candidate", text,
		KG_TS_LANG_UNTRIED, lang, NULL, 0, { 0 }, { 0 } };

	if (!query_compile(&probe, err, errsz)) {
		return 0;
	}
	ts_query_delete(probe.query);
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
