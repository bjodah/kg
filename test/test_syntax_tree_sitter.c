/* test_syntax_tree_sitter.c -- regression tests for the tree-sitter backend
 *
 * Everything src/syntax_tree_sitter.c and src/syntax_tree_sitter_lang.c
 * decide: that a grammar loads, that kg's own highlight query compiles
 * against it, that a capture becomes the right HL_* face at the right
 * offset of row->render, and that a buffer's parser and tree come and go
 * with the mode.  A build that installs a different backend does not build
 * this suite (the Makefile adds it to TESTBINS only when
 * WITH_TREE_SITTER=1); the backend-neutral half -- mode identity,
 * selection, colours, syntax-state lifetime -- is test/test_syntax.c, and
 * the other backend's own assertions are test/test_syntax_legacy.c.
 *
 * The C grammar is hard-required rather than skipped around.  A
 * WITH_TREE_SITTER=1 build only happens on a box with a tree-sitter
 * prefix, and the same box's grammar prefixes are part of that contract
 * (doc/plans/kg-tree-sitter-plan.md, Refinement decision 1); a suite that
 * skipped itself here would make "the grammar is missing" indistinguishable
 * from "the backend paints nothing".  Every OTHER grammar is required the
 * same way, through test_registry_queries_compile(), for the same reason:
 * a registry row whose grammar quietly is not there is a language that
 * quietly has no colours.
 *
 * Three layers, and they cost very different amounts to write.  Every row
 * of the registry gets a query-compile assertion (cheap, and it is what
 * says the node names in a query are the grammar's real ones).  Each
 * language gets two or three byte-exact paints, chosen for the decision in
 * its query that would have gone the other way if it had been written from
 * memory.  Nine of the languages get a differential fixture, an edit table
 * and a share of the seeded random loop; the other five rows ride the same
 * runner with no fixture, because the runner is language-generic and what a
 * tenth fixture adds is runtime. */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/syntax.h"
#include "../src/syntax_tree_sitter_lang.h"
#include "test.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- Helpers ---- */

static void setup(struct editor_syntax *syn)
{
	/* Before reset_current_buffer(), which memsets the record: the
	 * backend state is a pointer that lives in it, and a zeroed field is
	 * a leaked parser and tree (which is exactly what LeakSanitizer
	 * catches in the ASan lane). */
	syntax_state_release(bcur());
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	bcur()->syntax = syn;
}

static void teardown(void)
{
	syntax_state_release(bcur());
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
}

/* Put `lines` in the current buffer under `mode` and give the backend the
 * one notification a whole-document change earns: syntax_rebuild(), which
 * is where a buffer with no state acquires one.  editor_insert_row() on
 * its own highlights each row against whatever tree exists, which for the
 * first row of a fresh buffer is none -- that is the compat path, not the
 * one an editor takes, and test_row_at_a_time_needs_a_tree() below pins
 * it deliberately. */
static void load_mode(const char *mode, const char *const *lines, int n)
{
	int i;

	setup(syntax_find_by_name(mode));
	for (i = 0; i < n; i++) {
		editor_insert_row(bcur(), i, lines[i], strlen(lines[i]));
	}
	syntax_rebuild(bcur());
}

static void load_c(const char *const *lines, int n)
{
	load_mode("C", lines, n);
}

/* CHECK that row `r` of the current buffer has exactly `hl` -- one
 * expected face per byte of row->render, as a string of digits so the
 * expectation reads as a picture of the line above it. */
static void check_hl(int r, const char *expect)
{
	erow *row = &bcur()->row[r];
	int i, n = (int)strlen(expect);

	CHECKF(row->rsize == n, "row %d: rsize %d, expectation %d wide", r,
	    row->rsize, n);
	if (row->rsize != n) {
		return;
	}
	for (i = 0; i < n; i++) {
		CHECKF(row->hl[i] == (unsigned char)(expect[i] - '0'),
		    "row %d byte %d (%c): hl %d, expected %c", r, i,
		    row->chars ? row->render[i] : '?', row->hl[i], expect[i]);
	}
}

/* ---- Grammar loading and query compilation ---- */

/* The box contract: libtree-sitter-c.so is where the search path says. */
static void test_grammar_loads(void)
{
	char err[160];
	const TSLanguage *lang;

	err[0] = '\0';
	lang = kg_ts_grammar_load(
	    "c", kg_ts_grammar_search_path(), err, sizeof(err));
	CHECKF(lang != NULL, "C grammar did not load: %s", err);
}

/* A grammar that is not there is a miss with a reason, not a crash and not
 * a half-loaded language.  This is the whole of "unsupported languages
 * degrade to normal text": the caller gets NULL and paints nothing. */
static void test_missing_grammar_degrades(void)
{
	char err[160];

	err[0] = '\0';
	CHECK(
	    kg_ts_grammar_load("c", "/nonexistent/kg-ts-test", err, sizeof(err))
	    == NULL);
	CHECK(err[0] != '\0');

	err[0] = '\0';
	CHECK(kg_ts_grammar_load("no-such-language",
		  kg_ts_grammar_search_path(), err, sizeof(err))
	    == NULL);
	CHECK(err[0] != '\0');
}

/* Where libtree-sitter-c.so actually is on this box: the compiled-in search
 * path, walked the way the loader walks it -- colon-separated entries, one
 * %s substitution per entry.  The loader's own resolver is static to
 * src/syntax_tree_sitter_lang.c; repeating its two rules here is what lets
 * the fixtures below be built out of the real grammar whatever layout the
 * install happens to have. */
static int find_c_grammar_so(char *out, size_t outsz)
{
	const char *cursor = kg_ts_grammar_search_path();

	while (*cursor != '\0') {
		const char *sep = strchr(cursor, ':');
		size_t n = sep ? (size_t)(sep - cursor) : strlen(cursor);
		char entry[512], dir[512];
		const char *pct;

		if (n > 0 && n < sizeof(entry)) {
			memcpy(entry, cursor, n);
			entry[n] = '\0';
			pct = strstr(entry, "%s");
			if (pct) {
				snprintf(dir, sizeof(dir), "%.*s%s%s",
				    (int)(pct - entry), entry, "c", pct + 2);
			} else {
				snprintf(dir, sizeof(dir), "%s", entry);
			}
			snprintf(out, outsz, "%s/libtree-sitter-c.so", dir);
			if (access(out, R_OK) == 0) {
				return 1;
			}
		}
		if (!sep) {
			break;
		}
		cursor = sep + 1;
	}
	out[0] = '\0';
	CHECKF(0, "no libtree-sitter-c.so on the search path (%s)",
	    kg_ts_grammar_search_path());
	return 0;
}

static int make_fixture_dir(const char *path)
{
	if (mkdir(path, 0700) != 0 && errno != EEXIST) {
		CHECKF(0, "cannot make %s: %s", path, strerror(errno));
		return 0;
	}
	return 1;
}

/* Point <dir>/libtree-sitter-c.so at the real grammar, so a directory that
 * is a grammar directory only because a symlink says so still loads. */
static int link_c_grammar_into(const char *dir)
{
	char real[512], sopath[640];

	if (!find_c_grammar_so(real, sizeof(real))) {
		return 0;
	}
	snprintf(sopath, sizeof(sopath), "%s/libtree-sitter-c.so", dir);
	unlink(sopath);
	if (symlink(real, sopath) != 0) {
		CHECKF(0, "cannot symlink %s -> %s: %s", sopath, real,
		    strerror(errno));
		return 0;
	}
	return 1;
}

static void unlink_c_grammar_from(const char *dir)
{
	char sopath[640];

	snprintf(sopath, sizeof(sopath), "%s/libtree-sitter-c.so", dir);
	unlink(sopath);
}

/* Both spellings of a search-path entry resolve, and a path is searched in
 * order: a plain directory means <entry>/libtree-sitter-c.so, and an entry
 * containing %s has the grammar name substituted first, which is what lets
 * one entry cover a whole one-prefix-per-grammar tree.
 *
 * The install this box has is flat -- every grammar .so beside the core in
 * one directory -- so the tree the %s form exists for is built here out of
 * a symlink rather than found.  The substitution is a live feature of the
 * loader, and an Emacs-shaped one-prefix-per-grammar install is exactly
 * what it is for, so it stays asserted against a real dlopen instead of
 * going away with the layout that used to supply one. */
static void test_search_path_forms(void)
{
	const char *root = "test/.ts-grammar-prefix";
	char prefix[256], plain[320], subst[320], mixed[640], err[160];

	snprintf(prefix, sizeof(prefix), "%s/tree-sitter-grammar-c", root);
	snprintf(plain, sizeof(plain), "%s/lib", prefix);
	if (!make_fixture_dir(root) || !make_fixture_dir(prefix)
	    || !make_fixture_dir(plain) || !link_c_grammar_into(plain)) {
		return;
	}
	snprintf(subst, sizeof(subst), "%s/tree-sitter-grammar-%%s/lib", root);

	err[0] = '\0';
	CHECKF(kg_ts_grammar_load("c", plain, err, sizeof(err)) != NULL, "%s",
	    err);
	err[0] = '\0';
	CHECKF(kg_ts_grammar_load("c", subst, err, sizeof(err)) != NULL, "%s",
	    err);
	/* Misses before the hit are skipped, not fatal, and empty entries
	 * are not "the current directory". */
	snprintf(mixed, sizeof(mixed), "/nonexistent::%s", subst);
	err[0] = '\0';
	CHECKF(kg_ts_grammar_load("c", mixed, err, sizeof(err)) != NULL, "%s",
	    err);

	unlink_c_grammar_from(plain);
	rmdir(plain);
	rmdir(prefix);
	rmdir(root);
}

/* $KG_TS_GRAMMAR_PATH wins over the compiled-in default when it is set to
 * something non-empty, and the default is what an unset or empty variable
 * means.  What the default IS is the Makefile's business, so the assertion
 * is that the same answer comes back, not that it spells any one layout. */
static void test_env_overrides_search_path(void)
{
	const char *dir = "test/.ts-grammar-env";
	char dflt[1024];
	char err[160];

	unsetenv("KG_TS_GRAMMAR_PATH");
	snprintf(dflt, sizeof(dflt), "%s", kg_ts_grammar_search_path());
	CHECK(dflt[0] != '\0');
	setenv("KG_TS_GRAMMAR_PATH", "/somewhere/else", 1);
	CHECK(strcmp(kg_ts_grammar_search_path(), "/somewhere/else") == 0);
	setenv("KG_TS_GRAMMAR_PATH", "", 1);
	CHECK(strcmp(kg_ts_grammar_search_path(), dflt) == 0);
	unsetenv("KG_TS_GRAMMAR_PATH");
	CHECK(strcmp(kg_ts_grammar_search_path(), dflt) == 0);

	/* And the override is a path the loader really searches. */
	if (!make_fixture_dir(dir) || !link_c_grammar_into(dir)) {
		return;
	}
	setenv("KG_TS_GRAMMAR_PATH", dir, 1);
	err[0] = '\0';
	CHECKF(kg_ts_grammar_load(
		   "c", kg_ts_grammar_search_path(), err, sizeof(err))
		!= NULL,
	    "%s", err);
	unsetenv("KG_TS_GRAMMAR_PATH");
	unlink_c_grammar_from(dir);
	rmdir(dir);
}

/* kg's own highlight queries compile against the real grammars, and every
 * capture they name has a face.  A query that does not compile is
 * reported by kg_ts_language_for_mode() returning NULL, so this assertion
 * is the difference between "no colours because the query is broken" and
 * "no colours because the backend is broken".
 *
 * EVERY registry row, batch 1 and batch 2: its grammar is on this box,
 * kg's own query compiles against it, and each of its captures resolved to
 * a face.  A query that names a node type its grammar does not have is a
 * compile error, so this is also what says the node names in those queries
 * are the grammar's real ones rather than plausible guesses -- and for the
 * two TypeScript rows, that the one shared query text is inside BOTH
 * grammars' node inventories, which is not something either grammar
 * promises.
 *
 * The row is named by the (mode, filename) pair the registry resolves,
 * because that is the whole interface: a .tsx name and a .ts name are the
 * same mode and different grammars.  Asserting `grammar` is what makes the
 * variant selection observable rather than inferred. */
static void test_registry_queries_compile(void)
{
	static const struct {
		enum kg_mode_id mode;
		const char *filename;
		const char *grammar;
	} rows[] = {
		{ KG_MODE_C, "a.c", "c" },
		{ KG_MODE_PYTHON, "a.py", "python" },
		{ KG_MODE_YAML, "a.yaml", "yaml" },
		{ KG_MODE_MARKDOWN, "a.md", "markdown" },
		{ KG_MODE_JAVASCRIPT, "a.js", "javascript" },
		{ KG_MODE_REACT, "a.jsx", "javascript" },
		{ KG_MODE_TYPESCRIPT, "a.tsx", "tsx" },
		{ KG_MODE_TYPESCRIPT, "a.ts", "typescript" },
		{ KG_MODE_JAVA, "A.java", "java" },
		{ KG_MODE_RUST, "a.rs", "rust" },
		{ KG_MODE_GO, "a.go", "go" },
		{ KG_MODE_HTML, "a.html", "html" },
		{ KG_MODE_LISP, "init.el", "elisp" },
		{ KG_MODE_MAKEFILE, "Makefile", "make" },
		{ KG_MODE_SHELL, "a.sh", "bash" },
	};
	size_t i;

	for (i = 0; i < sizeof(rows) / sizeof(*rows); i++) {
		struct kg_ts_language *l
		    = kg_ts_language_for_mode(rows[i].mode, rows[i].filename);

		CHECKF(l != NULL, "%s: no language", rows[i].grammar);
		if (!l) {
			continue;
		}
		CHECKF(strcmp(l->grammar, rows[i].grammar) == 0,
		    "%s resolved to grammar %s, expected %s", rows[i].filename,
		    l->grammar, rows[i].grammar);
		CHECK(l->state == KG_TS_LANG_READY);
		CHECK(l->query != NULL);
		CHECKF(l->capture_count > 0
			&& l->capture_count <= KG_TS_MAX_CAPTURES,
		    "%s: %u captures", rows[i].grammar, l->capture_count);
		CHECK(kg_ts_language_for_mode(rows[i].mode, rows[i].filename)
		    == l); /* cached */
	}
}

/* The one mode whose grammar variant depends on the file name, asserted
 * from both ends: which row the registry hands back, and what the two
 * grammars then do to the same bytes.
 *
 * `<div className="c">hi</div>` is a JSX element in .tsx and, in .ts, a
 * type assertion applied to `div`.  Both spell the opening `div` @type and
 * for opposite reasons -- a (type_identifier) there, a JSX tag name here --
 * so what separates the two rows on this line is the CLOSING `</div>`,
 * which is a tag name in .tsx and nothing at all in .ts.  That is why the
 * variant exists: with only the typescript grammar, every .tsx file in the
 * tree parses as an ERROR node. */
static void test_typescript_variant_selection(void)
{
	static const char *const lines[]
	    = { "const a = <div className=\"c\">hi</div>;" };
	struct kg_ts_language *ts
	    = kg_ts_language_for_mode(KG_MODE_TYPESCRIPT, "app.ts");
	struct kg_ts_language *tsx
	    = kg_ts_language_for_mode(KG_MODE_TYPESCRIPT, "app.tsx");

	CHECK(ts != NULL && tsx != NULL);
	if (!ts || !tsx) {
		return;
	}
	CHECK(ts != tsx);
	CHECK(strcmp(ts->grammar, "typescript") == 0);
	CHECK(strcmp(tsx->grammar, "tsx") == 0);
	/* A buffer with no file name at all falls to the suffix-less row. */
	CHECK(kg_ts_language_for_mode(KG_MODE_TYPESCRIPT, NULL) == ts);
	/* ".tsx" is a suffix test, not a substring one. */
	CHECK(kg_ts_language_for_mode(KG_MODE_TYPESCRIPT, "a.tsx.ts") == ts);

	setup(syntax_find_by_name("TypeScript"));
	bcur()->filename = (char *)"app.ts";
	editor_insert_row(bcur(), 0, lines[0], strlen(lines[0]));
	syntax_rebuild(bcur());
	CHECK(bcur()->syntax_state != NULL);
	/*        const a = <div className="c">hi</div>;
	 *                   ^^^ a type_identifier under the .ts grammar */
	check_hl(0, "44444000000555000000000006660000000000");

	setup(syntax_find_by_name("TypeScript"));
	bcur()->filename = (char *)"app.tsx";
	editor_insert_row(bcur(), 0, lines[0], strlen(lines[0]));
	syntax_rebuild(bcur());
	CHECK(bcur()->syntax_state != NULL);
	/* ... and a JSX tag name under the .tsx grammar, at both ends of
	 * the element. */
	check_hl(0, "44444000000555000000000006660000055500");
	bcur()->filename = NULL;
	teardown();
}

/* Refinement decision 2, enforced rather than merely intended: kg's
 * queries carry no predicates, and one that did would be REJECTED.  It
 * has to be, because tree-sitter's C library parses predicates and then
 * matches the pattern anyway -- a #eq? kg never evaluates is not a filter
 * that fails, it is a filter that silently passes.  The seam compiles a
 * candidate under exactly the registry's rules and keeps nothing. */
static void test_query_predicates_are_rejected(void)
{
	char err[160];
	const TSLanguage *c = kg_ts_grammar_load(
	    "c", kg_ts_grammar_search_path(), err, sizeof(err));

	CHECKF(c != NULL, "%s", err);
	if (!c) {
		return;
	}
	err[0] = '\0';
	CHECKF(kg_ts_query_accepts(c, "(comment) @comment\n", err, sizeof(err))
		== 1,
	    "a plain query was refused: %s", err);

	err[0] = '\0';
	CHECK(kg_ts_query_accepts(c,
		  "((comment) @comment (#eq? @comment \"/* x */\"))\n", err,
		  sizeof(err))
	    == 0);
	CHECK(strstr(err, "predicate") != NULL);

	/* The directive spelling of the same thing -- #set! sets a property
	 * rather than filtering, and kg reads no properties either. */
	err[0] = '\0';
	CHECK(
	    kg_ts_query_accepts(c, "((comment) @comment (#set! priority 99))\n",
		err, sizeof(err))
	    == 0);

	/* And the two failures that were already there, through the same
	 * door: a capture with no face, and a query that will not parse. */
	err[0] = '\0';
	CHECK(kg_ts_query_accepts(c, "(comment) @invented\n", err, sizeof(err))
	    == 0);
	err[0] = '\0';
	CHECK(kg_ts_query_accepts(
		  c, "(no_such_node) @comment\n", err, sizeof(err))
	    == 0);
}

/* A mode with no registry row is not an error and costs nothing.  C# and
 * Vue are the deliberate examples: the install has no grammar for either,
 * so they are plain-text modes by Refinement decision 4 and expected to
 * stay ones.  A slice that gives C# a grammar moves this anchor; that is
 * the intended cost of the anchor being real -- Shell used to be asserted
 * here for want of a row and a query, and the slice that wrote them took
 * the line out and put `{ KG_MODE_SHELL, "a.sh", "bash" }` in
 * test_registry_queries_compile() instead. */
static void test_unregistered_mode_has_no_language(void)
{
	CHECK(kg_ts_language_for_mode(KG_MODE_CSHARP, "a.cs") == NULL);
	CHECK(kg_ts_language_for_mode(KG_MODE_VUE, "a.vue") == NULL);
	CHECK(kg_ts_language_for_mode(KG_MODE_TEXT, NULL) == NULL);
	/* The git modes stay grammarless by policy rather than by accident:
	 * they bind C-c keys that quit the editor, so their behaviour must
	 * never depend on a third-party parser (Phase 9). */
	CHECK(kg_ts_language_for_mode(KG_MODE_GIT_COMMIT, "COMMIT_EDITMSG")
	    == NULL);
	CHECK(kg_ts_language_for_mode(KG_MODE_GIT_REBASE, "git-rebase-todo")
	    == NULL);
	/* And a mode id past the end of the table is a miss, not a read off
	 * the end of the index. */
	CHECK(kg_ts_language_for_mode((enum kg_mode_id)KG_MODE_COUNT, NULL)
	    == NULL);
}

/* A grammar whose ABI this tree-sitter cannot read is refused, in both
 * directions, with the ABI it found in the message -- not loaded, not
 * parsed with, not half-installed.
 *
 * The mismatch used to be a real one: the box shipped a tree-sitter-bash of
 * grammar ABI 6, and this test pointed at it.  Every grammar the current
 * install ships is in range, so the case the guard exists for is built
 * instead -- test/fake_ts_grammar.c, compiled by the Makefile into
 * test/.ts-fake-grammar/ as two .so files reporting ABI 6 and ABI 999.
 * A manufactured mismatch is a weaker witness than a found one, so the
 * assertion is on the ABI NUMBER in the message rather than on the word:
 * that is what says the guard read the version the fixture planted, and a
 * fixture whose shape the core has outgrown reports something else and
 * fails here.
 *
 * Not skipped when the fixture is missing.  It is built by the same make
 * invocation that built this binary, so missing means broken, and a guard
 * that quietly stops running is the failure this whole test is against. */
static void test_grammar_abi_is_rejected(void)
{
	static const struct {
		const char *grammar;
		const char *reported;
	} fakes[] = {
		{ "kgfakeold", "ABI 6" },
		{ "kgfakenew", "ABI 999" },
	};
	const char *dir = "test/.ts-fake-grammar";
	char err[160];
	size_t i;

	for (i = 0; i < sizeof(fakes) / sizeof(fakes[0]); i++) {
		err[0] = '\0';
		CHECK(
		    kg_ts_grammar_load(fakes[i].grammar, dir, err, sizeof(err))
		    == NULL);
		CHECKF(strstr(err, fakes[i].reported) != NULL,
		    "%s: unexpected reason: %s", fakes[i].grammar, err);
	}
}

/* ---- Captures to faces ---- */

/* The representative line: a type, an identifier, a number and a trailing
 * comment, asserted byte for byte. */
static void test_declaration_number_and_comment(void)
{
	static const char *const lines[] = { "int x = 42; /* c */" };

	load_c(lines, 1);
	/* int -> type, 42 -> number, the trailing comment to end of row. */
	check_hl(0, "5550000077002222222");
	teardown();
}

/* A string literal is one span whatever is inside it: the escape is part
 * of the literal, and the block-comment opener in the second string does
 * not open anything. */
static void test_string_literals(void)
{
	static const char *const lines[] = {
		"const char *s = \"a\\tb\";",
		"const char *t = \"/* not a comment\";",
	};

	load_c(lines, 2);
	check_hl(0, "44444055550000006666660");
	check_hl(1, "44444055550000006666666666666666660");
	teardown();
}

/* A block comment over three rows: the row that opens it from its opener,
 * the row in the middle whole, the row that closes it up to the closer --
 * and only up to it, so the code after the comment is coloured normally. */
static void test_multiline_block_comment(void)
{
	static const char *const lines[] = {
		"int a;",
		"/* opened",
		"still inside",
		"*/ int b;",
	};

	load_c(lines, 4);
	check_hl(0, "555000");
	check_hl(1, "222222222");
	check_hl(2, "222222222222");
	check_hl(3, "220555000");
	teardown();
}

/* Preprocessor lines: the directive is a keyword and the bracketed header
 * is a string, which is the one place C's lexer produces a string that no
 * quote character opens. */
static void test_preproc_directive(void)
{
	static const char *const lines[] = { "#include <stdio.h>" };

	load_c(lines, 1);
	check_hl(0, "444444440666666666");
	teardown();
}

/* THE coordinate seam.  A tab is one byte of row->chars and eight bytes of
 * row->render, and row->hl is indexed by the latter: a capture that
 * tree-sitter reports at chars column 1 has to be painted at render offset
 * 8 (doc/coordinates.md rows 3 and 4).  Getting this wrong is invisible in
 * a saved file and obvious on a screen. */
static void test_tab_before_token(void)
{
	static const char *const lines[] = { "\tint x;" };
	erow *row;

	load_c(lines, 1);
	row = &bcur()->row[0];
	CHECK(row->size == 7);
	CHECK(row->rsize == 14); /* one tab -> eight columns */
	CHECK(row->hl[7] == HL_NORMAL); /* last column of the tab */
	CHECK(row->hl[8] == HL_KEYWORD2);
	CHECK(row->hl[9] == HL_KEYWORD2);
	CHECK(row->hl[10] == HL_KEYWORD2);
	CHECK(row->hl[11] == HL_NORMAL);
	teardown();
}

/* And the other half of it: a multi-byte character is BYTES, not one
 * column, on both sides of the conversion.  "e-acute" is two bytes of
 * chars and two bytes of render, so the token after it must not slide. */
static void test_utf8_before_token(void)
{
	/* 21 bytes; the "int" starts at byte 16 on both sides. */
	static const char *const lines[] = { "char *s = \"\xc3\xa9\"; int x;" };
	erow *row;

	load_c(lines, 1);
	row = &bcur()->row[0];
	CHECK(row->size == 22);
	CHECK(row->rsize == 22);
	check_hl(0, "5555000000666600555000");
	teardown();
}

/* ---- Python ----
 *
 * Byte-exact paints for the three languages batch 1 adds, chosen for the
 * node shapes their queries depend on rather than for coverage: what
 * makes a language interesting here is the construct whose colour is not
 * decided on its own row. */

/* The representative Python line: a keyword, the name a def introduces
 * (which is @type, not @keyword), and a trailing comment. */
static void test_python_def_and_comment(void)
{
	static const char *const lines[] = { "def f(x):  # c" };

	load_mode("Python", lines, 1);
	check_hl(0, "44405000000222");
	teardown();
}

/* An f-string is NOT one string.  The grammar spells it
 * (string (string_start) (string_content) (interpolation) ... (string_end))
 * and the interpolation is code: kg captures the pieces, so `{n}` comes
 * out unpainted between two painted halves.  A query that captured
 * (string) whole would paint this line uniformly and look right until
 * someone read the expression inside. */
static void test_python_fstring_interpolation(void)
{
	static const char *const lines[] = { "s = f\"hi {y} there\"" };

	load_mode("Python", lines, 1);
	check_hl(0, "0000666660006666666");
	teardown();
}

/* A triple-quoted string over rows: one (string_content) node covering
 * the middle, which is the multi-row construct Python contributes. */
static void test_python_triple_quoted_string(void)
{
	static const char *const lines[] = { "x = '''a", "b'''", "y = 1" };

	load_mode("Python", lines, 3);
	check_hl(0, "00006666");
	check_hl(1, "6666");
	check_hl(2, "00007");
	teardown();
}

/* Decorators, class names, annotations and the named None/True/False
 * nodes -- the four places the sketch's guesses had to be checked against
 * the grammar rather than assumed. */
static void test_python_decorator_class_and_annotation(void)
{
	static const char *const lines[] = {
		"@memoize",
		"class A:",
		"    n: int = None",
	};

	load_mode("Python", lines, 3);
	check_hl(0, "55555555"); /* the decorator, sigil included */
	check_hl(1, "44444050"); /* class, then the name it introduces */
	check_hl(2, "00000005550004444"); /* annotation @type, None @keyword */
	teardown();
}

/* ---- YAML ---- */

/* A mapping key is @type, a flow sequence's scalars keep their own faces,
 * and a comment runs to end of row. */
static void test_yaml_key_flow_and_comment(void)
{
	static const char *const lines[] = { "key: [1, true]  # c" };

	load_mode("YAML", lines, 1);
	check_hl(0, "5550007004444000222");
	teardown();
}

/* A block scalar is one capture from its `|` indicator to the last row of
 * its content, so every row of the block is @string without any row of it
 * being quoted. */
static void test_yaml_block_scalar(void)
{
	static const char *const lines[] = {
		"block: |",
		"  literal",
		"  more",
		"done: 1",
	};

	load_mode("YAML", lines, 4);
	check_hl(0, "55555006");
	check_hl(1, "666666666");
	check_hl(2, "666666");
	check_hl(3, "5555007");
	teardown();
}

/* Document markers, anchors and aliases: the sigil is part of the capture
 * (the (anchor) node, not its (anchor_name) child), so `&x` is coloured
 * whole. */
static void test_yaml_anchors_and_markers(void)
{
	static const char *const lines[] = { "---", "a: &x 1", "b: *x" };

	load_mode("YAML", lines, 3);
	check_hl(0, "444");
	check_hl(1, "5004407");
	check_hl(2, "50044");
	teardown();
}

/* ---- Markdown (block grammar) ---- */

/* A heading is the whole line.  The paragraph below it is plain text --
 * `*emphasis*` is the INLINE grammar's business, and kg does not load it
 * (Refinement decision 4), so this is what "block grammar only" looks
 * like from the outside. */
static void test_markdown_heading_and_paragraph(void)
{
	static const char *const lines[] = { "# Head", "", "a *b* c" };

	load_mode("Markdown", lines, 3);
	check_hl(0, "444444");
	check_hl(2, "0000000");
	teardown();
}

/* A fenced code block is one capture from the opening fence through the
 * closing one, info string included: three rows of @string here, and
 * ordinary text on the row after it. */
static void test_markdown_fenced_code_block(void)
{
	static const char *const lines[] = {
		"```python",
		"def f():",
		"```",
		"",
		"after",
	};

	load_mode("Markdown", lines, 5);
	check_hl(0, "666666666");
	check_hl(1, "66666666");
	check_hl(2, "666");
	check_hl(4, "00000");
	teardown();
}

/* A block quote reads as text that is in the file without being of it,
 * so it is @comment -- every row of it, marker included. */
static void test_markdown_block_quote(void)
{
	static const char *const lines[] = { "> quoted", "> more", "", "x" };

	load_mode("Markdown", lines, 4);
	check_hl(0, "22222222");
	check_hl(1, "222222");
	check_hl(3, "0");
	teardown();
}

/* List markers and thematic breaks: the marker is @keyword and its item's
 * text is not, which is the one place the block grammar draws a line
 * inside a row.
 *
 * The trailing "end" row is load-bearing, and the finding it encodes is
 * this grammar's, not kg's: a `---` on the LAST row of a document that
 * does not end in a newline parses as a (paragraph), not a
 * (thematic_break) -- the block scanner needs the line terminator before
 * it will commit.  kg's buffer never has a newline after its last row, so
 * a thematic break at end of buffer is genuinely plain text until
 * something is typed below it. */
static void test_markdown_list_and_thematic_break(void)
{
	static const char *const lines[] = { "- item", "", "---", "", "end" };

	load_mode("Markdown", lines, 5);
	check_hl(0, "440000");
	check_hl(2, "444");
	check_hl(4, "000");
	teardown();
}

/* ---- Batch 2: the trickiest choice per language ------------------------
 *
 * Two or three byte-exact paints each rather than a survey: what is worth
 * a case here is the assertion that would have gone the other way if the
 * query had been written from memory instead of from a probe.  The rest of
 * each language rides the machinery batch 1 already proved.
 *
 * (There is no Shell section, because there is no Shell row.  The
 * tree-sitter-bash this install ships loads -- the ABI-6 one batch 2 met
 * does not exist here any more -- so what Shell is waiting on is a row and
 * a query, not a grammar.) */

/* JavaScript: a template string is NOT one string.  `${n}` is code, so the
 * two backticks and the two fragments around the substitution are painted
 * and the substitution is not -- the same treatment Python's f-string gets,
 * arrived at from the same node shape. */
static void test_javascript_template_string(void)
{
	static const char *const lines[] = { "const s = `hi ${n} !`;" };

	load_mode("JavaScript", lines, 1);
	check_hl(0, "4444400000666600006660");
	teardown();
}

/* The JavaScript grammar's hardest ambiguity, and the reason this is a
 * parser and not a scanner: `/ab+/g` after `=` is a regex literal, and the
 * same characters after an identifier are two divisions.  Both rows are the
 * grammar's answer, not kg's. */
static void test_javascript_regex_versus_division(void)
{
	static const char *const lines[] = {
		"const r = /ab+/g;",
		"const q = a /b/ c;",
	};

	load_mode("JavaScript", lines, 2);
	check_hl(0, "44444000006666660");
	check_hl(1, "444440000000000000");
	teardown();
}

/* JSX, in an ordinary .js buffer: tree-sitter-javascript parses it, which
 * is why React is a row naming this grammar rather than a dependency of its
 * own.  The tag names are @type at both ends of the element. */
static void test_javascript_jsx_element(void)
{
	static const char *const lines[]
	    = { "const e = <Foo bar=\"b\">t</Foo>;" };

	load_mode("JavaScript", lines, 1);
	check_hl(0, "4444400000055500000666000055500");
	teardown();
}

/* React mode is the JavaScript grammar under another name, asserted rather
 * than assumed: the same bytes come out the same colour in both modes. */
static void test_react_mode_uses_javascript_grammar(void)
{
	static const char *const lines[] = { "const e = <Foo/>;" };

	load_mode("React", lines, 1);
	check_hl(0, "44444000000555000");
	teardown();
}

/* ---- TSX ---- */

/* Tag names in a .tsx buffer, in all three spellings the grammar gives
 * them, because "the tag name is @type" is one rule over three node types:
 * `<div>` is an (identifier), `<Wid.In>` a (member_expression) and
 * `<svg:rect>` a (jsx_namespace_name).  A fragment (`<>...</>`) has no name
 * field at all and paints nothing, which is the boundary of the rule rather
 * than a gap in it.
 *
 * Attribute names stay plain -- see the query -- but an attribute's VALUE
 * expression is ordinary TypeScript inside the tag, which is what the `1`
 * coming out @number on row 1 says. */
static void test_tsx_tag_names(void)
{
	static const char *const lines[]
	    = { "const a = <div>x</div>;", "const b = <Wid.In p={1} />;",
		      "const c = <svg:rect />;", "const d = <>frag</>;" };
	int i;

	setup(syntax_find_by_name("TypeScript"));
	bcur()->filename = (char *)"app.tsx";
	for (i = 0; i < 4; i++) {
		editor_insert_row(bcur(), i, lines[i], strlen(lines[i]));
	}
	syntax_rebuild(bcur());
	CHECK(bcur()->syntax_state != NULL);
	check_hl(0, "44444000000555000055500");
	check_hl(1, "444440000005555550000700000");
	check_hl(2, "44444000000555555550000");
	check_hl(3, "44444000000000000000");
	bcur()->filename = NULL;
	teardown();
}

/* The two TypeScript rows carry two query TEXTS now, and only one of them
 * may name a JSX node: `(jsx_opening_element)` does not exist in the
 * typescript grammar, so a .ts row that named it would fail to compile and
 * make every .ts file plain text.  test_registry_queries_compile() is what
 * catches that, and it catches it by compiling whatever each row names --
 * this asserts the two rows do not name the same thing, which is the
 * premise that makes the other test's coverage of the tsx literal real. */
static void test_tsx_and_ts_queries_differ(void)
{
	struct kg_ts_language *ts
	    = kg_ts_language_for_mode(KG_MODE_TYPESCRIPT, "app.ts");
	struct kg_ts_language *tsx
	    = kg_ts_language_for_mode(KG_MODE_TYPESCRIPT, "app.tsx");

	CHECK(ts != NULL && tsx != NULL);
	if (!ts || !tsx) {
		return;
	}
	CHECK(ts->query_text != tsx->query_text);
	CHECK(strstr(ts->query_text, "jsx_opening_element") == NULL);
	CHECK(strstr(tsx->query_text, "jsx_opening_element") != NULL);
	/* The shared body really is shared: a pattern only the TypeScript
	 * half has is in both. */
	CHECK(strstr(ts->query_text, "(accessibility_modifier) @keyword")
	    != NULL);
	CHECK(strstr(tsx->query_text, "(accessibility_modifier) @keyword")
	    != NULL);
}

/* ---- Rust ---- */

/* THE Rust assertion: `'a` is a lifetime and `'x'` is a character, and a
 * hand-written scanner cannot reliably tell them apart.  Here they are on
 * adjacent rows: the lifetime @type, the char @string. */
static void test_rust_lifetime_is_not_a_char(void)
{
	static const char *const lines[] = {
		"struct S<'a> { c: &'a str }",
		"const Q: char = 'x';",
	};

	load_mode("Rust", lines, 2);
	check_hl(0, "444444050550000000055055500");
	check_hl(1, "44444000055550006660");
	teardown();
}

/* An attribute is @type, the same call a Python decorator got, and the
 * inner form `#![...]` is a different node from the outer `#[...]`, so both
 * are spelled out in the query. */
static void test_rust_attributes(void)
{
	static const char *const lines[] = {
		"#![allow(dead_code)]",
		"#[derive(Debug)]",
	};

	load_mode("Rust", lines, 2);
	check_hl(0, "55555555555555555555");
	check_hl(1, "5555555555555555");
	teardown();
}

/* A raw string is its own node, which is how `r#"a " b"#` stays one span
 * through a quote that would close an ordinary literal. */
static void test_rust_raw_string(void)
{
	static const char *const lines[] = { "let r = r#\"a \" b\"#;" };

	load_mode("Rust", lines, 1);
	check_hl(0, "4440000066666666660");
	teardown();
}

/* ---- Go ---- */

/* A predeclared TYPE is an ordinary (type_identifier), so `int` is coloured
 * by being a type and not by being listed; the number and the one comment
 * node come out beside it. */
static void test_go_type_number_and_comment(void)
{
	static const char *const lines[] = { "var n int = 0xff // c" };

	load_mode("Go", lines, 1);
	check_hl(0, "444000555000777702222");
	teardown();
}

/* The predeclared CONSTANTS are named nodes rather than keyword tokens --
 * quoting "true" in the query would not compile -- and a rune literal is
 * its own node, so 'c' is a string span where Go calls it an integer. */
static void test_go_constants_and_rune(void)
{
	static const char *const lines[] = {
		"const ok = true",
		"var r = 'c'",
	};

	load_mode("Go", lines, 2);
	check_hl(0, "444440000004444");
	check_hl(1, "44400000666");
	teardown();
}

/* ---- HTML ---- */

/* The pair that makes markup readable: the tag name is @keyword and the
 * attribute name is @type, with the quoted value -- quotes included --
 * @string. */
static void test_html_tag_attribute_and_value(void)
{
	static const char *const lines[] = { "<p class=\"c\">t</p>" };

	load_mode("HTML", lines, 1);
	check_hl(0, "040555550666000040");
	teardown();
}

/* An unquoted attribute value is a bare (attribute_value) with no wrapper
 * node, so it needs its own capture; a comment is a comment. */
static void test_html_unquoted_value_and_comment(void)
{
	static const char *const lines[] = {
		"<!-- c -->",
		"<img src=a.png>",
	};

	load_mode("HTML", lines, 2);
	check_hl(0, "2222222222");
	check_hl(1, "044405550666660");
	teardown();
}

/* What "no language injection" looks like from outside: the <script> tags
 * are coloured and the JavaScript between them is plain, because the
 * grammar hands that over as one opaque (raw_text) and reaching into it
 * means a second parser (Refinement decision 4). */
static void test_html_script_content_is_plain(void)
{
	static const char *const lines[] = { "<script>var x = 1;</script>" };

	load_mode("HTML", lines, 1);
	check_hl(0, "044444400000000000004444440");
	teardown();
}

/* ---- Emacs Lisp ---- */

/* A defun builds (function_definition), which has a `name` field: the head
 * is an anonymous keyword token and the name it introduces is @type, the
 * same shape Python's def has. */
static void test_elisp_defun(void)
{
	static const char *const lines[] = { "(defun f (a) \"doc\" 42)" };

	load_mode("Lisp", lines, 1);
	check_hl(0, "0444440500000666660770");
	teardown();
}

/* A special form's head is a token of the node rather than a child symbol,
 * so `setq` is @keyword while `require` -- an ordinary function -- is not;
 * and `?c` is a character, which in Emacs Lisp is an integer. */
static void test_elisp_special_form_and_char(void)
{
	static const char *const lines[] = {
		"(setq x ?c) ; done",
		"(require 'cl-lib)",
	};

	load_mode("Lisp", lines, 2);
	check_hl(0, "044440007700222222");
	check_hl(1, "00000000000000000");
	teardown();
}

/* nil and t are keyword tokens of this grammar, not symbols. */
static void test_elisp_nil_and_t(void)
{
	static const char *const lines[] = { "(let ((a nil)) t)" };

	load_mode("Lisp", lines, 1);
	check_hl(0, "04440000044400040");
	teardown();
}

/* ---- Makefile ---- */

/* An assignment's name and a rule's target are @type -- the structure of
 * the file, the call YAML's mapping keys got. */
static void test_make_assignment_and_target(void)
{
	static const char *const lines[] = { "CC = gcc", "all: prog" };

	load_mode("Makefile", lines, 2);
	check_hl(0, "55000000");
	check_hl(1, "555000000");
	teardown();
}

/* A recipe line is shell and stays plain -- injecting a shell grammar is
 * Refinement decision 4's other side, and it stays plain now that a bash
 * grammar IS loaded for kg's Shell mode -- but the make-level references
 * inside it are not: a
 * `$(...)` reference and an automatic variable are the grammar's own nodes.
 *
 * The leading TAB is one byte of chars and eight of render, so this row is
 * also the coordinate seam again, in a language whose syntax is that tab. */
static void test_make_recipe_variables(void)
{
	static const char *const lines[] = { "all:", "\t$(CC) -o $@ x" };

	load_mode("Makefile", lines, 2);
	check_hl(0, "5550");
	check_hl(1, "000000004444400004400");
	teardown();
}

/* Conditionals are anonymous keyword tokens of their directive nodes. */
static void test_make_conditional(void)
{
	static const char *const lines[]
	    = { "ifeq ($(CC),gcc)", "X = 1", "endif" };

	load_mode("Makefile", lines, 3);
	check_hl(0, "4444004444400000");
	check_hl(1, "50000");
	check_hl(2, "44444");
	teardown();
}

/* ---- Java ---- */

/* Java splits the comment node in two -- (line_comment) and
 * (block_comment), where C has one (comment) -- and gives every number
 * base its own literal node, so a query naming only the decimal one would
 * leave 0xff uncoloured. */
static void test_java_types_numbers_and_comment(void)
{
	static const char *const lines[] = { "int n = 0xff; // c" };

	load_mode("Java", lines, 1);
	check_hl(0, "555000007777002222");
	teardown();
}

/* An annotation is @type, and a string ARGUMENT of one comes out @string:
 * the annotation node contains it, and the precedence table -- not the
 * order the query cursor walked -- decides which of the two overlapping
 * captures wins. */
static void test_java_annotation_over_string(void)
{
	static const char *const lines[] = { "@SuppressWarnings(\"unused\")" };

	load_mode("Java", lines, 1);
	check_hl(0, "555555555555555555666666665");
	teardown();
}

/* ---- Shell ---- */

/* The representative line: a command, a double-quoted string with an
 * expansion inside it, and a trailing comment.  The string is painted
 * through its pieces, so the `$USER` between them keeps its own face --
 * the same shape Python's f-string and JavaScript's template literal have,
 * and the reason the query does not capture (string) whole. */
static void test_shell_expansion_inside_string(void)
{
	static const char *const lines[] = { "echo \"hi $USER\" # who" };

	load_mode("Shell", lines, 1);
	check_hl(0, "555506666555556022222");
	teardown();
}

/* `-n` twice on two rows, meaning two different things, coloured two
 * different ways.  In `[ -n "$V" ]` it is a (test_operator) and @keyword;
 * in `echo -n "$V"` it is an argument word and plain.  Nothing about the
 * two bytes differs -- the enclosing command does -- so this is a
 * distinction only a parser can draw, and it is the reason the query
 * carries a (test_operator) pattern at all. */
static void test_shell_test_operator_versus_argument(void)
{
	static const char *const lines[]
	    = { "[ -n \"$V\" ]", "echo -n \"$V\"" };

	load_mode("Shell", lines, 2);
	check_hl(0, "00440655600");
	check_hl(1, "555500006556");
	teardown();
}

/* A heredoc is the multi-row construct this language contributes, and both
 * spellings of it are one @string block from the delimiter down.  The
 * `$V` on row 1 comes out @string rather than @type even though it is a
 * real expansion: (heredoc_body) is captured whole -- it has to be, since
 * the quoted body on row 4 is a leaf token with no pieces to capture -- and
 * @string outranks @type in the precedence table. */
static void test_shell_heredocs_quoted_and_not(void)
{
	static const char *const lines[]
	    = { "cat <<EOF", "a $V b", "EOF", "cat <<'RAW'", "a $V b", "RAW" };

	load_mode("Shell", lines, 6);
	check_hl(0, "555000666");
	check_hl(1, "666666");
	check_hl(2, "666");
	check_hl(3, "55500066666");
	check_hl(4, "666666");
	check_hl(5, "666");
	teardown();
}

/* A function name is @type, the rule Python's def and Rust's fn got, and a
 * `local` declaration is an anonymous keyword token of its own node rather
 * than a word in command position.  The leading TAB is one byte of chars
 * and eight of render, so this row is the coordinate seam as well. */
static void test_shell_function_and_declaration(void)
{
	static const char *const lines[] = { "greet() {", "\tlocal n=42", "}" };

	load_mode("Shell", lines, 3);
	check_hl(0, "555550000");
	check_hl(1, "000000004444405077");
	check_hl(2, "0");
	teardown();
}

/* What the query deliberately does NOT capture, asserted rather than
 * described: `$(...)` is not an expansion of a name, it is a command, so
 * the sigil and parentheses stay plain and `basename` inside them is
 * coloured for what it is -- a command in command position.  The shebang
 * needs no pattern of its own; bash parses it as an ordinary comment. */
static void test_shell_command_substitution_is_not_a_variable(void)
{
	static const char *const lines[]
	    = { "#!/bin/sh", "x=$(basename \"$0\")" };

	load_mode("Shell", lines, 2);
	check_hl(0, "222222222");
	check_hl(1, "500055555555065560");
	teardown();
}

/* ---- Lifecycle ---- */

/* A staged load -- rows that belong to no buffer yet -- comes out parsed
 * and painted, with a state for the buffer that adopts it.  This is the
 * only highlighting pass a file load pays for. */
static void test_prepare_rows_parses_and_paints(void)
{
	struct editor_syntax *c = syntax_find_by_name("C");
	struct kg_syntax_state *st;
	erow *rows = NULL;
	int numrows = 0, cap = 0, ok = 0;

	setup(c);
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "/* open", 7));
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "still", 5));
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "*/ int x;", 9));
	CHECK(kg_row_builder_render(rows, numrows) == 0);

	st = syntax_prepare_rows(rows, numrows, c, NULL, &ok);
	CHECK(ok == 1);
	CHECK(st != NULL);
	CHECK(rows[0].hl != NULL && rows[0].hl[0] == HL_COMMENT);
	CHECK(rows[1].hl != NULL && rows[1].hl[0] == HL_COMMENT);
	CHECK(rows[2].hl != NULL && rows[2].hl[0] == HL_COMMENT);
	CHECK(rows[2].hl[3] == HL_KEYWORD2); /* the int after the closer */
	syntax_state_discard(st);
	kg_row_builder_free(&rows, &numrows, &cap);
	teardown();
}

/* An edit through the gateway reaches the backend once, and slice 6
 * answers it with a full reparse: the rows the edit produced, and the rows
 * below them, are all correct afterwards.  The edit here opens a block
 * comment where there was none, which is the case a backend that only
 * looked at the replaced rows would get wrong. */
static void test_edit_reparses_whole_buffer(void)
{
	static const char *const lines[] = { "int a;", "int b;", "int c;" };
	struct kg_edit e;
	size_t begin, end;

	load_c(lines, 3);
	check_hl(0, "555000");
	check_hl(1, "555000");

	/* One transaction that replaces row 0 with two rows of block
	 * comment.  A backend that only re-examined the replaced span would
	 * still be right about rows 2 and 3 here; what this pins is that the
	 * multi-row replacement itself comes out right, offsets and all,
	 * from a tree that was thrown away and built again. */
	begin = buffer_row_col_to_position(bcur(), 0, 0);
	end = buffer_row_col_to_position(bcur(), 0, 6);
	e = kg_edit_user(bcur(), begin, end, "/* was a\nstill comment */", 25);
	CHECK(kg_buffer_replace(&e, NULL) == 1);

	CHECK(bcur()->numrows == 4);
	check_hl(0, "22222222");
	check_hl(1, "2222222222222222");
	check_hl(2, "555000");
	check_hl(3, "555000");
	teardown();
}

/* A buffer gets its state from the notification, and loses it the moment
 * its mode stops being one this backend parses.  b->syntax_state is the
 * pointer def.h keeps; what is behind it is nobody's business here. */
static void test_mode_change_acquires_and_releases_state(void)
{
	static const char *const lines[] = { "int x;" };

	load_c(lines, 1);
	CHECK(bcur()->syntax_state != NULL);
	check_hl(0, "555000");

	/* C#: a mode with no grammar in this build, so the state goes. */
	editor_set_syntax(bcur(), syntax_find_by_name("C#"));
	CHECK(bcur()->syntax_state == NULL);
	check_hl(0, "000000");

	editor_set_syntax(bcur(), syntax_find_by_name("C"));
	CHECK(bcur()->syntax_state != NULL);
	check_hl(0, "555000");

	/* And straight from one parsed mode to ANOTHER, with no unparsed
	 * mode in between: the state a buffer holds belongs to a language,
	 * so this is the release-and-reacquire the backend does when the two
	 * differ, not a reuse.  "int x;" is a valid Python expression
	 * statement, and Python paints none of it. */
	editor_set_syntax(bcur(), syntax_find_by_name("Python"));
	CHECK(bcur()->syntax_state != NULL);
	check_hl(0, "000000");
	teardown();
}

/* A mode with no grammar is plain text, with no state and no crash --
 * including through the row-at-a-time path, which is what an editor
 * command that touches one row uses. */
static void test_unsupported_mode_is_plain_text(void)
{
	static const char *const lines[] = { "var x = 42; // c" };
	int i;

	setup(syntax_find_by_name("C#"));
	editor_insert_row(bcur(), 0, lines[0], strlen(lines[0]));
	syntax_rebuild(bcur());
	CHECK(bcur()->syntax_state == NULL);
	for (i = 0; i < bcur()->row[0].rsize; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_NORMAL);
	}
	editor_update_syntax(bcur(), &bcur()->row[0]);
	CHECK(bcur()->row[0].hl[0] == HL_NORMAL);
	teardown();
}

/* The compat path, pinned rather than assumed: a row re-highlighted on its
 * own is answered from whatever tree the buffer currently has -- correctly
 * once there is one, and HL_NORMAL before there is.  Every editor path
 * that changes text goes through the gateway and therefore through
 * syntax_backend_after_edit(); this is what the leftovers get. */
static void test_row_at_a_time_needs_a_tree(void)
{
	static const char *const lines[] = { "int x;" };

	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, lines[0], strlen(lines[0]));
	check_hl(0, "000000"); /* no tree yet */
	syntax_rebuild(bcur());
	check_hl(0, "555000");
	editor_update_syntax(bcur(), &bcur()->row[0]);
	check_hl(0, "555000"); /* re-queried from the tree */
	teardown();
}

/* An empty row has no hl at all (the facade frees it), and a buffer of
 * nothing but empty rows must still parse and paint without reaching for
 * one. */
static void test_empty_rows(void)
{
	static const char *const lines[] = { "", "int x;", "" };

	load_c(lines, 3);
	CHECK(bcur()->row[0].hl == NULL);
	CHECK(bcur()->row[2].hl == NULL);
	check_hl(1, "555000");
	teardown();
}

/* ---- A real file ---- */

/* The end-to-end claim, and the only one that says the backend produces
 * something a person would see: a real source file comes out with a lot of
 * non-HL_NORMAL bytes, including types and comments.  Run from the repo
 * root, which is where `make check` runs it. */
static void test_real_source_file_is_colourful(void)
{
	static const char *const candidates[]
	    = { "src/main.c", "../src/main.c" };
	char line[4096];
	int painted = 0, keyword2 = 0, comment = 0, i, j, n = 0;
	FILE *fp = NULL;

	setup(syntax_find_by_name("C"));
	for (i = 0; i < 2 && !fp; i++) {
		fp = fopen(candidates[i], "r");
	}
	CHECK(fp != NULL);
	if (!fp) {
		teardown();
		return;
	}
	while (fgets(line, sizeof(line), fp)) {
		size_t len = strlen(line);

		while (len > 0
		    && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			len--;
		}
		editor_insert_row(bcur(), n++, line, len);
	}
	fclose(fp);
	syntax_rebuild(bcur());
	CHECK(bcur()->syntax_state != NULL);
	for (i = 0; i < bcur()->numrows; i++) {
		erow *row = &bcur()->row[i];

		for (j = 0; j < row->rsize; j++) {
			painted += row->hl[j] != HL_NORMAL;
			keyword2 += row->hl[j] == HL_KEYWORD2;
			comment += row->hl[j] == HL_COMMENT;
		}
	}
	CHECKF(painted > 100, "only %d painted bytes over %d rows", painted,
	    bcur()->numrows);
	CHECK(keyword2 > 0);
	CHECK(comment > 0);
	teardown();
}

/* ---- The differential: the incremental path against the full one -------
 *
 * The regression test doc/plans/kg-tree-sitter-plan.md calls the most
 * valuable one, and the thing that gives Phase 7 permission to repaint
 * fewer rows than it changed: apply an edit through the gateway, so the
 * backend answers it incrementally (ts_tree_edit() plus a parse against
 * the old tree, plus a repaint of the damaged rows only), then throw the
 * whole parse away and do the same text from nothing.  Every byte of every
 * row's hl must be the same.  Anything the damage window misses -- a row
 * whose colour changed without tree-sitter calling it changed, an offset
 * that survived the edit in the wrong coordinate space -- shows up here as
 * a difference, and nowhere else.
 *
 * Two layers, as the plan asks: a table of awkward edits, each named, and
 * a seeded random loop that prints its seed so a failure reproduces. */

/* The C fixture.  Every construct in it is one whose colouring depends on
 * text that is not on its own row: a comment spanning rows, a string
 * containing what would otherwise open one, preprocessor lines, nested
 * braces, a tab before a token and a multi-byte character before one.
 *
 * Each language with a fixture has one of these, and one table of edits,
 * and one alphabet for the random loop; the machinery below is shared, so
 * a language is three tables and a row in diff_langs[]. */
static const char *const c_fixture[] = {
	"#include <stdio.h>",
	"#define MAX(a, b) ((a) > (b) ? (a) : (b))",
	"",
	"/* A block comment",
	" * spanning three rows.",
	" */",
	"static const char *msg = \"hello /* not a comment */ world\";",
	"static const char *utf = \"caf\xc3\xa9\t\\ttab\";",
	"",
	"int main(int argc, char **argv)",
	"{",
	"\tint i = 0; /* trailing */",
	"\tfor (i = 0; i < 10; i++) {",
	"\t\tprintf(\"%d\\n\", i);",
	"\t}",
	"\tif (argc > 1) {",
	"\t\treturn 1; /* one */",
	"\t}",
	"\treturn 0;",
	"}",
};

#define C_FIXTURE_ROWS ((int)(sizeof(c_fixture) / sizeof(*c_fixture)))

/* A column that means "the end of whatever row this is":
 * buffer_row_col_to_position() clamps a column to the row's size, so a
 * case can name a row's end without knowing its width. */
#define EOL 100000

/* Every row's hl in two flat arenas -- the widths, and the bytes
 * concatenated -- so that comparing two of them is a memcmp and not a walk
 * of a structure.  An empty row contributes no bytes and a zero width,
 * which is exactly what the facade leaves behind for one. */
struct hl_snap {
	int numrows;
	int *rsize;
	size_t total;
	unsigned char *hl;
};

static void snap_free(struct hl_snap *s)
{
	free(s->rsize);
	free(s->hl);
	*s = (struct hl_snap) { 0 };
}

static int snap_take(struct editor_buffer *b, struct hl_snap *s)
{
	size_t at = 0;
	int i;

	*s = (struct hl_snap) { 0 };
	s->numrows = b->numrows;
	for (i = 0; i < b->numrows; i++) {
		s->total += (size_t)b->row[i].rsize;
	}
	s->rsize = calloc(
	    (size_t)(b->numrows > 0 ? b->numrows : 1), sizeof(*s->rsize));
	s->hl = calloc(s->total + 1, 1);
	if (!s->rsize || !s->hl) {
		snap_free(s);
		return 0;
	}
	for (i = 0; i < b->numrows; i++) {
		s->rsize[i] = b->row[i].rsize;
		if (b->row[i].rsize > 0) {
			memcpy(
			    s->hl + at, b->row[i].hl, (size_t)b->row[i].rsize);
			at += (size_t)b->row[i].rsize;
		}
	}
	return 1;
}

/* The whole arena in one memcmp: what the differential actually asks. */
static int snap_identical(const struct hl_snap *a, const struct hl_snap *b)
{
	return a->numrows == b->numrows && a->total == b->total
	    && memcmp(a->hl, b->hl, a->total) == 0
	    && memcmp(a->rsize, b->rsize,
		   sizeof(*a->rsize)
		       * (size_t)(a->numrows > 0 ? a->numrows : 1))
	    == 0;
}

/* Byte-identical, or the first byte that is not, with the row and offset
 * it is at and which side said what: the memcmp above answers "different"
 * and this answers "where", which is the difference between a failure that
 * is a bug report and one that is a puzzle. */
static void snap_expect_equal(const struct hl_snap *a, const struct hl_snap *b,
    const char *what, const char *lhs, const char *rhs)
{
	size_t at = 0;
	int i, j;

	if (snap_identical(a, b)) {
		return;
	}
	CHECKF(a->numrows == b->numrows, "%s: %d rows %s, %d %s", what,
	    a->numrows, lhs, b->numrows, rhs);
	if (a->numrows != b->numrows) {
		return;
	}
	for (i = 0; i < a->numrows; i++) {
		CHECKF(a->rsize[i] == b->rsize[i],
		    "%s: row %d is %d wide %s, %d %s", what, i, a->rsize[i],
		    lhs, b->rsize[i], rhs);
		if (a->rsize[i] != b->rsize[i]) {
			return;
		}
		for (j = 0; j < a->rsize[i]; j++) {
			CHECKF(a->hl[at + j] == b->hl[at + j],
			    "%s: row %d byte %d: hl %d %s, %d %s", what, i, j,
			    a->hl[at + j], lhs, b->hl[at + j], rhs);
			if (a->hl[at + j] != b->hl[at + j]) {
				return;
			}
		}
		at += (size_t)a->rsize[i];
	}
}

/* The comparison itself, and it is deliberately THREE snapshots, because
 * "the incremental answer differs from the full one" has two possible
 * causes and they belong to different projects:
 *
 *   inc   what the incremental edit left behind -- a parse against the
 *         edited old tree, and a repaint of the damaged rows only;
 *   wide  every row of that same tree repainted (editor_rehighlight_all()
 *         re-colours without reparsing), so it is what the incremental
 *         TREE says, with no damage window in the way;
 *   full  the state released -- parser, tree and all -- and the same text
 *         parsed by a parser that has never seen this document.
 *
 * inc == wide is KG's property and is checked strictly, always: it says
 * the damage window covered every row whose colour this tree implies, and
 * it is exactly what Phase 7 narrowed and therefore exactly what can be
 * wrong.  A row the window missed shows up here and nowhere else.
 *
 * wide == full is TREE-SITTER's property, and it is not one tree-sitter
 * promises for text containing syntax errors: an incremental parse of
 * broken input may recover differently from a fresh parse of the same
 * bytes, and both are legitimate trees.  Every deliberate case in the
 * table below holds it -- which is why they assert it -- and the random
 * loop counts the divergences it finds instead of failing on each one,
 * because a random edit produces broken C almost by definition.
 * Measured while writing this: 7 divergences in 18 900 random edits over
 * 63 seeds, none of them at the default seed, and every one of the five
 * examined with a has-error probe was on text that BOTH parses called
 * erroneous.
 *
 * The buffer is left holding the fresh state, so the next edit in a
 * sequence is an incremental one against a tree that is known good. */
static int differential_check_ex(const char *what, int strict_tree)
{
	struct hl_snap inc = { 0 }, wide = { 0 }, full = { 0 };
	int diverged;

	if (!snap_take(bcur(), &inc)) {
		CHECKF(0, "%s: out of memory taking the snapshot", what);
		return 0;
	}
	editor_rehighlight_all(bcur());
	if (!snap_take(bcur(), &wide)) {
		CHECKF(0, "%s: out of memory taking the snapshot", what);
		snap_free(&inc);
		return 0;
	}
	syntax_state_release(bcur());
	syntax_rebuild(bcur());
	if (!snap_take(bcur(), &full)) {
		CHECKF(0, "%s: out of memory taking the snapshot", what);
		snap_free(&inc);
		snap_free(&wide);
		return 0;
	}
	snap_expect_equal(&inc, &wide, what, "in the damaged rows",
	    "with every row repainted");
	diverged = !snap_identical(&wide, &full);
	if (strict_tree) {
		snap_expect_equal(&wide, &full, what,
		    "from the incremental tree", "from a fresh parse");
	}
	snap_free(&inc);
	snap_free(&wide);
	snap_free(&full);
	return diverged;
}

/* The whole differential, both halves strict: what a named, deliberate
 * case gets. */
static void differential_check(const char *what)
{
	differential_check_ex(what, 1);
}

/* One edit, in the terms the case table is written in.  Positions are
 * resolved against the buffer as it is now, so a table's rows can be
 * applied one after another as well as one at a time. */
static int apply_edit(int r0, int c0, int r1, int c1, const char *text)
{
	size_t begin = buffer_row_col_to_position(bcur(), r0, c0);
	size_t end = buffer_row_col_to_position(bcur(), r1, c1);
	struct kg_edit e = kg_edit_user(bcur(), begin, end, text, strlen(text));

	return kg_buffer_replace(&e, NULL);
}

struct diff_case {
	const char *name;
	int r0, c0, r1, c1;
	const char *text;
};

/* The awkward edits: each one changes what rows far away from it mean, or
 * sits on a boundary where an off-by-one would live. */
static const struct diff_case c_cases[] = {
	{ "open a block comment at the top", 0, 0, 0, 0, "/*" },
	{ "close a comment that was never opened", 0, 0, 0, 0, "*/" },
	{ "delete a block comment's opener", 3, 0, 3, 2, "" },
	{ "delete a block comment's closer", 5, 1, 5, 3, "" },
	{ "split a string across two rows", 6, 30, 6, 30, "\n" },
	{ "join the two rows a comment spans", 3, EOL, 4, 0, "" },
	{ "paste over a comment boundary", 4, 0, 6, 6, "*/ int x;\n/* re" },
	{ "delete the comment whole", 3, 0, 6, 0, "" },
	{ "edit at byte zero", 0, 0, 0, 1, "/" },
	{ "edit at the last byte", 19, 0, 19, EOL, "/*" },
	{ "append past the last byte", 19, EOL, 19, EOL, "\nint tail;" },
	{ "open a string at the top", 0, 0, 0, 0, "\"" },
	{ "a tab before a token", 11, 0, 11, 0, "\t" },
	{ "a multi-byte character before a token", 11, 1, 11, 1, "\xc3\xa9" },
	{ "delete everything", 0, 0, 19, EOL, "" },
	{ "replace everything with a comment", 0, 0, 19, EOL, "/* all */" },
};

/* ---- Python: the same machinery, a whitespace-significant grammar -----
 *
 * What is different about this fixture is not the constructs, it is that
 * INDENTATION is syntax: the grammar's external scanner emits indent and
 * dedent tokens, so an edit to a row's leading whitespace changes the
 * block structure of every row below it.  That is precisely where an
 * incremental parse's damage set is easiest to get wrong. */
static const char *const python_fixture[] = {
	"# module comment",
	"import os",
	"",
	"@memoize",
	"class Widget(Base):",
	"    \"\"\"A docstring",
	"    spanning rows.",
	"    \"\"\"",
	"",
	"    def method(self, n: int = 0) -> str:",
	"        s = f\"n is {n} of {self.total}\"",
	"        t = 'plain # not a comment'",
	"        if n > 0 and s is not None:",
	"            return s",
	"        try:",
	"            del t",
	"        except ValueError as e:",
	"            raise e",
	"        return None",
};

#define PYTHON_FIXTURE_ROWS                                                    \
	((int)(sizeof(python_fixture) / sizeof(*python_fixture)))

static const struct diff_case python_cases[] = {
	{ "py: open a triple quote at the top", 0, 0, 0, 0, "\"\"\"" },
	{ "py: delete the docstring's opener", 5, 4, 5, 7, "" },
	{ "py: delete the docstring's closer", 7, 4, 7, 7, "" },
	{ "py: dedent a method body row", 13, 0, 13, 4, "" },
	{ "py: indent a def row", 9, 0, 9, 0, "    " },
	{ "py: split a string across two rows", 11, 20, 11, 20, "\n" },
	{ "py: join the docstring's rows", 5, EOL, 6, 0, "" },
	{ "py: comment out the class header", 4, 0, 4, 0, "# " },
	{ "py: an unterminated quote at the top", 0, 0, 0, 0, "'" },
	{ "py: delete everything", 0, 0, 18, EOL, "" },
	{ "py: replace everything with a def", 0, 0, 18, EOL,
	    "def f():\n    pass" },
};

/* ---- YAML: indentation again, plus block scalars ---------------------- */
static const char *const yaml_fixture[] = {
	"# a yaml document",
	"---",
	"name: kg",
	"version: 1.1",
	"debug: false",
	"empty: null",
	"anchor: &base",
	"  a: 1",
	"  b: two",
	"merged: *base",
	"tagged: !!str 7",
	"quoted: \"has: colon\"",
	"list:",
	"  - one",
	"  - two",
	"block: |",
	"  literal line",
	"  second line",
	"folded: >",
	"  folded text",
	"...",
};

#define YAML_FIXTURE_ROWS ((int)(sizeof(yaml_fixture) / sizeof(*yaml_fixture)))

static const struct diff_case yaml_cases[] = {
	{ "yaml: open a block scalar at the top", 0, 0, 0, 0, "k: |" },
	{ "yaml: delete a block indicator", 15, 7, 15, 8, "" },
	{ "yaml: dedent a block scalar's content", 16, 0, 16, 2, "" },
	{ "yaml: indent a mapping key", 2, 0, 2, 0, "  " },
	{ "yaml: open a quote in a value", 3, EOL, 3, EOL, "\"" },
	{ "yaml: join a key and the row below", 12, EOL, 13, 0, "" },
	{ "yaml: delete the document marker", 1, 0, 1, EOL, "" },
	{ "yaml: turn a value into a comment", 4, 7, 4, 7, "# " },
	{ "yaml: delete everything", 0, 0, 20, EOL, "" },
	{ "yaml: replace everything with a list", 0, 0, 20, EOL,
	    "- one\n- two" },
};

/* ---- Markdown: block structure decided by blank rows and fences ------- */
static const char *const markdown_fixture[] = {
	"# Title",
	"",
	"A paragraph with *emphasis* and `code`.",
	"",
	"## Section",
	"",
	"> a quoted line",
	"> and another",
	"",
	"- first item",
	"- second item",
	"  1. nested item",
	"",
	"```python",
	"def f():",
	"    return 1",
	"```",
	"",
	"Setext",
	"======",
	"",
	"the end",
};

#define MARKDOWN_FIXTURE_ROWS                                                  \
	((int)(sizeof(markdown_fixture) / sizeof(*markdown_fixture)))

static const struct diff_case markdown_cases[] = {
	{ "md: open a fence at the top", 0, 0, 0, 0, "```\n" },
	{ "md: delete the closing fence", 16, 0, 16, EOL, "" },
	{ "md: promote a paragraph to a heading", 2, 0, 2, 0, "# " },
	{ "md: delete a heading marker", 0, 0, 0, 2, "" },
	{ "md: join a setext heading and its rule", 18, EOL, 19, 0, "" },
	{ "md: split the fenced block", 14, 0, 14, 0, "\n" },
	{ "md: quote a list item", 9, 0, 9, 0, "> " },
	{ "md: delete a list marker", 10, 0, 10, 2, "" },
	{ "md: remove the blank row before a list", 8, 0, 9, 0, "" },
	{ "md: delete everything", 0, 0, 21, EOL, "" },
	{ "md: replace everything with a fence", 0, 0, 21, EOL,
	    "```c\nint x;\n```" },
};

/* ---- Batch 2 and after: five languages, chosen for what they stress ---
 *
 * Not all ten.  The runner below is language-generic and batch 1 proved it
 * on four grammars; what an ordinary sixth language would add is runtime,
 * not evidence.  These five were picked because each breaks the machinery
 * in a different place if it is going to break at all:
 *
 *   Rust        a large grammar with an external scanner, raw strings and
 *               the lifetime/char ambiguity;
 *   HTML        node shapes unlike any other language here -- no
 *               statements, no expressions, an element is a tree of tags --
 *               and an external scanner that decides where raw text ends;
 *   Emacs Lisp  structure that is nothing but nesting, so an edit to one
 *               parenthesis re-parents everything after it;
 *   JavaScript  the external scanner that has to decide automatic
 *               semicolon insertion and regex-versus-division, which is
 *               the classic case of "the same bytes, a different tree,
 *               because of what came before them";
 *   Shell       the external scanner batch 2 wanted and could not have,
 *               because the tree-sitter-bash on the box was too old to
 *               load.  A heredoc is a body whose extent is decided by a
 *               delimiter word typed rows earlier, and `$(` opens a nested
 *               parse inside a string; both are scanner state that no
 *               edit's damage window contains, which is exactly what this
 *               differential is for.  JavaScript stood in for it while it
 *               was unavailable and keeps its own fixture, since the two
 *               scanners fail differently.
 *
 * TypeScript/TSX, Java and Makefile ride the same machinery with no
 * fixture of their own, which is the arrangement batch 1 established: a
 * language's own risk is in its QUERY, and that is what the byte-exact
 * cases above are for. */

static const char *const rust_fixture[] = {
	"//! crate docs",
	"#![allow(dead_code)]",
	"",
	"/* a block",
	" * comment */",
	"use std::fmt;",
	"",
	"#[derive(Debug, Clone)]",
	"pub struct Item<'a, T: Clone> {",
	"    pub name: &'a str,",
	"    count: u32,",
	"    tag: T,",
	"}",
	"",
	"impl<'a, T: Clone> Item<'a, T> {",
	"    pub fn new(name: &'a str, tag: T) -> Self {",
	"        let c = 'x';",
	"        let s = \"lit /* not a comment */\";",
	"        let r = r#\"raw \" string\"#;",
	"        let n = 0x2a + 1.5 as u32;",
	"        match name {",
	"            \"a\" => {}",
	"            _ => {}",
	"        }",
	"        Self { name, count: n, tag }",
	"    }",
	"}",
	"",
	"macro_rules! m { () => {} }",
};

#define RUST_FIXTURE_ROWS ((int)(sizeof(rust_fixture) / sizeof(*rust_fixture)))

static const struct diff_case rust_cases[] = {
	{ "rs: open a block comment at the top", 0, 0, 0, 0, "/*" },
	{ "rs: delete a block comment's closer", 4, 11, 4, 13, "" },
	{ "rs: make a lifetime look like a char", 9, 15, 9, 15, "'" },
	{ "rs: open a raw string at the top", 0, 0, 0, 0, "r#\"" },
	{ "rs: delete a raw string's hash", 18, 17, 18, 18, "" },
	{ "rs: comment out an attribute", 7, 0, 7, 0, "// " },
	{ "rs: unbalance the impl braces", 26, 0, 26, 1, "" },
	{ "rs: split a string across two rows", 17, 20, 17, 20, "\n" },
	{ "rs: join the two rows a comment spans", 3, EOL, 4, 0, "" },
	{ "rs: delete everything", 0, 0, 28, EOL, "" },
	{ "rs: replace everything with a fn", 0, 0, 28, EOL,
	    "fn f<'a>(s: &'a str) -> char { 'c' }" },
};

static const char *const html_fixture[] = {
	"<!DOCTYPE html>",
	"<!-- a comment",
	"     over rows -->",
	"<html lang=\"en\">",
	"  <head>",
	"    <title>kg &amp; co</title>",
	"    <meta charset=utf-8>",
	"  </head>",
	"  <body>",
	"    <p class='x' id=\"y\" disabled>text</p>",
	"    <img src=\"a.png\"/>",
	"    <script>",
	"      var x = 1; // not coloured",
	"    </script>",
	"    <style>.x { color: red; }</style>",
	"  </body>",
	"</html>",
};

#define HTML_FIXTURE_ROWS ((int)(sizeof(html_fixture) / sizeof(*html_fixture)))

static const struct diff_case html_cases[] = {
	{ "html: open a comment at the top", 0, 0, 0, 0, "<!--" },
	{ "html: delete a comment's closer", 2, 15, 2, 18, "" },
	{ "html: unclose an end tag", 7, 3, 7, 4, "" },
	{ "html: open a quote in an attribute", 3, 11, 3, 11, "\"" },
	{ "html: delete a script's end tag", 13, 4, 13, EOL, "" },
	{ "html: turn an element into text", 9, 4, 9, 5, "" },
	{ "html: split an attribute across rows", 9, 14, 9, 14, "\n" },
	{ "html: join a comment's rows", 1, EOL, 2, 0, "" },
	{ "html: delete everything", 0, 0, 16, EOL, "" },
	{ "html: replace everything with one element", 0, 0, 16, EOL,
	    "<p class=\"a\">x</p>" },
};

static const char *const elisp_fixture[] = {
	";;; init.el --- demo",
	";; a comment",
	"(require 'cl-lib)",
	"",
	"(defvar my-count 42",
	"  \"How many.\")",
	"(defconst my-ratio 1.5)",
	"",
	"(defun my-fun (a &optional b)",
	"  \"Docs for `my-fun'.\"",
	"  (let ((x (+ a 1))",
	"        (y ?c))",
	"    (if (and a b)",
	"        (message \"hi %s\" x)",
	"      (setq y nil))",
	"    `(a ,x ,@b)",
	"    #'my-fun))",
	"",
	"(lambda (x) (* x x))",
};

#define ELISP_FIXTURE_ROWS                                                     \
	((int)(sizeof(elisp_fixture) / sizeof(*elisp_fixture)))

static const struct diff_case elisp_cases[] = {
	{ "el: open a string at the top", 0, 0, 0, 0, "\"" },
	{ "el: delete a closing paren", 16, 12, 16, 13, "" },
	{ "el: unbalance a let", 10, 2, 10, 3, "" },
	{ "el: comment out a defun header", 8, 0, 8, 0, ";; " },
	{ "el: turn a char literal into a quote", 11, 11, 11, 12, "'" },
	{ "el: split a docstring across rows", 9, 10, 9, 10, "\n" },
	{ "el: delete a docstring's opening quote", 9, 2, 9, 3, "" },
	{ "el: join the defvar's two rows", 4, EOL, 5, 0, "" },
	{ "el: delete everything", 0, 0, 18, EOL, "" },
	{ "el: replace everything with a defun", 0, 0, 18, EOL,
	    "(defun f (x) \"d\" (let ((y 1)) y))" },
};

static const char *const javascript_fixture[] = {
	"// a module",
	"/* block",
	"   comment */",
	"import { a, b } from \"mod\";",
	"",
	"const re = /ab+\\/c/gi;",
	"const tpl = `x ${a} y`;",
	"const n = 0x2a, f = 1.5e3;",
	"",
	"class Widget extends Base {",
	"  #priv = null;",
	"  static async run(x = true) {",
	"    try {",
	"      await this.#priv;",
	"    } catch (e) {",
	"      throw new Error('bad ' + e);",
	"    }",
	"    return x ? undefined : null;",
	"  }",
	"}",
	"",
	"function App() {",
	"  return <div className=\"c\">hi {n}</div>;",
	"}",
	"",
	"export default App;",
};

#define JAVASCRIPT_FIXTURE_ROWS                                                \
	((int)(sizeof(javascript_fixture) / sizeof(*javascript_fixture)))

static const struct diff_case javascript_cases[] = {
	{ "js: open a block comment at the top", 0, 0, 0, 0, "/*" },
	{ "js: delete a block comment's closer", 2, 11, 2, 13, "" },
	{ "js: turn a regex into a division", 5, 11, 5, 12, "" },
	{ "js: open a template literal at the top", 0, 0, 0, 0, "`" },
	{ "js: split a template string across rows", 6, 16, 6, 16, "\n" },
	{ "js: comment out the class header", 9, 0, 9, 0, "// " },
	{ "js: unbalance the class braces", 19, 0, 19, 1, "" },
	{ "js: strip a JSX closing tag", 22, 34, 22, EOL, "" },
	{ "js: an unterminated string at the top", 0, 0, 0, 0, "'" },
	{ "js: join the two rows a comment spans", 1, EOL, 2, 0, "" },
	{ "js: delete everything", 0, 0, 25, EOL, "" },
	{ "js: replace everything with a class", 0, 0, 25, EOL,
	    "class A { m() { return `x`; } }" },
};

/* Shell.  Two constructs here carry state no other fixture does: a heredoc,
 * whose body ends at a delimiter word typed on an earlier row and whose
 * extent an edit far above it can change; and `$(...)` inside a
 * double-quoted string, which is a whole nested command parse begun in the
 * middle of a literal.  Both are the bash external scanner's answer, and an
 * incremental parse that hands it a stale span gets a different tree than a
 * parse from nothing -- which is what this differential is for. */
static const char *const shell_fixture[] = {
	"#!/bin/bash",
	"# a script",
	"set -euo pipefail",
	"",
	"NAME=\"world\"",
	"readonly n=42",
	"",
	"greet() {",
	"\tlocal who=${1:-$NAME}",
	"\techo \"hello $who\" >&2",
	"\tprintf '%s\\n' 'single'",
	"}",
	"",
	"if [ -n \"$NAME\" ]; then",
	"\tgreet \"$NAME\"",
	"elif test \"$#\" -gt 0; then",
	"\tfor f in *.txt; do",
	"\t\techo \"$(basename \"$f\") $((n + 1))\"",
	"\tdone",
	"else",
	"\twhile read -r line; do",
	"\t\tcase \"$line\" in",
	"\t\ta*) echo a ;;",
	"\t\t*) echo other ;;",
	"\t\tesac",
	"\tdone < input",
	"fi",
	"",
	"cat <<EOF",
	"heredoc $NAME body",
	"EOF",
	"",
	"cat <<'QUOTED'",
	"raw $NAME",
	"QUOTED",
};

#define SHELL_FIXTURE_ROWS                                                     \
	((int)(sizeof(shell_fixture) / sizeof(*shell_fixture)))

static const struct diff_case shell_cases[] = {
	{ "sh: open a heredoc at the top", 0, 0, 0, 0, "cat <<EOF\n" },
	{ "sh: rename a heredoc's delimiter", 28, 6, 28, 9, "END" },
	{ "sh: delete a heredoc's terminator", 30, 0, 30, EOL, "" },
	{ "sh: unquote a heredoc's delimiter", 32, 6, 32, 7, "" },
	{ "sh: join the rows a heredoc spans", 29, EOL, 30, 0, "" },
	{ "sh: open a command substitution at the top", 0, 0, 0, 0, "$(" },
	{ "sh: split a string across two rows", 9, 10, 9, 10, "\n" },
	{ "sh: an unterminated quote at the top", 0, 0, 0, 0, "\"" },
	{ "sh: an unterminated raw string at the top", 0, 0, 0, 0, "'" },
	{ "sh: turn a test operator into an argument", 13, 3, 13, 4, "" },
	{ "sh: comment out the function header", 7, 0, 7, 0, "# " },
	{ "sh: unbalance the function braces", 11, 0, 11, 1, "" },
	{ "sh: delete a case item's terminator", 22, 13, 22, EOL, "" },
	{ "sh: delete everything", 0, 0, 34, EOL, "" },
	{ "sh: replace everything with a function", 0, 0, 34, EOL,
	    "f() { echo \"$1\"; }" },
};

/* One language's differential material: what to load, what to edit, and
 * what a random edit is spelled out of.  The three tables above times
 * eight languages go through exactly the same runner, which is the point:
 * a language that diverges does so against the same machinery C is known
 * to hold up under, so the finding is about the language and not about
 * the harness. */
struct diff_lang {
	const char *mode; /* syntax_find_by_name() */
	const char *const *fixture;
	int rows;
	const struct diff_case *cases;
	unsigned int ncases;
	const char *const *tokens;
	unsigned int ntokens;
};

/* The alphabets a random replacement is built from: per language, the
 * tokens that change what the text around them means.  Whitespace is in
 * every one of them on purpose -- for three of these four grammars it is
 * syntax. */
static const char *const c_tokens[] = {
	"/*",
	"*/",
	"\"",
	"'",
	"\\",
	"\n",
	"\n\n",
	";",
	"{",
	"}",
	"(",
	")",
	"int ",
	"x",
	"//",
	"#define ",
	"\t",
	" ",
	"\xc3\xa9",
	"return 0;",
	"#if",
	"#endif",
	"0x2a",
	"'c'",
};

static const char *const python_tokens[] = {
	"#",
	"\"\"\"",
	"'''",
	"\"",
	"'",
	"\\",
	":",
	"\n",
	"\n    ",
	"    ",
	"\t",
	"def ",
	"class ",
	"return ",
	"None",
	"if ",
	"lambda ",
	"f\"",
	"(",
	")",
	"42",
	"0x2a",
	"# c",
	"\xc3\xa9",
};

static const char *const yaml_tokens[] = {
	"#",
	"-",
	":",
	": ",
	"|",
	">",
	"\"",
	"'",
	"\n",
	"\n  ",
	"  ",
	"&a",
	"*a",
	"!!str ",
	"---",
	"...",
	"true",
	"null",
	"42",
	"1.5",
	"key",
	"\xc3\xa9",
};

static const char *const markdown_tokens[] = {
	"#",
	"##",
	"```",
	"`",
	"*",
	"-",
	"+",
	">",
	"\n",
	"\n\n",
	"===",
	"---",
	"1. ",
	"    ",
	"[x](y)",
	"text",
	"\t",
	"\xc3\xa9",
};

static const char *const rust_tokens[] = {
	"//",
	"/*",
	"*/",
	"\"",
	"'",
	"'a",
	"r#\"",
	"\"#",
	"#[",
	"]",
	"\n",
	"{",
	"}",
	"(",
	")",
	";",
	"fn ",
	"let ",
	"impl ",
	"match ",
	"->",
	"u32",
	"0x2a",
	"1.5",
	" ",
	"\t",
	"\xc3\xa9",
};

static const char *const html_tokens[] = {
	"<",
	">",
	"</",
	"/>",
	"<!--",
	"-->",
	"\"",
	"'",
	"=",
	"\n",
	"div",
	"class",
	"&amp;",
	"<script>",
	"</script>",
	" ",
	"\t",
	"\xc3\xa9",
};

static const char *const elisp_tokens[] = {
	"(",
	")",
	"'",
	"`",
	",",
	",@",
	"\"",
	";",
	";; ",
	"?c",
	"\n",
	"\n  ",
	"defun ",
	"let ",
	"setq ",
	"nil",
	"t",
	"42",
	"1.5",
	"my-fun",
	" ",
	"\t",
	"\xc3\xa9",
};

static const char *const javascript_tokens[] = {
	"//",
	"/*",
	"*/",
	"\"",
	"'",
	"`",
	"${",
	"}",
	"\n",
	"\n  ",
	"(",
	")",
	"{",
	";",
	"const ",
	"function ",
	"class ",
	"return ",
	"=>",
	"/re/",
	"<div>",
	"</div>",
	"0x2a",
	"1.5",
	" ",
	"\t",
	"\xc3\xa9",
};

/* Shell's alphabet, and the one omission in any of them that is a finding
 * rather than a choice: `<<`, `<<EOF\n` and `EOF` are NOT here, so the
 * random loop perturbs the fixture's heredocs without manufacturing new
 * ones on top of them.  The five named heredoc edits in shell_cases[] hold
 * both halves of the differential strictly and are what covers the
 * construct.
 *
 * The reason is that with those tokens in, the random loop stops asking
 * about kg.  A document of overlapping unterminated heredocs makes
 * ts_tree_get_changed_ranges() incomplete: replaying seed 0x18c case 76 of
 * such a run through the C API, an edit at row 15 turns the token at byte
 * 92 from an anonymous `<<` [92,95] into a (heredoc_start) [92,102] -- a
 * different type over a different extent, on row 8 -- while the reported
 * changed ranges are [15.23-15.38] and [15.39-16.6] and name nothing on
 * row 8.  kg repaints the union of the edit's rows and those ranges, which
 * is the documented contract, so the row keeps a stale colour and inc ==
 * wide fails on a property kg does not own.  That is recorded in
 * doc/TODO.md; what does not belong here is a random alphabet whose
 * failures are somebody else's. */
static const char *const shell_tokens[] = {
	"#",
	"\"",
	"'",
	"$",
	"${",
	"}",
	"$(",
	")",
	"$((",
	"))",
	"\n",
	"\n\t",
	";",
	";;",
	"if ",
	"then",
	"fi",
	"for ",
	"do",
	"done",
	"case ",
	"esac",
	"echo ",
	"local ",
	"function ",
	"$NAME",
	"-n",
	"[",
	"]",
	"42",
	" ",
	"\t",
	"\xc3\xa9",
};

#define NTOK(a) ((unsigned int)(sizeof(a) / sizeof(*(a))))
#define NCASE(a) ((unsigned int)(sizeof(a) / sizeof(*(a))))

static const struct diff_lang diff_langs[] = {
	{ "C", c_fixture, C_FIXTURE_ROWS, c_cases, NCASE(c_cases), c_tokens,
	    NTOK(c_tokens) },
	{ "Python", python_fixture, PYTHON_FIXTURE_ROWS, python_cases,
	    NCASE(python_cases), python_tokens, NTOK(python_tokens) },
	{ "YAML", yaml_fixture, YAML_FIXTURE_ROWS, yaml_cases,
	    NCASE(yaml_cases), yaml_tokens, NTOK(yaml_tokens) },
	{ "Markdown", markdown_fixture, MARKDOWN_FIXTURE_ROWS, markdown_cases,
	    NCASE(markdown_cases), markdown_tokens, NTOK(markdown_tokens) },
	{ "Rust", rust_fixture, RUST_FIXTURE_ROWS, rust_cases,
	    NCASE(rust_cases), rust_tokens, NTOK(rust_tokens) },
	{ "HTML", html_fixture, HTML_FIXTURE_ROWS, html_cases,
	    NCASE(html_cases), html_tokens, NTOK(html_tokens) },
	{ "Lisp", elisp_fixture, ELISP_FIXTURE_ROWS, elisp_cases,
	    NCASE(elisp_cases), elisp_tokens, NTOK(elisp_tokens) },
	{ "JavaScript", javascript_fixture, JAVASCRIPT_FIXTURE_ROWS,
	    javascript_cases, NCASE(javascript_cases), javascript_tokens,
	    NTOK(javascript_tokens) },
	{ "Shell", shell_fixture, SHELL_FIXTURE_ROWS, shell_cases,
	    NCASE(shell_cases), shell_tokens, NTOK(shell_tokens) },
};

#define DIFF_LANGS ((unsigned int)(sizeof(diff_langs) / sizeof(*diff_langs)))

/* Each awkward edit on its own, against a fresh fixture: an incremental
 * answer and a from-scratch one, byte for byte, for every language. */
static void test_differential_case_table(void)
{
	unsigned int l, i;

	for (l = 0; l < DIFF_LANGS; l++) {
		const struct diff_lang *lang = &diff_langs[l];

		for (i = 0; i < lang->ncases; i++) {
			const struct diff_case *c = &lang->cases[i];

			load_mode(lang->mode, lang->fixture, lang->rows);
			CHECKF(apply_edit(c->r0, c->c0, c->r1, c->c1, c->text)
				== 1,
			    "%s: the edit was refused", c->name);
			differential_check(c->name);
			teardown();
		}
	}
}

/* The same edits again, this time one after another over the same buffer
 * with no reset between them, because an incremental parse is normally
 * given a tree that another incremental parse produced.  The check after
 * each one keeps a failure attributable to the edit that caused it. */
static void test_differential_chained_edits(void)
{
	static const struct diff_case chain[] = {
		{ "chain: open a comment", 0, 0, 0, 0, "/*" },
		{ "chain: close it two rows down", 2, 0, 2, 0, "*/" },
		{ "chain: delete the closer again", 2, 0, 2, 2, "" },
		{ "chain: open a string on row 1", 1, 0, 1, 0, "\"" },
		{ "chain: type into the string", 1, 1, 1, 1, "text /* x" },
		{ "chain: close the comment at the end", 3, EOL, 3, EOL,
		    " */" },
		{ "chain: join two rows", 0, EOL, 1, 0, "" },
	};
	size_t i;

	load_c(c_fixture, C_FIXTURE_ROWS);
	for (i = 0; i < sizeof(chain) / sizeof(*chain); i++) {
		CHECKF(apply_edit(chain[i].r0, chain[i].c0, chain[i].r1,
			   chain[i].c1, chain[i].text)
			== 1,
		    "%s: the edit was refused", chain[i].name);
		differential_check(chain[i].name);
	}
	teardown();
}

/* Typing into a buffer that has nothing in it -- the one starting state
 * the fixture cannot produce, and the one where the edit's start point is
 * the only position that exists. */
static void test_differential_empty_buffer(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "", 0);
	syntax_rebuild(bcur());
	CHECK(apply_edit(0, 0, 0, 0, "i") == 1);
	differential_check("empty buffer: first character");
	CHECK(apply_edit(0, 1, 0, 1, "nt x;\n/* c") == 1);
	differential_check("empty buffer: a comment opener");
	CHECK(apply_edit(0, 0, 1, EOL, "") == 1);
	differential_check("empty buffer: emptied again");
	teardown();
}

/* ---- The seeded random loop ---- */

/* xorshift32, so the sequence is the same on every box a seed is quoted
 * on; rand() promises nothing of the sort. */
static unsigned int diff_rand(unsigned int *state)
{
	unsigned int x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/* Up to three tokens of the language's own alphabet, concatenated.
 * Returns the length. */
static size_t diff_random_text(
    unsigned int *state, const struct diff_lang *lang, char *out, size_t outsz)
{
	unsigned int n = diff_rand(state) % 4;
	size_t len = 0;
	unsigned int i;

	out[0] = '\0';
	for (i = 0; i < n; i++) {
		const char *tok
		    = lang->tokens[diff_rand(state) % lang->ntokens];
		size_t tl = strlen(tok);

		if (len + tl + 1 >= outsz) {
			break;
		}
		memcpy(out + len, tok, tl);
		len += tl;
	}
	out[len] = '\0';
	return len;
}

/* The span an edit replaces: usually a few bytes, occasionally a big one,
 * because a buffer that only ever grows and shrinks by three bytes never
 * makes the parser re-lay a whole subtree. */
static void diff_random_span(
    unsigned int *state, size_t total, size_t *begin, size_t *end)
{
	size_t at = (size_t)diff_rand(state) % (total + 1);
	size_t width = (diff_rand(state) % 8 == 0)
	    ? (size_t)diff_rand(state) % 64
	    : (size_t)diff_rand(state) % 6;

	*begin = at;
	*end = at + width > total ? total : at + width;
}

/* One language's random run: `cases` edits from its own alphabet against
 * its own fixture, each checked immediately.  Returns nothing and reports
 * everything, because what a run is for is the RATE. */
static void random_edits_for(
    const struct diff_lang *lang, unsigned int seed0, int cases)
{
	unsigned int state = seed0 ? seed0 : 1u;
	int applied = 0, diverged = 0;
	char text[128];
	char what[256];
	int i;

	load_mode(lang->mode, lang->fixture, lang->rows);
	for (i = 0; i < cases; i++) {
		size_t begin, end, len;
		struct kg_edit e;

		diff_random_span(
		    &state, buffer_byte_length(bcur()), &begin, &end);
		len = diff_random_text(&state, lang, text, sizeof(text));
		e = kg_edit_user(bcur(), begin, end, text, len);
		if (!kg_buffer_replace(&e, NULL)) {
			continue;
		}
		snprintf(what, sizeof(what),
		    "%s seed 0x%x case %d: [%zu,%zu) <- %zu bytes", lang->mode,
		    seed0, i, begin, end, len);
		applied++;
		diverged += differential_check_ex(what, 0);
	}
	if (diverged) {
		fprintf(stderr,
		    "  (%s, seed 0x%x: %d of %d edits recovered from a syntax "
		    "error differently than a fresh parse)\n",
		    lang->mode, seed0, diverged, applied);
	}
	/* The shape is still asserted even though the individual case is
	 * not.  Error recovery diverges on a small fraction of edits; a
	 * wrong TSInputEdit, or an old tree that was never told about the
	 * edit, diverges on nearly all of them, and that is what this
	 * catches.  The bound is the same for every language deliberately:
	 * the whitespace-significant grammars are exactly the ones expected
	 * to recover differently more often, so a bound relaxed per language
	 * would stop measuring the thing it is for -- and measurement says
	 * they are not: over seeds 100..399 at 200 edits each (~60 000 edits
	 * per language), the divergences were Shell 59, C 39, JavaScript 26,
	 * Rust 10, Python 2, YAML 2, and HTML, Emacs Lisp and Markdown 0 --
	 * with the worst single seed 6 of 200 (Shell) and 2 of 200 for
	 * everything else, and the strict inc == wide half never once failing
	 * for any of the nine.  At the default seed only Shell diverges, once
	 * in 200.  The three grammars whose external scanner has to guess
	 * (bash's heredoc delimiters, JavaScript's regex-or-division, C's
	 * preprocessor) are the three that recover differently most often,
	 * which is the expected shape. */
	CHECKF(diverged * 10 <= applied,
	    "%s seed 0x%x: %d of %d edits disagreed with a fresh parse, which "
	    "is too many to be error recovery",
	    lang->mode, seed0, diverged, applied);
	teardown();
}

/* Random edits, checked after every one, for every language that has a
 * fixture -- batch 1's four, batch 2's four and Shell.  The
 * seed is printed with every failure and fixed by default, so CI runs the
 * same edits each time and a hunt can run different ones: KG_TS_DIFF_SEED
 * and KG_TS_DIFF_CASES are the knobs, the same shape
 * make check-regex-differential uses.
 *
 * Every language runs from the SAME seed rather than from a stirred one,
 * so a quoted seed reproduces all nine runs and the alphabets stay the
 * only difference between them.
 *
 * The damage window is checked strictly on every edit; the tree
 * comparison is counted rather than asserted, for the reason
 * differential_check_ex() gives at length -- tree-sitter does not promise
 * that an incremental parse of broken input recovers the way a fresh parse
 * of the same bytes does, and random edits produce broken input on
 * purpose.  The count is printed when it is not zero so that a change
 * which makes it jump is visible rather than silent. */
static void test_differential_random_edits(void)
{
	unsigned int seed0 = 0x9e3779b9u;
	int cases = 200;
	const char *env;
	unsigned int l;

	env = getenv("KG_TS_DIFF_SEED");
	if (env && *env) {
		seed0 = (unsigned int)strtoul(env, NULL, 0);
	}
	env = getenv("KG_TS_DIFF_CASES");
	if (env && *env) {
		cases = (int)strtol(env, NULL, 0);
	}
	for (l = 0; l < DIFF_LANGS; l++) {
		random_edits_for(&diff_langs[l], seed0, cases);
	}
}

int main(void)
{
	RUN(test_grammar_loads);
	RUN(test_missing_grammar_degrades);
	RUN(test_search_path_forms);
	RUN(test_env_overrides_search_path);
	RUN(test_registry_queries_compile);
	RUN(test_typescript_variant_selection);
	RUN(test_query_predicates_are_rejected);
	RUN(test_unregistered_mode_has_no_language);
	RUN(test_grammar_abi_is_rejected);
	RUN(test_declaration_number_and_comment);
	RUN(test_string_literals);
	RUN(test_multiline_block_comment);
	RUN(test_preproc_directive);
	RUN(test_tab_before_token);
	RUN(test_utf8_before_token);
	RUN(test_python_def_and_comment);
	RUN(test_python_fstring_interpolation);
	RUN(test_python_triple_quoted_string);
	RUN(test_python_decorator_class_and_annotation);
	RUN(test_yaml_key_flow_and_comment);
	RUN(test_yaml_block_scalar);
	RUN(test_yaml_anchors_and_markers);
	RUN(test_markdown_heading_and_paragraph);
	RUN(test_markdown_fenced_code_block);
	RUN(test_markdown_block_quote);
	RUN(test_markdown_list_and_thematic_break);
	RUN(test_javascript_template_string);
	RUN(test_javascript_regex_versus_division);
	RUN(test_javascript_jsx_element);
	RUN(test_react_mode_uses_javascript_grammar);
	RUN(test_tsx_tag_names);
	RUN(test_tsx_and_ts_queries_differ);
	RUN(test_rust_lifetime_is_not_a_char);
	RUN(test_rust_attributes);
	RUN(test_rust_raw_string);
	RUN(test_go_type_number_and_comment);
	RUN(test_go_constants_and_rune);
	RUN(test_html_tag_attribute_and_value);
	RUN(test_html_unquoted_value_and_comment);
	RUN(test_html_script_content_is_plain);
	RUN(test_elisp_defun);
	RUN(test_elisp_special_form_and_char);
	RUN(test_elisp_nil_and_t);
	RUN(test_make_assignment_and_target);
	RUN(test_make_recipe_variables);
	RUN(test_make_conditional);
	RUN(test_java_types_numbers_and_comment);
	RUN(test_java_annotation_over_string);
	RUN(test_shell_expansion_inside_string);
	RUN(test_shell_test_operator_versus_argument);
	RUN(test_shell_heredocs_quoted_and_not);
	RUN(test_shell_function_and_declaration);
	RUN(test_shell_command_substitution_is_not_a_variable);
	RUN(test_prepare_rows_parses_and_paints);
	RUN(test_edit_reparses_whole_buffer);
	RUN(test_mode_change_acquires_and_releases_state);
	RUN(test_unsupported_mode_is_plain_text);
	RUN(test_row_at_a_time_needs_a_tree);
	RUN(test_empty_rows);
	RUN(test_real_source_file_is_colourful);
	RUN(test_differential_case_table);
	RUN(test_differential_chained_edits);
	RUN(test_differential_empty_buffer);
	RUN(test_differential_random_edits);
	return test_summary();
}
