/* ===================== Mode registry and syntax facade ====================
 *
 * The backend-neutral half of syntax highlighting: what modes exist, which
 * one a file name selects, what a mode is called, what an HL_* face is in
 * terminal colours, and where a row's hl[] bytes come from.  The scanners
 * that fill those bytes in live behind src/syntax_backend.h -- today
 * src/syntax_legacy.c, chosen by the Makefile's source list.
 *
 * To add a mode: define its file-name patterns here and add a row to HLDB.
 * A pattern starting with a dot matches the end of the file name (".c");
 * any other pattern is searched anywhere inside it ("Makefile").  How the
 * mode is *highlighted* is the backend's business, keyed by the row's
 * enum kg_mode_id -- for the legacy backend, a legacy_syntax_spec row.
 *
 * There is no support to highlight patterns currently. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bufhandle.h"
#include "def.h"
#include "event.h"
#include "localvars.h"
#include "perf.h"
#include "syntax.h"
#include "syntax_backend.h"

/* C / C++ */
char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", ".cc", NULL };

/* Python */
char *PYTHON_HL_extensions[] = { ".py", ".pyw", ".pyi", ".pyx", NULL };

/* Shell */
char *SHELL_HL_extensions[] = { ".sh", ".bash", ".zsh", ".ksh", ".csh", ".tcsh",
	".profile", ".bashrc", ".bash_profile", ".bash_login", ".zshrc",
	".zshenv", ".zlogin", ".zprofile", NULL };

/* JavaScript */
char *JS_HL_extensions[] = { ".js", ".jsx", ".mjs", ".cjs", NULL };

/* Rust */
char *RUST_HL_extensions[] = { ".rs", ".rlib", NULL };

/* Java */
char *JAVA_HL_extensions[] = { ".java", ".class", NULL };

/* TypeScript */
char *TS_HL_extensions[] = { ".ts", ".tsx", ".d.ts", NULL };

/* C# */
char *CSHARP_HL_extensions[] = { ".cs", ".csx", NULL };

/* PHP */
char *PHP_HL_extensions[]
    = { ".php", ".phtml", ".php3", ".php4", ".php5", ".phps", NULL };

/* Ruby */
char *RUBY_HL_extensions[] = { ".rb", ".rbw", ".rake", ".gemspec", NULL };

/* Swift */
char *SWIFT_HL_extensions[] = { ".swift", NULL };

/* SQL */
char *SQL_HL_extensions[] = { ".sql", ".ddl", ".dml", NULL };

/* Dart */
char *DART_HL_extensions[] = { ".dart", NULL };

/* HTML */
char *HTML_HL_extensions[] = { ".html", ".htm", ".xhtml", NULL };

/* React/JSX - extends JavaScript with React-specific features */
char *REACT_HL_extensions[] = { ".jsx", NULL };

/* Vue.js - single file component syntax */
char *VUE_HL_extensions[] = { ".vue", NULL };

/* Angular - TypeScript with Angular decorators and directives */
char *ANGULAR_HL_extensions[] = { ".component.ts", ".service.ts", ".module.ts",
	".directive.ts", ".pipe.ts", ".guard.ts", NULL };

/* Svelte - single file component with reactive syntax */
char *SVELTE_HL_extensions[] = { ".svelte", NULL };

/* Makefile */
char *MAKE_HL_extensions[]
    = { "Makefile", "makefile", "GNUmakefile", ".mk", ".mak", NULL };

/* Markdown */
char *MD_HL_extensions[] = { ".md", ".markdown", ".mkd", NULL };

/* YAML */
char *YAML_HL_extensions[] = { ".yaml", ".yml", NULL };

/* Lisp */
char *LISP_HL_extensions[] = { ".el", ".lisp", ".lsp", NULL };

/* Git message files (.git/COMMIT_EDITMSG and friends).  Like the
 * rebase todo below, matched on the exact basename only in
 * editor_select_syntax_highlight(): the mode carries quit-the-editor
 * keys (C-c C-c / C-c C-k), so a name merely containing one of these
 * strings must not select it. */
static const char *gitcommit_basenames[]
    = { "COMMIT_EDITMSG", "MERGE_MSG", "SQUASH_MSG", "TAG_EDITMSG",
	      "NOTES_EDITMSG", "EDIT_DESCRIPTION", NULL };
char *GITCOMMIT_HL_extensions[] = { NULL };

/* Git interactive-rebase todo (.git/rebase-merge/git-rebase-todo).  No
 * filematch patterns: the mode carries quit-the-editor keys (C-c C-c /
 * C-c C-k), so it is selected only on an exact basename match in
 * editor_select_syntax_highlight(), never on a substring hit. */
char *GITREBASE_HL_extensions[] = { NULL };

/* The mode registry.  The first column is the entry's stable identity
 * (syntax.h's enum kg_mode_id), which is what mode semantics compares.
 * A row is what syntax_find_by_name() and syntax_find_by_mode() hand
 * back.  Every row's highlighter is NULL: how a registry mode is
 * fontified is the compiled-in backend's business, keyed by that
 * identity.  The column exists for the modes that own their rendering
 * outright -- dired's listing -- and lives outside this table. */
struct editor_syntax HLDB[] = {
	{ KG_MODE_C, "C", C_HL_extensions, "//", NULL },
	{ KG_MODE_PYTHON, "Python", PYTHON_HL_extensions, "#", NULL },
	{ KG_MODE_SHELL, "Shell", SHELL_HL_extensions, "#", NULL },
	{ KG_MODE_JAVASCRIPT, "JavaScript", JS_HL_extensions, "//", NULL },
	{ KG_MODE_RUST, "Rust", RUST_HL_extensions, "//", NULL },
	{ KG_MODE_JAVA, "Java", JAVA_HL_extensions, "//", NULL },
	{ KG_MODE_TYPESCRIPT, "TypeScript", TS_HL_extensions, "//", NULL },
	{ KG_MODE_CSHARP, "C#", CSHARP_HL_extensions, "//", NULL },
	{ KG_MODE_PHP, "PHP", PHP_HL_extensions, "//", NULL },
	{ KG_MODE_RUBY, "Ruby", RUBY_HL_extensions, "#", NULL },
	{ KG_MODE_SWIFT, "Swift", SWIFT_HL_extensions, "//", NULL },
	{ KG_MODE_SQL, "SQL", SQL_HL_extensions, "--", NULL },
	{ KG_MODE_DART, "Dart", DART_HL_extensions, "//", NULL },
	{ KG_MODE_HTML, "HTML", HTML_HL_extensions, "<!--", NULL },
	{ KG_MODE_REACT, "React", REACT_HL_extensions, "//", NULL },
	{ KG_MODE_VUE, "Vue", VUE_HL_extensions, "//", NULL },
	{ KG_MODE_ANGULAR, "Angular", ANGULAR_HL_extensions, "//", NULL },
	{ KG_MODE_SVELTE, "Svelte", SVELTE_HL_extensions, "//", NULL },
	{ KG_MODE_MAKEFILE, "Makefile", MAKE_HL_extensions, "", NULL },
	{ KG_MODE_MARKDOWN, "Markdown", MD_HL_extensions, "", NULL },
	{ KG_MODE_LISP, "Lisp", LISP_HL_extensions, ";", NULL },
	{ KG_MODE_GIT_COMMIT, "Git commit", GITCOMMIT_HL_extensions, "", NULL },
	{ KG_MODE_GIT_REBASE, "Git rebase", GITREBASE_HL_extensions, "", NULL },
	{ KG_MODE_YAML, "YAML", YAML_HL_extensions, "#", NULL },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* ======================== Git mode semantics ============================
 *
 * What a git-commit or git-rebase-todo line *means*, as opposed to what
 * colour it is: the C-c action keys read these, so they belong to the
 * facade and stay available whatever backend (if any) is highlighting. */

/* True when the current buffer is a git commit/merge/tag message. */
int syntax_is_git_commit(void)
{
	return bcur()->syntax && bcur()->syntax->id == KG_MODE_GIT_COMMIT;
}

/* Row index of the commit subject: the first non-blank row that is not
 * a '#' comment.  Returns -1 when the buffer has no subject yet. */
int syntax_git_commit_subject(void)
{
	int j;

	for (j = 0; j < bcur()->numrows; j++) {
		erow *r = &bcur()->row[j];

		if (r->rsize == 0 || r->render[0] == '#') {
			continue;
		}
		return j;
	}
	return -1;
}

/* One entry per git-rebase-todo command: long name, single-letter
 * abbreviation, and whether a commit hash follows the command word
 * (those are the lines C-c C-p and friends may rewrite). */
struct gitrebase_action {
	const char *name;
	char abbrev;
	int takes_commit;
};

static const struct gitrebase_action gitrebase_actions[] = {
	{ "pick", 'p', 1 },
	{ "reword", 'r', 1 },
	{ "edit", 'e', 1 },
	{ "squash", 's', 1 },
	{ "fixup", 'f', 1 },
	{ "drop", 'd', 1 },
	{ "exec", 'x', 0 },
	{ "break", 'b', 0 },
	{ "label", 'l', 0 },
	{ "reset", 't', 0 },
	{ "merge", 'm', 0 },
	{ "update-ref", 'u', 0 },
	{ "noop", 0, 0 },
};

#define GITREBASE_NACTIONS                                                     \
	((int)(sizeof(gitrebase_actions) / sizeof(gitrebase_actions[0])))

/* True when the current buffer is a git-rebase-todo. */
int syntax_is_git_rebase(void)
{
	return bcur()->syntax && bcur()->syntax->id == KG_MODE_GIT_REBASE;
}

/* Index into gitrebase_actions for the word at p[0..len), or -1. */
static int gitrebase_lookup(const char *p, int len)
{
	int i;

	for (i = 0; i < GITREBASE_NACTIONS; i++) {
		const struct gitrebase_action *a = &gitrebase_actions[i];

		if ((int)strlen(a->name) == len
		    && strncmp(a->name, p, len) == 0) {
			return i;
		}
		if (len == 1 && a->abbrev && p[0] == a->abbrev) {
			return i;
		}
	}
	return -1;
}

/* Index of the first byte at or after `i` that is not a space or tab.
 * Not static: the legacy backend's rebase scanner walks the same words
 * (src/syntax_backend.h), rather than keeping a second copy of the
 * vocabulary this file owns. */
int syntax_git_rebase_skip_ws(const char *p, int len, int i)
{
	while (i < len && (p[i] == ' ' || p[i] == '\t')) {
		i++;
	}
	return i;
}

/* Index just past the word starting at `i`. */
int syntax_git_rebase_skip_word(const char *p, int len, int i)
{
	while (i < len && p[i] != ' ' && p[i] != '\t') {
		i++;
	}
	return i;
}

/* The action word at p[0..len), for a caller that wants the vocabulary
 * rather than the pick-line verdict syntax_git_rebase_pick_span() gives:
 * the canonical long name, or NULL when the word names no action. */
const char *syntax_git_rebase_action_name(
    const char *p, int len, int *takes_commit)
{
	int a = gitrebase_lookup(p, len);

	if (a < 0) {
		return NULL;
	}
	*takes_commit = gitrebase_actions[a].takes_commit;
	return gitrebase_actions[a].name;
}

/* If the first word of line[0..len) is a commit-taking rebase action
 * (pick/reword/edit/squash/fixup/drop, long or abbreviated), store the
 * word's span in *start and *wlen and return 1; otherwise return 0.
 * Used by the C-c action keys to decide which lines they may rewrite. */
int syntax_git_rebase_pick_span(
    const char *line, int len, int *start, int *wlen)
{
	int i, w, a;

	i = syntax_git_rebase_skip_ws(line, len, 0);
	w = syntax_git_rebase_skip_word(line, len, i);
	if (w == i || line[i] == '#') {
		return 0;
	}
	a = gitrebase_lookup(line + i, w - i);
	if (a < 0 || !gitrebase_actions[a].takes_commit) {
		return 0;
	}
	*start = i;
	*wlen = w - i;
	return 1;
}

/* End of the option words (-C/-c and the like) following position
 * `from` in line[0..len): the index just past the last such word, or
 * `from` when none follow.  Lets the action keys drop flags that the
 * new action cannot take. */
int syntax_git_rebase_flags_end(const char *line, int len, int from)
{
	int i = from, end = from;

	for (;;) {
		i = syntax_git_rebase_skip_ws(line, len, i);
		if (i >= len || line[i] != '-') {
			return end;
		}
		i = syntax_git_rebase_skip_word(line, len, i);
		end = i;
	}
}

/* Make row->hl able to hold row->rsize bytes.  Reused like the render
 * buffer: a row that has been this wide before pays nothing, and a
 * failed grow leaves the old highlight in place rather than freeing what
 * row->rsize still describes. */
static int row_hl_reserve(erow *row)
{
	unsigned char *newhl;
	int newcap;

	if (row->rsize <= row->hl_capacity) {
		return 1;
	}
	newcap = editor_row_grown_capacity(row->rsize);
	KG_PERF_INC(KG_PERF_HL_ALLOC);
	KG_PERF_ADD(KG_PERF_HL_BYTES, newcap);
	newhl = realloc(row->hl, (size_t)newcap);
	if (!newhl) {
		editor_set_status_message("Out of memory");
		running = 0;
		return 0;
	}
	row->hl = newhl;
	row->hl_capacity = newcap;
	return 1;
}

/* One row's highlight, with no downstream propagation: reserve hl,
 * default it to HL_NORMAL, and hand the row to whoever colours it -- a
 * mode that owns its own rendering (dired), else the compiled-in backend.
 *
 * An empty row has no hl at all, only the cross-row state a backend keeps
 * in hl_oc, and an empty row can only inherit it from the row above, so
 * the facade settles that here rather than waking a backend for a row
 * with no bytes.  That is what the five legacy scanners each computed for
 * an empty row before this split -- Markdown's fence state, YAML's block
 * state and git-commit's subject flag all pass the previous row's value
 * straight through when rsize is 0, and Makefile's and git-rebase's
 * scanners return without touching hl_oc, which is 0 there and 0 in every
 * row above them. */
void syntax_update_row_only(struct editor_buffer *b, struct erow *row)
{
	KG_PERF_INC(KG_PERF_SYNTAX_ROW);
	KG_PERF_ADD(KG_PERF_SYNTAX_BYTES, row->rsize);
	if (row->rsize == 0) {
		free(row->hl);
		row->hl = NULL;
		row->hl_capacity = 0;
		row->hl_oc = 0;
		if (b->syntax && b->syntax->highlight) {
			b->syntax->highlight(
			    b, row); /* state propagation only */
		} else {
			row->hl_oc
			    = (row->idx > 0) ? b->row[row->idx - 1].hl_oc : 0;
		}
		return;
	}

	if (!row_hl_reserve(row)) {
		return;
	}
	memset(row->hl, HL_NORMAL, (size_t)row->rsize);

	if (b->syntax == NULL) {
		return; /* No syntax, everything is HL_NORMAL. */
	}

	if (b->syntax->highlight) {
		row->hl_oc = 0;
		b->syntax->highlight(b, row);
	} else {
		syntax_backend_update_row(b, row);
	}
}

/* Re-highlight the rows below row `idx` until one of them comes out with
 * the cross-row state it already had, which is where the change stops
 * being visible: every row below that one was computed from that same
 * state and is already what it would be computed to again.
 *
 * The caller decides whether row `idx` itself changed anything -- see
 * editor_update_syntax() for the row-at-a-time answer and the legacy
 * backend's syntax_backend_after_edit() for the transaction's.
 *
 * Not static: it is one of the facade's services to a backend
 * (src/syntax_backend.h), because hl_oc is a row field the facade owns. */
void syntax_propagate_below(struct editor_buffer *b, int idx)
{
	while (idx + 1 < b->numrows) {
		erow *row = &b->row[++idx];
		int old_oc = row->hl_oc;

		KG_PERF_INC(KG_PERF_SYNTAX_PROPAGATE);
		syntax_update_row_only(b, row);
		if (row->hl_oc == old_oc) {
			return;
		}
	}
}

/* Set every byte of row->hl (that corresponds to every character in the line)
 * to the right syntax highlight type (HL_* defines). */
void editor_update_syntax(struct editor_buffer *b, erow *row)
{
	int old_oc = row->hl_oc;

	syntax_update_row_only(b, row);
	if (row->hl_oc != old_oc) {
		syntax_propagate_below(b, row->idx);
	}
}

/* One completed edit transaction's whole highlighting cost, paid once
 * after kg_buffer_replace() has published the new rows and rendered them
 * -- not once per row, which is what a row-at-a-time rebuild used to make
 * a ten-row paste pay (ten independent updates, each with its own
 * downstream propagation, over a buffer topology that was still being
 * assembled).  A backend that keeps state over the whole text gets one
 * notification per edit, in the shape it can reparse from.
 *
 * What is HERE rather than in a backend is the part that is true of every
 * backend: the counter that says one transaction happened, the guard that
 * an edit outside the published rows is not an edit at all, and the
 * coordinate assertion.  Everything the answer depends on -- which rows
 * are re-coloured, and in what order -- is the backend's, because the two
 * backends genuinely disagree about it: the legacy scanners walk the
 * replacement's rows and then propagate their per-row carry downstream,
 * and a parser reparses and re-queries.  See syntax_backend_after_edit()
 * in the compiled-in backend for that half. */
void syntax_after_edit(
    struct editor_buffer *b, const struct kg_syntax_edit *edit)
{
	KG_PERF_INC(KG_PERF_SYNTAX_EDIT);
	if (edit->start_point.row < 0 || edit->start_point.row >= b->numrows) {
		return;
	}
	/* The consuming end of the coordinate seam: an edit's columns are
	 * bytes of row->chars, and the start row is published by now, so the
	 * claim is checkable here (doc/coordinates.md; armed by
	 * -DKG_DEBUG_COORDS=1, which .ci/ci-04 builds with). */
	KG_ASSERT_CHARS_OFF(
	    &b->row[edit->start_point.row], edit->start_point.column);
	syntax_backend_after_edit(b, edit);
}

void editor_rehighlight_from(struct editor_buffer *b, int start_idx)
{
	if (start_idx < 0 || start_idx >= b->numrows) {
		return;
	}
	editor_update_syntax(b, &b->row[start_idx]);
}

struct editor_syntax *syntax_find_by_name(const char *name)
{
	int j;
	for (j = 0; j < (int)HLDB_ENTRIES; j++) {
		if (strcmp(HLDB[j].name, name) == 0) {
			return &HLDB[j];
		}
	}
	return NULL;
}

/* The registry entry for a mode id, or NULL.  The synthetic modes
 * (Text, IBuffer, Compilation, Lisp Interaction, Dired) live outside HLDB
 * and never resolve here; their records are owned by bufmgr.c and dired.c. */
struct editor_syntax *syntax_find_by_mode(enum kg_mode_id id)
{
	int j;
	for (j = 0; j < (int)HLDB_ENTRIES; j++) {
		if (HLDB[j].id == id) {
			return &HLDB[j];
		}
	}
	return NULL;
}

void editor_rehighlight_all(struct editor_buffer *b)
{
	int j;
	for (j = 0; j < b->numrows; j++) {
		b->row[j].hl_oc = 0;
	}
	for (j = 0; j < b->numrows; j++) {
		syntax_update_row_only(b, &b->row[j]);
	}
}

/* "Everything changed": the notification for a path that rewrites a
 * buffer without being able to say what it replaced -- the counterpart of
 * syntax_after_edit() rather than a variant of it, so a backend holding a
 * parse tree knows to throw it away instead of trying to edit it.
 *
 * The legacy backend answers it with editor_rehighlight_all()'s body,
 * because it keeps no state a row rebuild does not: for it,
 * re-highlighting every row IS discarding the state.  The two names are
 * still separate, because the callers are: editor_rehighlight_all() is
 * "re-colour what is displayed", and this is "the text is not the text you
 * parsed", which is also the moment a parsing backend is allowed to
 * acquire state for a buffer that has none. */
void syntax_rebuild(struct editor_buffer *b) { syntax_backend_rebuild(b); }

/* The whole of editor_set_syntax(): reserve capacity for the
 * KG_EVENT_MODE_CHANGED event before the transition, assign the syntax and
 * rehighlight, then publish -- pulled into its own function so
 * editor_set_syntax() stays the single call this comment describes,
 * rather than gaining a branch of its own.  A refused reservation leaves
 * `b`'s syntax exactly as it was. */
static void editor_set_syntax_commit(
    struct editor_buffer *b, struct editor_syntax *syntax)
{
	struct kg_event_reservation res = kg_event_reserve_lifecycle();

	if (!res.valid) {
		editor_set_status_message(
		    "Too many pending events; mode not changed.");
		return;
	}
	if (b->syntax != syntax) {
		/* The mode really changes, so anything the backend derived
		 * under the old one describes a fontification that no longer
		 * applies -- the "mode change" release point.  Compared by
		 * identity, and released here rather than in the caller, so
		 * that a re-selection of the mode a buffer is already in
		 * costs nothing and a refused transition above loses
		 * nothing. */
		syntax_state_release(b);
	}
	b->syntax = syntax;
	/* syntax_rebuild() rather than editor_rehighlight_all(): the text is
	 * now being read under a different grammar, which is exactly the
	 * "what you parsed is not what this is" notification, and it is the
	 * one call on this path a parsing backend can acquire the new mode's
	 * state from.  For the legacy backend the two are the same body. */
	syntax_rebuild(b);
	kg_event_publish_lifecycle(&res,
	    kg_event_make_mode_changed(
		buf_handle_of(b), syntax ? syntax->name : NULL));
}

void editor_set_syntax(struct editor_buffer *b, struct editor_syntax *syntax)
{
	editor_set_syntax_commit(b, syntax);
}

/* ---- Per-buffer backend state ------------------------------------------
 *
 * The facade owns the *lifetime*; the backend owns the contents.  Four
 * calls, and they are deliberately small: an ownership rule that is spelled
 * out at each of its points is one a future backend cannot quietly leak
 * through. */

/* Prepare against a staged, unpublished row array.  The temporary record is
 * what turns "some rows and a mode" into the complete document a backend
 * needs to see -- it is never published and never observable, and the
 * backend is required to leave it alone. */
struct kg_syntax_state *syntax_prepare_rows(struct erow *rows, int numrows,
    struct editor_syntax *syntax, char *filename, int *ok)
{
	struct editor_buffer staged = { 0 };

	staged.row = rows;
	staged.numrows = numrows;
	staged.syntax = syntax;
	staged.filename = filename;
	return syntax_backend_prepare(&staged, ok);
}

void syntax_state_discard(struct kg_syntax_state *st)
{
	syntax_backend_state_free(st);
}

void syntax_state_release(struct editor_buffer *b)
{
	syntax_backend_state_free(b->syntax_state);
	b->syntax_state = NULL;
}

void syntax_state_adopt(struct editor_buffer *b, struct kg_syntax_state *st)
{
	syntax_state_release(b);
	b->syntax_state = st;
}

/* Maps syntax highlight token types to terminal colors. */
int editor_syntax_to_color(int hl)
{
	switch (hl) {
	case HL_COMMENT:
	case HL_MLCOMMENT:
		return 36; /* cyan */
	case HL_KEYWORD1:
		return 33; /* yellow */
	case HL_KEYWORD2:
		return 32; /* green */
	case HL_STRING:
		return 35; /* magenta */
	case HL_NUMBER:
		return 31; /* red */
	case HL_MATCH:
		return 34; /* blue */
	case HL_WARNING:
		return 31; /* red */
	default:
		return 37; /* white */
	}
}

/* Map a shebang interpreter name to a file extension for syntax lookup.
 * Supports versioned names (python3, python3.11) since shebangs often
 * pin specific versions.
 * Returns a dot-prefixed extension string, or NULL if unknown. */
static const char *shebang_interp_to_ext(const char *interp)
{
	static const struct {
		const char *name;
		const char *ext;
	} table[] = {
		{ "sh", ".sh" },
		{ "bash", ".bash" },
		{ "zsh", ".zsh" },
		{ "ksh", ".ksh" },
		{ "csh", ".csh" },
		{ "tcsh", ".tcsh" },
		{ "dash", ".sh" },
		{ "python", ".py" },
		{ "pypy", ".py" },
		{ "ruby", ".rb" },
		{ "node", ".js" },
		{ "nodejs", ".js" },
		{ "perl", ".pl" },
		{ NULL, NULL },
	};
	int i;

	for (i = 0; table[i].name; i++) {
		size_t len = strlen(table[i].name);
		if (strncmp(interp, table[i].name, len) == 0
		    && (interp[len] == '\0'
			|| isdigit((unsigned char)interp[len])
			|| interp[len] == '.')) {
			return table[i].ext;
		}
	}
	return NULL;
}

/* Try to select syntax by reading a hash-bang (#!) on the first line of
 * filename, falling back to extension matching via shebang_interp_to_ext(). */
static void select_syntax_by_shebang(
    struct editor_buffer *b, const char *filename)
{
	char line[256];
	char *interp, *slash, *end;
	const char *ext;
	unsigned int j, i;
	FILE *fp;

	fp = fopen(filename, "r");
	if (!fp) {
		return;
	}
	char *got = fgets(line, sizeof(line), fp);
	fclose(fp);
	if (!got) {
		return;
	}

	if (line[0] != '#' || line[1] != '!') {
		return;
	}

	/* Strip leading spaces after #! */
	interp = line + 2;
	while (*interp == ' ') {
		interp++;
	}

	/* Take the last path component: "/usr/bin/bash" → "bash" */
	slash = strrchr(interp, '/');
	if (slash) {
		interp = slash + 1;
	}

	/* If the component is "env", the real interpreter is the next word.
	 * Handles "#!/usr/bin/env python3" and "#!env bash". */
	if (strncmp(interp, "env", 3) == 0
	    && (interp[3] == '\0' || isspace((unsigned char)interp[3]))) {
		interp += 3;
		while (*interp == ' ') {
			interp++;
		}
	}

	/* Trim at whitespace (strips newline and any arguments) */
	end = interp;
	while (*end && !isspace((unsigned char)*end)) {
		end++;
	}
	*end = '\0';

	if (*interp == '\0') {
		return;
	}

	ext = shebang_interp_to_ext(interp);
	if (!ext) {
		return;
	}

	for (j = 0; j < HLDB_ENTRIES; j++) {
		struct editor_syntax *s = HLDB + j;
		for (i = 0; s->filematch[i]; i++) {
			if (strcmp(s->filematch[i], ext) == 0) {
				b->syntax = s;
				return;
			}
		}
	}
}

/* Select the syntax highlight scheme depending on the filename,
 * setting it on the buffer that is being given that filename. */
void editor_select_syntax_highlight(struct editor_buffer *b, char *filename)
{
	unsigned int j;
	/* The git modes are matched on the exact basename only: they bind
	 * keys that quit the editor, so a file whose name merely contains
	 * one of these strings must not select them. */
	const char *base = buf_basename(filename);

	if (strcmp(base, "git-rebase-todo") == 0) {
		b->syntax = syntax_find_by_name("Git rebase");
		return;
	}
	for (j = 0; gitcommit_basenames[j]; j++) {
		if (strcmp(base, gitcommit_basenames[j]) == 0) {
			b->syntax = syntax_find_by_name("Git commit");
			return;
		}
	}

	for (j = 0; j < HLDB_ENTRIES; j++) {
		struct editor_syntax *s = HLDB + j;
		unsigned int i = 0;
		while (s->filematch[i]) {
			int patlen = strlen(s->filematch[i]);
			char *p = strstr(filename, s->filematch[i]);
			if (p
			    && (s->filematch[i][0] != '.'
				|| p[patlen] == '\0')) {
				b->syntax = s;
				return;
			}
			i++;
		}
	}

	/* No extension match — try hash-bang on first line of file */
	select_syntax_by_shebang(b, filename);
}
