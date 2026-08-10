/* ============ Tree-sitter grammar and highlight-query registry ===========
 *
 * One table row per editor mode kg can parse, plus the run-time machinery
 * that turns a row into a usable (TSLanguage, TSQuery) pair: dlopen the
 * grammar by soname, check its ABI, compile kg's own highlight query
 * against it, and map that query's captures onto kg's HL_* palette.
 *
 * Nothing here is linked at build time.  Grammars are shared objects
 * resolved at run time through a search path, which is Emacs' model and
 * the reason kg can use the very same installs Emacs does
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
#define KG_TS_GRAMMAR_DEFAULT_PATH "/opt-2/tree-sitter-v0.26.12-release/lib"
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
 * lisp/prelude.el is embedded).  Slice 8 declined to build it while the
 * queries were one language and twenty lines, and named batch 2 as the
 * moment to reconsider.  Batch 2 is here, and it is declined again, on
 * different grounds: a literal is grep-able (`grep -n string_fragment
 * src/`), it costs the complexity ratchets nothing (scc does not look
 * inside a string literal, which is why 500 lines of query measure zero),
 * and every one of these needs a paragraph of prose next to it saying why
 * its node names are what they are.  A generator would move the text into
 * files the compiler never sees, add a build step, and separate each query
 * from the only thing that explains it.  It buys no behaviour.  What would
 * change the answer is a second consumer of the same `.scm` -- an upstream
 * query kg vendors, or a user-supplied one -- and there is none.
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

/* ==== Batch 2 ============================================================
 *
 * JavaScript, and JSX with it.  tree-sitter-javascript parses JSX natively
 * -- (jsx_element), (jsx_opening_element), (jsx_text) are ordinary nodes of
 * the same grammar -- so kg's React mode is a second registry row naming
 * this same grammar and this same query, not a second dependency.  (kg's
 * React mode is in practice unreachable by file name anyway: its only
 * pattern is ".jsx" and JavaScript claims ".jsx" from an earlier HLDB row.
 * The row exists so that a buffer put into React mode by hand parses.)
 *
 * A template string is captured through its PIECES, for the reason Python's
 * f-string is: `${...}` is code.  (template_substitution) sits between the
 * fragments, and capturing (template_string) whole would colour an
 * expression as text.  The two backticks are anonymous tokens of that node
 * and are captured by name so the delimiters match the content.
 *
 * (regex) is @string rather than a face of its own: a literal is a literal,
 * and kg's palette has five colours.  Note it is the whole /pat/flags node,
 * which is what makes the JavaScript grammar's hardest ambiguity -- regex
 * or division -- visible on screen, and it is the external scanner's answer,
 * not kg's.
 *
 * true/false/null/undefined/this/super are NAMED nodes here, so they are
 * listed apart from the anonymous keyword tokens, exactly as Python's
 * none/true/false are. */
static const char JAVASCRIPT_HIGHLIGHT_QUERY[]
    = "(comment) @comment\n"
      "(hash_bang_line) @comment\n"
      "(string) @string\n"
      "(template_string \"`\" @string)\n"
      "(template_string (string_fragment) @string)\n"
      "(regex) @string\n"
      "(number) @number\n"
      "[\n"
      "  \"as\" \"async\" \"await\" \"break\" \"case\" \"catch\" \"class\"\n"
      "  \"const\" \"continue\" \"default\" \"delete\" \"do\" \"else\"\n"
      "  \"export\" \"extends\" \"finally\" \"for\" \"from\" \"function\"\n"
      "  \"if\" \"import\" \"in\" \"instanceof\" \"let\" \"new\" \"of\"\n"
      "  \"return\" \"static\" \"switch\" \"throw\" \"try\" \"typeof\"\n"
      "  \"var\" \"void\" \"while\" \"yield\"\n"
      "] @keyword\n"
      "[ (true) (false) (null) (undefined) (this) (super) ] @keyword\n"
      "(function_declaration name: (identifier) @type)\n"
      "(class_declaration name: (identifier) @type)\n"
      "(method_definition name: (property_identifier) @type)\n"
      "(call_expression function: (identifier) @type)\n"
      "(jsx_opening_element name: (identifier) @type)\n"
      "(jsx_closing_element name: (identifier) @type)\n"
      "(jsx_self_closing_element name: (identifier) @type)\n";

/* TypeScript AND TSX, one query text against two grammars.
 *
 * kg has a single TypeScript mode matching .ts, .tsx and .d.ts, and
 * tree-sitter has two grammars: `typescript`, in which `<T>x` is a type
 * assertion, and `tsx`, in which it opens a JSX element.  They are not
 * interchangeable -- typescript parses a .tsx file's `<div>hi</div>` as an
 * ERROR with a stray regex in it -- so the registry carries two rows for
 * the mode and picks between them by file-name suffix.
 *
 * The two grammars have DIFFERENT node inventories, and this text is the
 * intersection: `(jsx_element)` and friends do not exist in typescript, and
 * a query naming one fails to compile there (TSQueryErrorNodeType), which
 * would make .ts files plain text.  So the JSX patterns of the JavaScript
 * query above are absent here, and a .tsx file's tag names are uncoloured
 * while its attribute strings, expressions and keywords are not.  Two
 * literals would buy those tag names; one literal, verified against both
 * grammars by test_registry_queries_compile(), buys the guarantee that a
 * change to it cannot silently break one of the two.
 *
 * A class name is (type_identifier) in TypeScript where JavaScript makes it
 * an (identifier), so the type patterns cover it without a rule of their
 * own; (predefined_type) is `string`, `number`, `boolean` and the rest.
 * (accessibility_modifier) is public/private/protected, which are one named
 * node rather than three tokens. */
static const char TYPESCRIPT_HIGHLIGHT_QUERY[]
    = "(comment) @comment\n"
      "(hash_bang_line) @comment\n"
      "(string) @string\n"
      "(template_string \"`\" @string)\n"
      "(template_string (string_fragment) @string)\n"
      "(regex) @string\n"
      "(number) @number\n"
      "[\n"
      "  \"abstract\" \"as\" \"async\" \"await\" \"break\" \"case\"\n"
      "  \"catch\" \"class\" \"const\" \"continue\" \"declare\" \"default\"\n"
      "  \"delete\" \"do\" \"else\" \"enum\" \"export\" \"extends\"\n"
      "  \"finally\" \"for\" \"from\" \"function\" \"if\" \"implements\"\n"
      "  \"import\" \"in\" \"instanceof\" \"interface\" \"is\" \"keyof\"\n"
      "  \"let\" \"namespace\" \"new\" \"of\" \"readonly\" \"return\"\n"
      "  \"satisfies\" \"static\" \"switch\" \"throw\" \"try\" \"type\"\n"
      "  \"typeof\" \"var\" \"void\" \"while\" \"yield\"\n"
      "] @keyword\n"
      "[ (true) (false) (null) (undefined) (this) (super) ] @keyword\n"
      "(accessibility_modifier) @keyword\n"
      "[ (type_identifier) (predefined_type) ] @type\n"
      "(function_declaration name: (identifier) @type)\n"
      "(method_definition name: (property_identifier) @type)\n"
      "(call_expression function: (identifier) @type)\n";

/* Java.  The comment node is split in two here -- (line_comment) and
 * (block_comment), where C has one (comment) -- which is the sort of thing
 * a query has to be probed for rather than guessed at.
 *
 * Number literals are six named nodes and no umbrella node, so all six are
 * listed; a query that named only (decimal_integer_literal) would leave
 * 0xff and 1.5 uncoloured.  A text block ("""...""") is a (string_literal)
 * like any other and therefore spans rows through the same code path a
 * C block comment does.
 *
 * An annotation is @type for the reason a Python decorator is: it is about
 * the declaration under it.  Its arguments are inside the captured node, so
 * a string argument comes out @string -- @string outranks @type in the
 * precedence table below, which is the table earning its keep. */
static const char JAVA_HIGHLIGHT_QUERY[]
    = "(line_comment) @comment\n"
      "(block_comment) @comment\n"
      "(string_literal) @string\n"
      "(character_literal) @string\n"
      "[\n"
      "  (decimal_integer_literal) (hex_integer_literal)\n"
      "  (octal_integer_literal) (binary_integer_literal)\n"
      "  (decimal_floating_point_literal) (hex_floating_point_literal)\n"
      "] @number\n"
      "[\n"
      "  \"abstract\" \"assert\" \"break\" \"case\" \"catch\" \"class\"\n"
      "  \"continue\" \"default\" \"do\" \"else\" \"enum\" \"extends\"\n"
      "  \"final\" \"finally\" \"for\" \"if\" \"implements\" \"import\"\n"
      "  \"instanceof\" \"interface\" \"native\" \"new\" \"package\"\n"
      "  \"private\" \"protected\" \"public\" \"record\" \"return\"\n"
      "  \"sealed\" \"static\" \"strictfp\" \"switch\" \"synchronized\"\n"
      "  \"throw\" \"throws\" \"transient\" \"try\" \"volatile\" \"while\"\n"
      "  \"yield\"\n"
      "] @keyword\n"
      "[ (true) (false) (null_literal) ] @keyword\n"
      "[\n"
      "  (type_identifier) (integral_type) (floating_point_type)\n"
      "  (boolean_type) (void_type)\n"
      "] @type\n"
      "[ (marker_annotation) (annotation) ] @type\n"
      "(class_declaration name: (identifier) @type)\n"
      "(method_declaration name: (identifier) @type)\n";

/* Rust.  Two node names here are the ones worth having probed.
 *
 * A LIFETIME is not a character literal.  `'a` is (lifetime (identifier))
 * and `'a'` is (char_literal), and the grammar -- not kg -- is what tells
 * them apart; a scanner written by hand gets this wrong in both directions,
 * which is the entire argument for a parser.  (lifetime) is @type because
 * that is what it is, a parameter of the type system, and painting it makes
 * the distinction visible: `'a` green, `'a'` magenta, on the same row.
 *
 * `mut` and `crate` are NAMED nodes ((mutable_specifier), (crate)), not
 * anonymous keyword tokens, so listing them among the quoted tokens does
 * not compile.  `self` and `super` are named too.
 *
 * An attribute is @type, the Python-decorator rule again -- and note that
 * `#![...]` and `#[...]` are different nodes, (inner_attribute_item) and
 * (attribute_item), so a file's crate-level attributes need the first one
 * spelled out.  (raw_string_literal) is separate from (string_literal),
 * which is how r#"..."# stays one span. */
static const char RUST_HIGHLIGHT_QUERY[]
    = "(line_comment) @comment\n"
      "(block_comment) @comment\n"
      "(string_literal) @string\n"
      "(raw_string_literal) @string\n"
      "(char_literal) @string\n"
      "(integer_literal) @number\n"
      "(float_literal) @number\n"
      "[\n"
      "  \"as\" \"async\" \"await\" \"break\" \"const\" \"continue\"\n"
      "  \"dyn\" \"else\" \"enum\" \"extern\" \"fn\" \"for\" \"if\" \"impl\"\n"
      "  \"in\" \"let\" \"loop\" \"match\" \"mod\" \"move\" \"pub\" \"ref\"\n"
      "  \"return\" \"static\" \"struct\" \"trait\" \"type\" \"union\"\n"
      "  \"unsafe\" \"use\" \"where\" \"while\" \"yield\"\n"
      "] @keyword\n"
      "[\n"
      "  (boolean_literal) (self) (super) (crate) (mutable_specifier)\n"
      "] @keyword\n"
      "[ (primitive_type) (type_identifier) (lifetime) ] @type\n"
      "[ (attribute_item) (inner_attribute_item) ] @type\n"
      "(function_item name: (identifier) @type)\n"
      "(call_expression function: (identifier) @type)\n";

/* HTML.  A tag name is @keyword and an attribute name is @type, which is
 * the pair that makes markup readable: the element is the line noise and
 * the attribute is the thing being said about it.
 *
 * Both spellings of a value are captured.  (quoted_attribute_value) is the
 * whole "..." including the quotes and contains an (attribute_value); an
 * unquoted value is a bare (attribute_value) with no wrapper.  Capturing
 * both to the same face colours either, and the overlap costs nothing
 * because it is the same colour at the same precedence.
 *
 * <script> and <style> CONTENT is deliberately not coloured.  The grammar
 * hands it over as one opaque (raw_text) node, and colouring it as
 * JavaScript or CSS means language injection -- a second parser over a
 * sub-range of the same document -- which is out of v1 by Refinement
 * decision 4.  Leaving (raw_text) uncaptured is what "no injections" looks
 * like from the outside: the tags are coloured and the script between them
 * is plain. */
static const char HTML_HIGHLIGHT_QUERY[]
    = "(comment) @comment\n"
      "(doctype) @keyword\n"
      "(tag_name) @keyword\n"
      "(attribute_name) @type\n"
      "[ (quoted_attribute_value) (attribute_value) ] @string\n"
      "(entity) @string\n";

/* Emacs Lisp.  kg's Lisp mode edits init.el and kg's own packages, so the
 * grammar is tree-sitter-elisp rather than a Common Lisp or Scheme one.
 *
 * The shape to know is that a special form's HEAD is an anonymous token of
 * the (special_form) node, not a child symbol: `(defvar x 1)` parses as
 * (special_form (symbol) (integer)) with "defvar" spelled as a token.  So
 * the keywords below are quoted rather than captured through node types,
 * and the list is closed -- it is exactly the heads this grammar special
 * cases, and `require`, `message` and every other ordinary function are
 * (symbol) in a (list), correctly, because that is what they are.
 *
 * `defun`, `defsubst` and `defmacro` are different again: they build
 * (function_definition) and (macro_definition), which have a `name` field,
 * so the name a defun introduces is @type -- the same rule Python's def
 * gets.  A character literal (?c) is @number, because in Emacs Lisp a
 * character IS an integer.
 *
 * The quote sigils ' ` , ,@ #' are anonymous tokens of (quote), (unquote)
 * and friends and could be captured; they are not, because a one-character
 * colour change is noise, and capturing the (quote) node whole would paint
 * the quoted form as though it were a keyword. */
static const char ELISP_HIGHLIGHT_QUERY[]
    = "(comment) @comment\n"
      "(string) @string\n"
      "[ (integer) (float) (char) ] @number\n"
      "[\n"
      "  \"and\" \"catch\" \"cond\" \"condition-case\" \"defconst\"\n"
      "  \"defmacro\" \"defsubst\" \"defun\" \"defvar\" \"function\" \"if\"\n"
      "  \"interactive\" \"lambda\" \"let\" \"let*\" \"or\" \"prog1\"\n"
      "  \"prog2\" \"progn\" \"quote\" \"save-current-buffer\"\n"
      "  \"save-excursion\" \"save-restriction\" \"setq\" \"setq-default\"\n"
      "  \"unwind-protect\" \"while\"\n"
      "] @keyword\n"
      "[ \"nil\" \"t\" ] @keyword\n"
      "(function_definition name: (symbol) @type)\n"
      "(macro_definition name: (symbol) @type)\n";

/* Makefile.  What the legacy scanner reached for -- targets, variable
 * assignments and `$(...)` references -- is what this reaches for too; the
 * difference is that the grammar knows which is which instead of guessing
 * from a colon.
 *
 * A variable reference is captured WHOLE, sigil and parentheses included,
 * so `$(CC)` is one span; (automatic_variable) is `$@`, `$<`, `$^` and the
 * rest, which the grammar gives their own node type inside a recipe's
 * (shell_text).  Recipe bodies are otherwise plain: a recipe line is shell,
 * and colouring it as shell would be an injection (Refinement decision 4),
 * and this build has no bash grammar to inject anyway.
 *
 * The target and the assigned name are @type -- the structure of the file,
 * the same call YAML's mapping keys got. */
static const char MAKE_HIGHLIGHT_QUERY[]
    = "(comment) @comment\n"
      "(string) @string\n"
      "[ (variable_reference) (automatic_variable) ] @keyword\n"
      "[\n"
      "  \"define\" \"endef\" \"else\" \"endif\" \"export\" \"ifdef\"\n"
      "  \"ifeq\" \"ifndef\" \"ifneq\" \"include\" \"-include\"\n"
      "  \"sinclude\" \"override\" \"private\" \"undefine\" \"unexport\"\n"
      "  \"vpath\"\n"
      "] @keyword\n"
      "(variable_assignment name: (word) @type)\n"
      "(rule (targets (word) @type))\n";

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
 * Batches 1 and 2 of the plan's grammar manifest.  Adding a language is one
 * row plus its query.
 *
 * The `grammar` column is the soname stem, so the row also picks WHICH
 * grammar of a family is loaded.  "markdown" is the block grammar; the
 * companion "markdown-inline" grammar is a separate soname sitting beside
 * it, and kg does not load it -- inline highlighting inside a
 * paragraph is language injection, which is out of v1 (Refinement
 * decision 4).  A future injection slice adds a second row shape, not a
 * second name in this one.
 *
 * Two rows share a mode: TypeScript's, one per grammar, .tsx first because
 * the first row whose suffix matches wins and the suffix-less row is the
 * fallback.  Rows of one mode MUST be adjacent -- registry_find() stops at
 * the first row of a different mode -- and the last of them must have no
 * suffix, or a file the suffixes miss would have no grammar at all.
 *
 * React names the javascript grammar and the javascript query, which is
 * the answer to "should React be its own dependency": tree-sitter's
 * javascript grammar parses JSX, so it should not.
 *
 * Two kg modes that a reader will look for are deliberately absent.
 * SHELL has no row yet: the box's tree-sitter-bash was grammar ABI 6 when
 * batch 2 was written, below the 13-15 this tree-sitter reads, so a row
 * would have bought a status-line message instead of colours.  The install
 * has since moved to a bash the loader accepts, and what is missing now is
 * the row and its query (doc/TODO.md, "Tree-sitter follow-ups").  The
 * remaining modes (C#, PHP, Ruby, Swift, SQL, Dart, Vue, Angular, Svelte)
 * have no grammar installed and are plain text by Refinement decision 4;
 * Git commit and Git rebase stay grammarless by policy, because their C-c
 * keys quit the editor and must never depend on a third-party parser.
 *
 * The grammars these rows name are versioned by the install
 * TREE_SITTER_PREFIX points at, not by kg, so nothing here depends on
 * which release is in it: test_registry_queries_compile() is what says the
 * installed set still answers to these queries
 * (doc/plans/kg-tree-sitter-plan.md, "Grammar manifest"). */
static struct kg_ts_language ts_registry[] = {
	{ KG_MODE_C, NULL, "c", C_HIGHLIGHT_QUERY, KG_TS_LANG_UNTRIED, NULL,
	    NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_PYTHON, NULL, "python", PYTHON_HIGHLIGHT_QUERY,
	    KG_TS_LANG_UNTRIED, NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_YAML, NULL, "yaml", YAML_HIGHLIGHT_QUERY, KG_TS_LANG_UNTRIED,
	    NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_MARKDOWN, NULL, "markdown", MARKDOWN_HIGHLIGHT_QUERY,
	    KG_TS_LANG_UNTRIED, NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_JAVASCRIPT, NULL, "javascript", JAVASCRIPT_HIGHLIGHT_QUERY,
	    KG_TS_LANG_UNTRIED, NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_REACT, NULL, "javascript", JAVASCRIPT_HIGHLIGHT_QUERY,
	    KG_TS_LANG_UNTRIED, NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_TYPESCRIPT, ".tsx", "tsx", TYPESCRIPT_HIGHLIGHT_QUERY,
	    KG_TS_LANG_UNTRIED, NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_TYPESCRIPT, NULL, "typescript", TYPESCRIPT_HIGHLIGHT_QUERY,
	    KG_TS_LANG_UNTRIED, NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_JAVA, NULL, "java", JAVA_HIGHLIGHT_QUERY, KG_TS_LANG_UNTRIED,
	    NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_RUST, NULL, "rust", RUST_HIGHLIGHT_QUERY, KG_TS_LANG_UNTRIED,
	    NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_HTML, NULL, "html", HTML_HIGHLIGHT_QUERY, KG_TS_LANG_UNTRIED,
	    NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_LISP, NULL, "elisp", ELISP_HIGHLIGHT_QUERY,
	    KG_TS_LANG_UNTRIED, NULL, NULL, 0, { 0 }, { 0 } },
	{ KG_MODE_MAKEFILE, NULL, "make", MAKE_HIGHLIGHT_QUERY,
	    KG_TS_LANG_UNTRIED, NULL, NULL, 0, { 0 }, { 0 } },
};

#define TS_NLANGS ((unsigned int)(sizeof(ts_registry) / sizeof(ts_registry[0])))

/* mode -> its FIRST row, or -1.  The registry is walked once per edit that
 * arrives at a buffer with no tree (ts_after_edit_untreed()), so at
 * thirteen rows the linear scan stops being free; kg_mode_id is dense from
 * zero, so the index is an array subscript.  Built on first use because a
 * static initializer cannot compute it, and never rebuilt: the table is
 * const in everything but its cached verdicts. */
static signed char ts_mode_row[KG_MODE_COUNT];
static int ts_index_ready;

static void registry_index(void)
{
	unsigned int i;

	memset(ts_mode_row, -1, sizeof(ts_mode_row));
	/* Backwards, so a mode with several rows records the first. */
	for (i = TS_NLANGS; i-- > 0;) {
		ts_mode_row[ts_registry[i].mode] = (signed char)i;
	}
	ts_index_ready = 1;
}

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
 * occurrence) -- that is what makes a one-prefix-per-grammar install
 * expressible as a single entry; an entry without one is an ordinary
 * directory, which is what a flat install needs.  Either way the file name
 * is Emacs' soname,
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
	struct kg_ts_language probe = { KG_MODE_TEXT, NULL, "candidate", text,
		KG_TS_LANG_UNTRIED, lang, NULL, 0, { 0 }, { 0 } };

	if (!query_compile(&probe, err, errsz)) {
		return 0;
	}
	ts_query_delete(probe.query);
	return 1;
}

/* Does this row's grammar variant apply to `filename`?  A row with no
 * suffix is the mode's default and takes anything, including a buffer with
 * no file name at all. */
static int row_covers(const struct kg_ts_language *l, const char *filename)
{
	size_t flen, slen;

	if (!l->filename_suffix) {
		return 1;
	}
	if (!filename) {
		return 0;
	}
	flen = strlen(filename);
	slen = strlen(l->filename_suffix);
	return flen >= slen
	    && strcmp(filename + flen - slen, l->filename_suffix) == 0;
}

static struct kg_ts_language *registry_find(
    enum kg_mode_id mode, const char *filename)
{
	int i;

	if (!ts_index_ready) {
		registry_index();
	}
	if ((unsigned int)mode >= (unsigned int)KG_MODE_COUNT
	    || ts_mode_row[mode] < 0) {
		return NULL;
	}
	for (i = ts_mode_row[mode];
	    i < (int)TS_NLANGS && ts_registry[i].mode == mode; i++) {
		if (row_covers(&ts_registry[i], filename)) {
			return &ts_registry[i];
		}
	}
	return NULL;
}

struct kg_ts_language *kg_ts_language_for_mode(
    enum kg_mode_id mode, const char *filename)
{
	struct kg_ts_language *l = registry_find(mode, filename);
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
