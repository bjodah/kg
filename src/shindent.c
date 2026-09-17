/* Shell-script indentation: the rules, and the two edits.
 *
 * The computation is pure over a row array (shindent_target_for_rows(),
 * shindent_newline_indent(), shindent_build_indent()), so the suite
 * drives it without a live buffer; the two command-level functions at
 * the bottom are the only ones that touch editor state.  Rows are read
 * as bytes of row->chars -- doc/coordinates.md's **chars** space -- and
 * targets are visual columns with tabs expanded, which is what makes a
 * TAB-indented line above and a space-indented one below agree about
 * the level they describe.
 *
 * What counts as code on a line ignores a trailing ` #' comment and
 * trailing whitespace, so `if x; then # open' still opens the block.
 * A `#' inside a word (`a#b') is not a comment, matching the shell. */

#include "shindent.h"

#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "edit.h"
#include "localvars.h"
#include "syntax.h"

/* Which opener a dedenting line is looking for.  Patterns scan for
 * their `case' separately (is_pattern_scan), so they need no member. */
enum sh_closer {
	SH_CLOSE_NONE,
	SH_CLOSE_FI,
	SH_CLOSE_DONE,
	SH_CLOSE_ESAC,
	SH_CLOSE_RBRACE,
	SH_CLOSE_ELIF_ELSE,
};

static int is_ws(char c) { return c == ' ' || c == '\t'; }

static int is_wordch(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
	    || (c >= '0' && c <= '9') || c == '_';
}

/* A separator before a keyword's last word: `x;then' ends in the word
 * `then', `xthen' does not. */
static int is_sep(char c)
{
	return c == ' ' || c == '\t' || c == ';' || c == '&' || c == '|'
	    || c == '(' || c == ')' || c == '{' || c == '}';
}

/* Code length of a line: cut a trailing ` #' comment, then trailing
 * whitespace.  Comment-only and blank lines both answer 0, which is
 * what makes them inherit the surrounding indent. */
static int code_len(const char *s, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (s[i] == '#' && (i == 0 || is_ws(s[i - 1]))) {
			len = i;
			break;
		}
	}
	while (len > 0 && is_ws(s[len - 1])) {
		len--;
	}
	return len;
}

/* Skip a code span's leading whitespace: classification reads words,
 * and the indent is not one.  Measuring the indent is the caller's own
 * job (indent_width()/indent_bytes()), never this one's. */
static void strip_leading(const char **s, int *len)
{
	while (*len > 0 && is_ws(**s)) {
		(*s)++;
		(*len)--;
	}
}

/* True when `word' opens the code span, on a word boundary. */
static int starts_word(const char *s, int len, const char *word)
{
	int n = (int)strlen(word);

	if (len < n || memcmp(s, word, (size_t)n) != 0) {
		return 0;
	}
	return len == n || !is_wordch(s[n]);
}

/* True when `word' closes the code span, on a word boundary. */
static int ends_word(const char *s, int len, const char *word)
{
	int n = (int)strlen(word);
	int at;

	if (len < n || memcmp(s + len - n, word, (size_t)n) != 0) {
		return 0;
	}
	at = len - n;
	return at == 0 || is_sep(s[at - 1]);
}

static int is_exact(const char *s, int len, const char *word)
{
	return len == (int)strlen(word) && memcmp(s, word, (size_t)len) == 0;
}

/* Visual width of a row's leading whitespace, tabs expanded. */
static int indent_width(const erow *row, int tab_width)
{
	int i;
	int col = 0;

	if (tab_width < 1) {
		tab_width = 8;
	}
	for (i = 0; i < row->size && is_ws(row->chars[i]); i++) {
		if (row->chars[i] == '\t') {
			col += tab_width - col % tab_width;
		} else {
			col++;
		}
	}
	return col;
}

/* Byte count of a row's leading whitespace. */
static int indent_bytes(const erow *row)
{
	int i = 0;

	while (i < row->size && is_ws(row->chars[i])) {
		i++;
	}
	return i;
}

/* A line after which the next one sits one level deeper: `...; then',
 * `...; do' (and the bare words), a trailing `{', a trailing backslash,
 * a lone `else', and `case ... in'.  One-line blocks (`if x; then y;
 * fi') end in `fi', not in an opener, so they stay flat with no special
 * case. */
static int is_opener(const char *s, int len)
{
	strip_leading(&s, &len);
	if (len == 0) {
		return 0;
	}
	if (s[len - 1] == '{' || s[len - 1] == '\\') {
		return 1;
	}
	if (ends_word(s, len, "then") || ends_word(s, len, "do")) {
		return 1;
	}
	if (is_exact(s, len, "else")) {
		return 1;
	}
	return starts_word(s, len, "case") && ends_word(s, len, "in");
}

/* The dedenting words that start a line: `fi', `done', `esac', `elif',
 * `else', and a leading `}'.  `;;' is not one -- it sits at the body
 * level, which the neighbour rule below already gives it -- but it
 * counts as a closer while scanning, where it pairs against its case. */
static enum sh_closer self_closer(const char *s, int len)
{
	strip_leading(&s, &len);
	if (len == 0) {
		return SH_CLOSE_NONE;
	}
	if (s[0] == '}') {
		return SH_CLOSE_RBRACE;
	}
	if (starts_word(s, len, "fi")) {
		return SH_CLOSE_FI;
	}
	if (starts_word(s, len, "done")) {
		return SH_CLOSE_DONE;
	}
	if (starts_word(s, len, "esac")) {
		return SH_CLOSE_ESAC;
	}
	if (starts_word(s, len, "elif") || is_exact(s, len, "else")) {
		return SH_CLOSE_ELIF_ELSE;
	}
	return SH_CLOSE_NONE;
}

/* A case pattern: `a)', `a|b)', `*)'.  The bytes before the first `)'
 * must be pattern characters only -- no space, no `=', no `$(` -- so
 * `y=$(foo)' stays an ordinary line. */
static int is_pattern(const char *s, int len)
{
	int i;

	strip_leading(&s, &len);
	if (len == 0 || s[0] == '(' || s[len - 1] != ')') {
		return 0;
	}
	for (i = 0; s[i] != ')'; i++) {
		if (s[i] == ' ' || s[i] == '\t' || s[i] == '=') {
			return 0;
		}
		if (s[i] == '$' && s[i + 1] == '(') {
			return 0;
		}
	}
	return 1;
}

/* A `;;' arm terminator.  It keeps the body level -- the neighbour
 * rule below already gives it that -- and only counts as a closer
 * while scanning, where it pairs against its case. */
static int is_dangle(const char *s, int len)
{
	strip_leading(&s, &len);
	return len >= 2 && s[0] == ';' && s[1] == ';';
}

/* A chained continuation: `foo &&' and `foo ||' indent the next line
 * one level, the way sh-mode lays an unfinished list.  Only the next
 * line reads this -- is_opener() does not -- so a closer above the
 * chain still scans straight past it to its real opener. */
static int is_chain(const char *s, int len)
{
	strip_leading(&s, &len);
	return ends_word(s, len, "&&") || ends_word(s, len, "||");
}

/* True when a scanning line closes a level: any dedenting line, plus
 * `;;'. */
static int scan_counts_close(const char *s, int len)
{
	if (is_dangle(s, len)) {
		return 1;
	}
	return self_closer(s, len) != SH_CLOSE_NONE;
}

/* The line a `kind' closer dedents to.  A bare `do' matches `done':
 * the `for' line above may not carry the `do' itself. */
static int opener_matches(enum sh_closer kind, const char *s, int len)
{
	strip_leading(&s, &len);
	switch (kind) {
	case SH_CLOSE_FI:
	case SH_CLOSE_ELIF_ELSE:
		return starts_word(s, len, "if");
	case SH_CLOSE_DONE:
		return starts_word(s, len, "for")
		    || starts_word(s, len, "while")
		    || starts_word(s, len, "until")
		    || starts_word(s, len, "select") || is_exact(s, len, "do");
	case SH_CLOSE_ESAC:
		return starts_word(s, len, "case");
	case SH_CLOSE_RBRACE:
		return len > 0 && s[len - 1] == '{';
	case SH_CLOSE_NONE:
		return 0;
	}
	return 0;
}

/* One scanning row's part in the search: answered, one level deeper,
 * one level paired off, or walked past.  An answer arrives in *target;
 * the depth itself is the caller's to keep. */
enum sh_scan_verdict {
	SH_SCAN_PASS,
	SH_SCAN_MATCH,
	SH_SCAN_CLOSE,
	SH_SCAN_PAIR,
};

static enum sh_scan_verdict scan_classify(const erow *row, int tab_width,
    enum sh_closer kind, int is_pattern_scan, int depth, int *target)
{
	int clen = code_len(row->chars, row->size);
	const char *s = row->chars;

	if (clen == 0) {
		return SH_SCAN_PASS;
	}
	strip_leading(&s, &clen);
	/* `elif' and `else' belong to the `if' at their own depth: at
	 * zero that is the match, deeper they are interior to a block
	 * whose own opener still pairs off above. */
	if (starts_word(s, clen, "elif") || is_exact(s, clen, "else")) {
		if (depth == 0) {
			*target = indent_width(row, tab_width);
			return SH_SCAN_MATCH;
		}
		return SH_SCAN_PASS;
	}
	if (scan_counts_close(s, clen)) {
		return SH_SCAN_CLOSE;
	}
	if (is_pattern(s, clen)) {
		/* A pattern while closing is broken code (a `fi' inside a
		 * case arm): align with it rather than invent a level. */
		if (depth == 0) {
			*target = indent_width(row, tab_width);
			return SH_SCAN_MATCH;
		}
		return SH_SCAN_PAIR;
	}
	if (is_pattern_scan ? starts_word(s, clen, "case")
			    : opener_matches(kind, s, clen)) {
		if (depth == 0) {
			*target = indent_width(row, tab_width)
			    + (is_pattern_scan ? SH_INDENT_WIDTH : 0);
			return SH_SCAN_MATCH;
		}
		return SH_SCAN_PAIR;
	}
	/* The wrong kind of opener at depth zero is broken code too:
	 * align with the construct the line sits in. */
	if (is_opener(s, clen)) {
		if (depth == 0) {
			*target = indent_width(row, tab_width);
			return SH_SCAN_MATCH;
		}
		return SH_SCAN_PAIR;
	}
	return SH_SCAN_PASS;
}

/* Dedent target of the closer on row `idx' (or the `case' a pattern
 * sits in, plus one level), or -1 when no opener is above it. */
static int scan_match(const erow *rows, int idx, int tab_width,
    enum sh_closer kind, int is_pattern_scan)
{
	int depth = 0;
	int j;

	for (j = idx - 1; j >= 0; j--) {
		int target = 0;

		switch (scan_classify(&rows[j], tab_width, kind,
		    is_pattern_scan, depth, &target)) {
		case SH_SCAN_MATCH:
			return target;
		case SH_SCAN_CLOSE:
			depth++;
			break;
		case SH_SCAN_PAIR:
			depth--;
			break;
		case SH_SCAN_PASS:
			break;
		}
	}
	return -1;
}

/* Nearest row below `idx' carrying code, or -1. */
static int prev_nonblank(const erow *rows, int idx)
{
	int j;

	for (j = idx - 1; j >= 0; j--) {
		if (code_len(rows[j].chars, rows[j].size) > 0) {
			return j;
		}
	}
	return -1;
}

/* What an ordinary line takes from the code above it: an opener or a
 * pattern deepens, a closer reuses its own match (so a line after `fi'
 * sits where the `fi' does), anything else continues the level. */
static int target_after(const erow *rows, int prev, int tab_width)
{
	const char *s;
	int clen;
	enum sh_closer closer;
	int m;

	if (prev < 0) {
		return 0;
	}
	s = rows[prev].chars;
	clen = code_len(s, rows[prev].size);
	if (is_opener(s, clen) || is_pattern(s, clen) || is_chain(s, clen)) {
		return indent_width(&rows[prev], tab_width) + SH_INDENT_WIDTH;
	}
	closer = self_closer(s, clen);
	if (closer != SH_CLOSE_NONE) {
		m = scan_match(rows, prev, tab_width, closer, 0);
		if (m >= 0) {
			return m;
		}
		m = indent_width(&rows[prev], tab_width) - SH_INDENT_WIDTH;
		return m > 0 ? m : 0;
	}
	return indent_width(&rows[prev], tab_width);
}

int shindent_target_for_rows(
    const erow *rows, int numrows, int idx, int tab_width)
{
	const char *s;
	int clen;
	enum sh_closer closer;
	int prev;
	int m;

	if (!rows || idx <= 0 || idx >= numrows) {
		return 0;
	}
	s = rows[idx].chars;
	clen = code_len(s, rows[idx].size);
	if (clen == 0) {
		return target_after(rows, prev_nonblank(rows, idx), tab_width);
	}
	/* A pattern with no `case' above it degrades to the neighbour
	 * rule: the misfire (`y=$(foo)' reads as a pattern) then costs
	 * nothing, because that rule is what an ordinary line gets. */
	if (is_pattern(s, clen)) {
		m = scan_match(rows, idx, tab_width, SH_CLOSE_NONE, 1);
		if (m >= 0) {
			return m;
		}
		return target_after(rows, prev_nonblank(rows, idx), tab_width);
	}
	closer = self_closer(s, clen);
	if (closer != SH_CLOSE_NONE) {
		m = scan_match(rows, idx, tab_width, closer, 0);
		if (m >= 0) {
			return m;
		}
		prev = prev_nonblank(rows, idx);
		if (prev < 0) {
			return 0;
		}
		m = indent_width(&rows[prev], tab_width) - SH_INDENT_WIDTH;
		return m > 0 ? m : 0;
	}
	return target_after(rows, prev_nonblank(rows, idx), tab_width);
}

int shindent_newline_indent(
    const erow *rows, int numrows, int filerow, int tab_width)
{
	int j;

	if (!rows || numrows <= 0) {
		return 0;
	}
	if (filerow >= numrows) {
		filerow = numrows - 1;
	}
	/* The line RET split on is the neighbour, whole: back over blank
	 * lines when it has no code itself. */
	for (j = filerow; j >= 0; j--) {
		if (code_len(rows[j].chars, rows[j].size) > 0) {
			return target_after(rows, j, tab_width);
		}
	}
	return 0;
}

int shindent_build_indent(
    char *out, int out_size, int target_col, int tab_width, int use_tabs)
{
	int tabs = 0;
	int spaces;
	int i;

	if (!out || out_size <= 0) {
		return -1;
	}
	if (target_col < 0) {
		target_col = 0;
	}
	if (tab_width < 1) {
		tab_width = 8;
	}
	if (use_tabs) {
		tabs = target_col / tab_width;
	}
	spaces = target_col - tabs * tab_width;
	if (tabs + spaces > out_size) {
		return -1;
	}
	for (i = 0; i < tabs; i++) {
		out[i] = '\t';
	}
	for (i = 0; i < spaces; i++) {
		out[tabs + i] = ' ';
	}
	return tabs + spaces;
}

int shindent_active_for_buffer(const struct editor_buffer *b)
{
	return b && b->syntax && b->syntax->id == KG_MODE_SHELL;
}

int shindent_active(void) { return shindent_active_for_buffer(bcur()); }

/* The newline half: one user edit of `\n' plus the computed indent,
 * point after it -- the shape editor_insert_text_at_point() gives
 * every command that puts a run of text at point. */
void shindent_insert_newline(int filerow)
{
	struct editor_buffer *b = bcur();
	int width = display_tab_width(&b->display);
	int use_tabs = b->indent_tabs_mode_local != LOCAL_BOOL_FALSE;
	int target
	    = shindent_newline_indent(b->row, b->numrows, filerow, width);
	char *text = malloc((size_t)target + 2);
	int n;

	if (!text) {
		editor_set_status_message("Out of memory");
		return;
	}
	n = shindent_build_indent(
	    text + 1, target + 1, target, width, use_tabs);
	if (n < 0) {
		free(text);
		editor_set_status_message("Out of memory");
		return;
	}
	text[0] = '\n';
	editor_insert_text_at_point(text, n + 1);
	free(text);
}

/* TAB half: replace the line's leading whitespace with its target, as
 * one transaction (one C-_ rejoins it), and keep point on the same
 * side of the indent it was on.  An already-correct line is not an
 * edit at all: point moves to the indent when it was inside it. */
void shindent_indent_current_line(void)
{
	struct editor_buffer *b = bcur();
	int filerow = editor_current_filerow_or_eof();
	erow *row;
	int width;
	int target;
	int old_bytes;
	int old_col;
	int use_tabs;
	int filecol;
	char *indent;
	int n;

	if (filerow >= b->numrows) {
		return;
	}
	row = &b->row[filerow];
	width = display_tab_width(&b->display);
	target = shindent_target_for_rows(b->row, b->numrows, filerow, width);
	old_bytes = indent_bytes(row);
	old_col = indent_width(row, width);
	if (old_col == target) {
		if (editor_current_filecol() <= old_bytes && old_bytes > 0) {
			editor_cursor_goto(filerow, old_bytes);
		}
		return;
	}
	use_tabs = b->indent_tabs_mode_local != LOCAL_BOOL_FALSE;
	indent = malloc((size_t)target + 1);
	if (!indent) {
		editor_set_status_message("Out of memory");
		return;
	}
	n = shindent_build_indent(indent, target + 1, target, width, use_tabs);
	if (n < 0) {
		free(indent);
		editor_set_status_message("Out of memory");
		return;
	}
	/* Refused on a read-only buffer: point stays where it was. */
	if (!editor_row_replace_range(
		filerow, 0, old_bytes, indent, n, KG_EDIT_USER)) {
		free(indent);
		return;
	}
	free(indent);
	filecol = editor_current_filecol();
	if (filecol <= old_bytes) {
		editor_cursor_goto(filerow, n);
	} else {
		editor_cursor_goto(filerow, filecol + n - old_bytes);
	}
}
