#include "compile_parse.h"
#include "def.h"
#include <string.h>

/* A parsed location, before it is either turned into a record or rejected
 * for an overflowed number.  Kept out of the header: nothing outside this
 * file needs the pre-validated form. */
struct diag_location {
	size_t path_len;
	size_t line;
	bool line_overflow;
	bool has_column;
	size_t column;
	bool column_overflow;
};

static bool path_is_all_digits(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (!ascii_is_digit((unsigned char)s[i])) {
			return false;
		}
	}
	return true;
}

static size_t find_first_colon(const char *s, size_t len)
{
	size_t i = 0;

	while (i < len && s[i] != ':') {
		i++;
	}
	return i;
}

/* Scan the longest run of ASCII digits starting at `pos`.  Accumulates with
 * the checked helpers so a run too long to fit `size_t` is reported as
 * overflow rather than silently wrapped; scanning still consumes every
 * digit, so `*end` is correct even when `*value` is not.  False means no
 * digit was found at `pos` at all -- a distinct outcome from overflow. */
static bool scan_uint(const char *s, size_t len, size_t pos, size_t *end,
    size_t *value, bool *overflow)
{
	size_t start = pos;
	size_t v = 0;
	bool of = false;

	while (pos < len && ascii_is_digit((unsigned char)s[pos])) {
		size_t d = (size_t)(s[pos] - '0');
		size_t tmp;

		if (!of
		    && (!checked_mul_size_t(&tmp, v, 10)
			|| !checked_add_size_t(&v, tmp, d))) {
			of = true;
		}
		pos++;
	}
	if (pos == start) {
		return false;
	}
	*end = pos;
	*value = v;
	*overflow = of;
	return true;
}

/* Match "<path>:<line>[:<column>]:" anchored at the start of `s`.  `path` is
 * whatever precedes the first colon, rejected only when it is empty or made
 * up entirely of digits -- a bare number there is a timestamp or a ratio far
 * more often than a filename, and excluding it is what keeps "12:34:56: ..."
 * from being read as file "12", line 34.  The optional column is accepted
 * greedily: a second run of digits followed by ':' is a column, otherwise
 * the colon after the line number was the trailing delimiter all along. */
static bool match_location(const char *s, size_t len, struct diag_location *out)
{
	size_t colon = find_first_colon(s, len);
	size_t num1_end, num1_val, num2_end, num2_val;
	bool num1_of, num2_of;

	if (colon == 0 || colon >= len || path_is_all_digits(s, colon)) {
		return false;
	}
	if (!scan_uint(s, len, colon + 1, &num1_end, &num1_val, &num1_of)) {
		return false;
	}
	if (num1_end >= len || s[num1_end] != ':') {
		return false;
	}

	out->path_len = colon;
	out->line = num1_val;
	out->line_overflow = num1_of;
	out->has_column = false;
	out->column = 0;
	out->column_overflow = false;

	if (scan_uint(s, len, num1_end + 1, &num2_end, &num2_val, &num2_of)
	    && num2_end < len && s[num2_end] == ':') {
		out->has_column = true;
		out->column = num2_val;
		out->column_overflow = num2_of;
	}
	return true;
}

static void frame_set(
    struct compile_dir_frame *f, const char *path, size_t path_len)
{
	size_t copy = path_len < KG_COMPILE_DIAG_PATH_MAX
	    ? path_len
	    : KG_COMPILE_DIAG_PATH_MAX;

	memcpy(f->buf, path, copy);
	f->len = copy;
	f->truncated = copy < path_len;
}

/* Push a newly entered directory.  Beyond the tracked-depth cap the tracked
 * directory simply stops changing -- see stack_overflow_entries -- rather
 * than growing without bound. */
static void parser_push_dir(
    struct compile_diag_parser *p, const char *path, size_t path_len)
{
	if (p->depth + 1 >= KG_COMPILE_DIAG_DIR_STACK_MAX) {
		p->stack_overflow_entries++;
		return;
	}
	p->depth++;
	frame_set(&p->stack[p->depth], path, path_len);
}

static void parser_pop_dir(struct compile_diag_parser *p)
{
	if (p->depth > 0) {
		p->depth--;
	} else {
		p->unbalanced_leaving++;
	}
}

/* Consume an optional "[<digits>]" job tag right after "make".  Absent is
 * fine (plain "make: ..."); present but unterminated is not a match. */
static bool skip_make_job_tag(const char *s, size_t len, size_t *pos)
{
	size_t i = *pos;

	if (i >= len || s[i] != '[') {
		return true;
	}
	i++;
	while (i < len && ascii_is_digit((unsigned char)s[i])) {
		i++;
	}
	if (i >= len || s[i] != ']') {
		return false;
	}
	*pos = i + 1;
	return true;
}

/* Match "make[N]: Entering directory " / "make[N]: Leaving directory "
 * (job tag optional).  On success `*pos` is the byte after the trailing
 * space, where the quoted path begins. */
struct dir_line_kw {
	const char *text;
	size_t len;
	bool entering;
};

static const struct dir_line_kw dir_line_kws[] = {
	{ "Entering directory ", sizeof("Entering directory ") - 1, true },
	{ "Leaving directory ", sizeof("Leaving directory ") - 1, false },
};

static bool match_make_prefix(
    const char *s, size_t len, size_t *pos, bool *entering)
{
	size_t i = 4;

	if (len < 5 || memcmp(s, "make", 4) != 0) {
		return false;
	}
	if (!skip_make_job_tag(s, len, &i)) {
		return false;
	}
	if (i >= len || s[i] != ':') {
		return false;
	}
	i++;
	if (i < len && s[i] == ' ') {
		i++;
	}
	for (size_t k = 0; k < sizeof(dir_line_kws) / sizeof(dir_line_kws[0]);
	    k++) {
		const struct dir_line_kw *kw = &dir_line_kws[k];

		if (len - i >= kw->len
		    && memcmp(s + i, kw->text, kw->len) == 0) {
			*entering = kw->entering;
			*pos = i + kw->len;
			return true;
		}
	}
	return false;
}

/* GNU make quotes the directory with '...' (modern) or `...' (older); the
 * closing mark is always a plain apostrophe. */
static bool extract_quoted_path(
    const char *s, size_t len, size_t pos, size_t *path_start, size_t *path_len)
{
	char open;

	if (pos >= len) {
		return false;
	}
	open = s[pos];
	if (open != '\'' && open != '`') {
		return false;
	}
	pos++;
	*path_start = pos;
	while (pos < len && s[pos] != '\'') {
		pos++;
	}
	if (pos >= len) {
		return false;
	}
	*path_len = pos - *path_start;
	return true;
}

/* True when `s` is a make Entering/Leaving directory line at all -- whether
 * or not its quoted path could actually be read -- so the caller never also
 * tries to read it as a diagnostic. */
static bool try_match_directory_line(
    struct compile_diag_parser *p, const char *s, size_t len)
{
	size_t pos, path_start, path_len;
	bool entering;

	if (!match_make_prefix(s, len, &pos, &entering)) {
		return false;
	}
	if (!extract_quoted_path(s, len, pos, &path_start, &path_len)) {
		p->malformed_directory_lines++;
		return true;
	}
	if (entering) {
		parser_push_dir(p, s + path_start, path_len);
	} else {
		parser_pop_dir(p);
	}
	return true;
}

static void copy_current_dir(
    const struct compile_diag_parser *p, struct compile_diag *rec)
{
	const struct compile_dir_frame *f = &p->stack[p->depth];

	memcpy(rec->cwd, f->buf, f->len);
	rec->cwd_len = f->len;
	rec->cwd_truncated = f->truncated;
}

static void record_diagnostic_if_matched(const struct compile_diag_parser *p,
    struct compile_diag_store *store, const char *s, size_t len)
{
	struct diag_location loc;
	struct compile_diag *rec;
	size_t copy_len;

	if (!match_location(s, len, &loc)) {
		return;
	}
	if (loc.line_overflow || (loc.has_column && loc.column_overflow)) {
		store->rejected_overflow++;
		return;
	}
	if (store->count >= KG_COMPILE_DIAG_MAX) {
		store->truncated = true;
		return;
	}

	rec = &store->entries[store->count++];
	copy_len = loc.path_len < KG_COMPILE_DIAG_PATH_MAX
	    ? loc.path_len
	    : KG_COMPILE_DIAG_PATH_MAX;
	memcpy(rec->file, s, copy_len);
	rec->file_len = copy_len;
	rec->file_truncated = copy_len < loc.path_len;
	rec->line = loc.line;
	rec->has_column = loc.has_column;
	rec->column = loc.column;
	copy_current_dir(p, rec);
}

void compile_diag_parser_init(struct compile_diag_parser *parser,
    const char *initial_cwd, size_t initial_cwd_len)
{
	memset(parser, 0, sizeof(*parser));
	frame_set(
	    &parser->stack[0], initial_cwd ? initial_cwd : "", initial_cwd_len);
}

void compile_diag_store_init(struct compile_diag_store *store)
{
	memset(store, 0, sizeof(*store));
}

void compile_diag_parse_line(struct compile_diag_parser *parser,
    struct compile_diag_store *store, const char *line, size_t len)
{
	if (try_match_directory_line(parser, line, len)) {
		return;
	}
	record_diagnostic_if_matched(parser, store, line, len);
}
