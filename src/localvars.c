#include "localvars.h"
#include "def.h"
#include "syntax.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#include <unistd.h>
#endif

void local_settings_init(struct local_settings *settings)
{
	memset(settings, 0, sizeof(*settings));
	settings->buffer_read_only = LOCAL_BOOL_UNSET;
	settings->compile_command[0] = '\0';
	settings->compile_command_set = false;
	settings->tab_width_set = false;
	settings->tab_width = 0;
	settings->indent_tabs_mode = LOCAL_BOOL_UNSET;
	settings->c_basic_offset_set = false;
	settings->c_basic_offset = 0;
}

/* File-local display state merges like the rest: set overwrites, unset
 * leaves the destination alone. */
static void local_settings_merge_display(
    struct local_settings *destination, const struct local_settings *source)
{
	if (source->tab_width_set) {
		destination->tab_width = source->tab_width;
		destination->tab_width_set = true;
	}
	if (source->indent_tabs_mode != LOCAL_BOOL_UNSET) {
		destination->indent_tabs_mode = source->indent_tabs_mode;
	}
	if (source->c_basic_offset_set) {
		destination->c_basic_offset = source->c_basic_offset;
		destination->c_basic_offset_set = true;
	}
}

void local_settings_merge(
    struct local_settings *destination, const struct local_settings *source)
{
	if (source->compile_command_set) {
		memcpy(destination->compile_command, source->compile_command,
		    KG_COMPILE_COMMAND_MAX);
		destination->compile_command_set = true;
		destination->compile_command[KG_COMPILE_COMMAND_MAX - 1] = '\0';
	}
	if (source->buffer_read_only != LOCAL_BOOL_UNSET) {
		destination->buffer_read_only = source->buffer_read_only;
	}
	local_settings_merge_display(destination, source);

	destination->ignored_entries += source->ignored_entries;
	destination->malformed_entries += source->malformed_entries;
}

/* ---- The variables, and what their values mean ----
 *
 * kg understands five file-local variables through three envelope
 * grammars.  The grammars differ in how a name and a value are *found*
 * -- semicolon splitting inside `-*- ... -*-`, a prefixed/suffixed block
 * with backslash continuation, or a non-evaluating sexp reader -- and
 * that scanning stays with each parser.  What a name means, and what a
 * value means once it has been found, is stated only here, so a sixth
 * variable is one row and not three edits. */

enum local_var_kind localvars_kind(const char *name)
{
	static const struct {
		const char *name;
		enum local_var_kind kind;
	} table[] = {
		{ "compile-command", LOCAL_VAR_STRING },
		{ "buffer-read-only", LOCAL_VAR_BOOL },
		{ "tab-width", LOCAL_VAR_TAB_WIDTH },
		{ "c-basic-offset", LOCAL_VAR_C_OFFSET },
		{ "c-ts-mode-indent-offset", LOCAL_VAR_C_OFFSET },
		{ "java-ts-indent-offset", LOCAL_VAR_C_OFFSET },
		{ "indent-tabs-mode", LOCAL_VAR_INDENT_TABS },
		{ NULL, LOCAL_VAR_NONE },
	};
	int i;

	for (i = 0; table[i].name; i++) {
		if (strcmp(table[i].name, name) == 0) {
			return table[i].kind;
		}
	}
	return LOCAL_VAR_NONE;
}

/* Apply a boolean variable's value from the raw token text the envelope
 * found.  Emacs spells these `t` and `nil`, case-insensitively; anything
 * else, including a token too long to be either, is malformed.  Trailing
 * blanks are trimmed here so a parser can hand over the span it has
 * without normalising it first -- a symbol read by the dir-locals reader
 * has none, and the other two envelopes have already trimmed. */
static void apply_bool_token(
    enum local_bool_value *slot, unsigned *malformed, const char *text, int len)
{
	char token[12];
	int i, n;

	while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
		len--;
	}
	n = len < (int)sizeof(token) - 1 ? len : (int)sizeof(token) - 1;
	for (i = 0; i < n; i++) {
		token[i] = (char)tolower((unsigned char)text[i]);
	}
	token[n] = '\0';

	if (strcmp(token, "t") == 0) {
		*slot = LOCAL_BOOL_TRUE;
	} else if (strcmp(token, "nil") == 0) {
		*slot = LOCAL_BOOL_FALSE;
	} else {
		(*malformed)++;
	}
}

void localvars_apply_bool(struct local_settings *out, const char *text, int len)
{
	apply_bool_token(
	    &out->buffer_read_only, &out->malformed_entries, text, len);
}

static void apply_indent_tabs(
    struct local_settings *out, const char *text, int len)
{
	apply_bool_token(
	    &out->indent_tabs_mode, &out->malformed_entries, text, len);
}

/* Defined beside the init.el reader below; the digit grammar both text
 * envelopes share with it. */
static bool is_integer_token(const char *s);

/* An integer variable's value from the same raw token text: trailing
 * blanks trimmed here, the digit grammar shared with the init.el reader,
 * one closed range per variable.  A quoted "2" never reaches this -- the
 * envelopes read a string and an integer differently on purpose, and a
 * string where an integer belongs is malformed, not coerced. */
static void apply_int_token(struct local_settings *out,
    enum local_var_kind kind, const char *text, int len)
{
	char token[64];
	int i, n, val;

	while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
		len--;
	}
	n = len < (int)sizeof(token) - 1 ? len : (int)sizeof(token) - 1;
	for (i = 0; i < n; i++) {
		token[i] = text[i];
	}
	token[n] = '\0';

	if (n != len || !is_integer_token(token)) {
		out->malformed_entries++;
		return;
	}
	val = atoi(token);
	if (kind == LOCAL_VAR_TAB_WIDTH) {
		if (val < 1 || val > KG_TAB_WIDTH_MAX) {
			out->malformed_entries++;
			return;
		}
		out->tab_width = val;
		out->tab_width_set = true;
	} else {
		if (val < KG_C_BASIC_OFFSET_MIN
		    || val > KG_C_BASIC_OFFSET_MAX) {
			out->malformed_entries++;
			return;
		}
		out->c_basic_offset = val;
		out->c_basic_offset_set = true;
	}
}

/* Apply a string variable's value, already decoded by the envelope's own
 * unquoting.  `len` excludes the terminator, which the caller's buffer
 * still carries. */
void localvars_apply_string(
    struct local_settings *out, const char *text, int len)
{
	out->compile_command_set = true;
	memcpy(out->compile_command, text, (size_t)len + 1);
}

static int parse_quoted_string(const char *value, char *out, size_t out_size)
{
	while (*value == ' ' || *value == '\t') {
		value++;
	}
	if (*value != '"') {
		return -1;
	}
	value++;

	size_t n = 0;
	for (;;) {
		if (!*value) {
			return -3;
		}
		if (*value == '\\') {
			value++;
			if (!*value) {
				return -3;
			}
			char c;
			switch (*value) {
			case 'n':
				c = '\n';
				break;
			case 't':
				c = '\t';
				break;
			case 'r':
				c = '\r';
				break;
			case '\\':
				c = '\\';
				break;
			case '"':
				c = '"';
				break;
			default:
				c = *value;
				break;
			}
			if (n >= out_size - 1) {
				return -2;
			}
			out[n++] = c;
			value++;
		} else if (*value == '"') {
			out[n] = '\0';
			return (int)n;
		} else {
			if (n >= out_size - 1) {
				return -2;
			}
			out[n++] = *value;
			value++;
		}
	}
}

/* ---- Span scanning shared by the modeline and footer envelopes ----
 *
 * The two envelopes delimit an assignment differently -- one splits a
 * `-*- ... -*-` property list on semicolons, the other strips a comment
 * prefix and suffix off every line of a block -- but once a span has been
 * delimited, both find the same `name: value` inside it by the same
 * rules.  That span-level scanning lives here so it is stated once. */

/* Trim ASCII blanks from both ends of the span [*s, *s + *len). */
static void lv_trim_blanks(const char **s, int *len)
{
	const char *p = *s;
	int n = *len;

	while (n > 0 && (p[0] == ' ' || p[0] == '\t')) {
		p++;
		n--;
	}
	while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
		n--;
	}
	*s = p;
	*len = n;
}

/* Offset of the first `:` that separates a name from a value, or -1.  A
 * colon inside a double-quoted string belongs to the value; a backslash
 * escapes the byte after it, in or out of quotes. */
static int lv_find_assign_colon(const char *s, int len)
{
	bool in_quote = false;
	bool escaped = false;

	for (int i = 0; i < len; i++) {
		if (escaped) {
			escaped = false;
		} else if (s[i] == '\\') {
			escaped = true;
		} else if (s[i] == '"') {
			in_quote = !in_quote;
		} else if (!in_quote && s[i] == ':') {
			return i;
		}
	}
	return -1;
}

/* What one envelope span turned out to hold. */
enum lv_assign {
	LV_ASSIGN_BLANK, /* nothing but blanks: not an entry, not an error */
	LV_ASSIGN_MALFORMED, /* no separating colon, or an empty name */
	LV_ASSIGN_OK
};

/* Split one envelope span into a lowercased variable name and the value
 * text that follows the colon.  The span is trimmed first, so the value
 * handed back has no leading blanks and ends where the envelope's own
 * delimiter put it.  A name longer than `name_size` is truncated, which
 * can only turn a match into a miss. */
static enum lv_assign lv_split_assignment(const char *body, int body_len,
    char *name, size_t name_size, const char **value, int *value_len)
{
	int colon, name_len, n;

	lv_trim_blanks(&body, &body_len);
	if (body_len <= 0) {
		return LV_ASSIGN_BLANK;
	}

	colon = lv_find_assign_colon(body, body_len);
	if (colon < 0) {
		return LV_ASSIGN_MALFORMED;
	}

	name_len = colon;
	while (name_len > 0
	    && (body[name_len - 1] == ' ' || body[name_len - 1] == '\t')) {
		name_len--;
	}
	if (name_len <= 0) {
		return LV_ASSIGN_MALFORMED;
	}

	n = name_len < (int)name_size - 1 ? name_len : (int)name_size - 1;
	for (int i = 0; i < n; i++) {
		name[i] = (char)tolower((unsigned char)body[i]);
	}
	name[n] = '\0';

	*value = body + colon + 1;
	*value_len = body_len - colon - 1;
	while (*value_len > 0 && (**value == ' ' || **value == '\t')) {
		(*value)++;
		(*value_len)--;
	}
	return LV_ASSIGN_OK;
}

/* First occurrence of `needle` in [hay, hay + hay_len), or NULL. */
static const char *lv_find_bytes(
    const char *hay, int hay_len, const char *needle, int needle_len)
{
	for (int i = 0; i <= hay_len - needle_len; i++) {
		if (memcmp(hay + i, needle, (size_t)needle_len) == 0) {
			return hay + i;
		}
	}
	return NULL;
}

/* ---- The `-*- ... -*-` modeline ---- */

/* The `;` that ends the leading segment of [seg, end), or NULL when the
 * last segment runs to the close marker.  A semicolon inside a quoted
 * string or inside parentheses belongs to a value, not to the envelope. */
static const char *modeline_segment_end(const char *seg, const char *end)
{
	bool in_quote = false;
	bool escaped = false;
	int depth = 0;

	for (const char *p = seg; p < end; p++) {
		if (escaped) {
			escaped = false;
		} else if (*p == '\\') {
			escaped = true;
		} else if (*p == '"') {
			in_quote = !in_quote;
		} else if (in_quote) {
			continue;
		} else if (*p == '(') {
			depth++;
		} else if (*p == ')' && depth > 0) {
			depth--;
		} else if (*p == ';' && depth == 0) {
			return p;
		}
	}
	return NULL;
}

/* Apply one modeline property.  A string value is read straight out of
 * the row, whose terminator bounds it -- the modeline has no continuation
 * rule, so an unclosed quote simply fails to parse. */
static void modeline_apply_assignment(struct local_settings *out,
    const char *name, const char *value, int value_len)
{
	switch (localvars_kind(name)) {
	case LOCAL_VAR_STRING: {
		char decoded[KG_COMPILE_COMMAND_MAX];
		int rc = parse_quoted_string(value, decoded, sizeof(decoded));

		if (rc < 0) {
			out->malformed_entries++;
		} else {
			localvars_apply_string(out, decoded, rc);
		}
		break;
	}
	case LOCAL_VAR_BOOL:
		localvars_apply_bool(out, value, value_len);
		break;
	case LOCAL_VAR_TAB_WIDTH:
	case LOCAL_VAR_C_OFFSET:
		apply_int_token(out, localvars_kind(name), value, value_len);
		break;
	case LOCAL_VAR_INDENT_TABS:
		apply_indent_tabs(out, value, value_len);
		break;
	default:
		out->ignored_entries++;
		break;
	}
}

int localvars_parse_modeline(
    const erow *rows, int row_count, struct local_settings *out)
{
	int cand;
	const char *line, *start_marker;
	const char *search_from, *end_marker;
	const char *seg, *props_end;
	int len, remaining;

	local_settings_init(out);

	if (row_count < 1 || rows == NULL) {
		return -1;
	}

	cand = 0;
	if (row_count >= 2 && rows[0].size >= 2
	    && memcmp(rows[0].chars, "#!", 2) == 0) {
		cand = 1;
	}

	line = rows[cand].chars;
	len = rows[cand].size;

	start_marker = lv_find_bytes(line, len, "-*-", 3);
	if (!start_marker) {
		return -1;
	}

	search_from = start_marker + 3;
	remaining = len - (int)(search_from - line);
	end_marker = lv_find_bytes(search_from, remaining, "-*-", 3);
	if (!end_marker) {
		return -1;
	}

	seg = search_from;
	props_end = end_marker;

	while (seg < props_end) {
		const char *sep = modeline_segment_end(seg, props_end);
		const char *value;
		int value_len;
		char name[64];

		switch (lv_split_assignment(seg,
		    (int)((sep ? sep : props_end) - seg), name, sizeof(name),
		    &value, &value_len)) {
		case LV_ASSIGN_OK:
			modeline_apply_assignment(out, name, value, value_len);
			break;
		case LV_ASSIGN_MALFORMED:
			out->malformed_entries++;
			break;
		case LV_ASSIGN_BLANK:
			break;
		}

		seg = sep ? sep + 1 : props_end;
	}

	return 0;
}

enum {
	FOOTER_TAIL_BYTES = 3000,
	FOOTER_WIN_EXTRA = 32,
	FOOTER_MAX_LINES = 512
};

static int footer_body_ends_unescaped_bslash(const char *body, int len)
{
	int n;

	if (len <= 0 || body[len - 1] != '\\') {
		return 0;
	}
	n = 0;
	for (int i = len - 1; i >= 0 && body[i] == '\\'; i--) {
		n++;
	}
	return (n & 1) != 0;
}

/* ---- .dir-locals.el directory search ---- */

int dirlocals_find(
    const char *visited_filename, char *result, size_t result_size)
{
	char dir[PATH_MAX];
	char resolved[PATH_MAX];
	const char *last_slash;
	char *rpath;
	size_t dlen;

	if (!visited_filename || !visited_filename[0] || !result
	    || result_size < 2) {
		return -1;
	}

	rpath = realpath(visited_filename, resolved);
	if (rpath) {
		dlen = strlen(resolved);
		if (dlen >= sizeof(dir)) {
			return -1;
		}
		memcpy(dir, resolved, dlen + 1);
		/* The search starts at the resolved path when it is itself a
		 * directory, and at its parent otherwise.  realpath() always
		 * returns an absolute path, so the working-directory fallback
		 * is unreachable here; it mirrors the branch below. */
		if (!path_is_dir(dir) && path_parent_dir(dir) != 0
		    && !getcwd(dir, sizeof(dir))) {
			return -1;
		}
	} else {
		last_slash = strrchr(visited_filename, '/');
		if (last_slash) {
			if (last_slash == visited_filename) {
				dir[0] = '/';
				dir[1] = '\0';
			} else {
				char canon[PATH_MAX];
				char cwd[PATH_MAX];

				dlen = (size_t)(last_slash - visited_filename);
				if (dlen >= sizeof(dir)) {
					return -1;
				}
				memcpy(dir, visited_filename, dlen);
				dir[dlen] = '\0';

				if (dir[0] != '/') {
					size_t cwdlen;

					if (!getcwd(cwd, sizeof(cwd))) {
						return -1;
					}
					cwdlen = strlen(cwd);
					if (cwdlen + 1 + dlen >= PATH_MAX) {
						return -1;
					}
					memmove(
					    dir + cwdlen + 1, dir, dlen + 1);
					memcpy(dir, cwd, cwdlen);
					dir[cwdlen] = '/';
				}

				if (realpath(dir, canon)) {
					dlen = strlen(canon);
					if (dlen >= sizeof(dir)) {
						return -1;
					}
					memcpy(dir, canon, dlen + 1);
				} else {
					return -1;
				}
			}
		} else {
			if (!getcwd(dir, sizeof(dir))) {
				return -1;
			}
		}
	}

	for (;;) {
		char path[PATH_MAX + 16];

		dlen = strlen(dir);
		if (dlen + sizeof("/.dir-locals.el") > sizeof(path)) {
			return -1;
		}
		snprintf(path, sizeof(path), "%s/.dir-locals.el", dir);

		if (access(path, R_OK) == 0) {
			size_t plen = strlen(path);

			if (plen >= result_size) {
				return -1;
			}
			memcpy(result, path, plen + 1);
			return 0;
		}

		if (strcmp(dir, "/") == 0) {
			break;
		}

		{
			char parent[PATH_MAX + 16];

			if (dlen + 4 > sizeof(parent)) {
				return -1;
			}
			snprintf(parent, sizeof(parent), "%s/..", dir);
			if (!realpath(parent, dir)) {
				return -1;
			}
		}
	}

	return -1;
}

/* ---- safe .dir-locals.el S-expression parser ---- */

enum {
	DL_MAX_NESTING = 64,
	DL_MAX_TOKENS = 4096,
};

struct dlr {
	const char *src;
	size_t len;
	size_t pos;
	int depth;
	size_t tokcount;
};

static int dlr_is_delim(char c)
{
	/* strchr() answers "yes" for '\0', because every string ends in
	 * one.  Every atom scan below is a "walk while not a delimiter"
	 * loop, so a NUL inside a sexp stopped the walk without consuming
	 * anything and dirlocals_parse() spun on it forever -- a
	 * .dir-locals.el with one NUL byte in it hung the editor.  A NUL
	 * is an ordinary atom byte here. */
	return c != '\0' && strchr("() \t\n\r;'\"", c) != NULL;
}

static void dlr_skip_ws(struct dlr *r)
{
	for (;;) {
		while (r->pos < r->len) {
			char c = r->src[r->pos];

			if (c && strchr(" \t\n\r\f", c)) {
				r->pos++;
				continue;
			}
			break;
		}
		if (r->pos < r->len && r->src[r->pos] == ';') {
			while (r->pos < r->len && r->src[r->pos] != '\n') {
				r->pos++;
			}
			continue;
		}
		break;
	}
}

static int dlr_read_sym(struct dlr *r, char *buf, size_t bufsz)
{
	size_t start;

	dlr_skip_ws(r);
	if (r->pos >= r->len) {
		return -1;
	}
	start = r->pos;
	while (r->pos < r->len && !dlr_is_delim(r->src[r->pos])) {
		r->pos++;
	}
	{
		size_t slen = r->pos - start;

		if (slen == 0 || slen >= bufsz) {
			return -1;
		}
		memcpy(buf, r->src + start, slen);
		buf[slen] = '\0';
		return (int)slen;
	}
}

static int dlr_read_str(struct dlr *r, char *buf, size_t bufsz)
{
	dlr_skip_ws(r);
	if (r->pos >= r->len || r->src[r->pos] != '"') {
		return -1;
	}
	r->pos++;

	{
		size_t n = 0;

		while (r->pos < r->len) {
			char c = r->src[r->pos];

			if (c == '"') {
				r->pos++;
				buf[n] = '\0';
				return (int)n;
			}
			if (c == '\\') {
				char decoded, ec;

				r->pos++;
				if (r->pos >= r->len) {
					return -3;
				}
				ec = r->src[r->pos];
				switch (ec) {
				case 'n':
					decoded = '\n';
					break;
				case 't':
					decoded = '\t';
					break;
				case 'r':
					decoded = '\r';
					break;
				case '\\':
					decoded = '\\';
					break;
				case '"':
					decoded = '"';
					break;
				default:
					decoded = ec;
					break;
				}
				if (n >= bufsz - 1) {
					return -2;
				}
				buf[n++] = decoded;
				r->pos++;
			} else {
				if (n >= bufsz - 1) {
					return -2;
				}
				buf[n++] = c;
				r->pos++;
			}
		}
		return -3;
	}
}

static int dlr_skip_sexp(struct dlr *r)
{
	dlr_skip_ws(r);
	if (r->pos >= r->len) {
		return -1;
	}
	r->tokcount++;
	if (r->tokcount > DL_MAX_TOKENS) {
		return -1;
	}

	{
		char c = r->src[r->pos];

		if (c == '(') {
			int pdepth = 1;

			r->pos++;
			r->depth++;
			if (r->depth > DL_MAX_NESTING) {
				return -1;
			}
			while (pdepth > 0 && r->pos < r->len) {
				dlr_skip_ws(r);
				if (r->pos >= r->len) {
					break;
				}
				c = r->src[r->pos];
				if (c == '(') {
					r->depth++;
					if (r->depth > DL_MAX_NESTING) {
						return -1;
					}
					pdepth++;
					r->pos++;
					r->tokcount++;
					if (r->tokcount > DL_MAX_TOKENS) {
						return -1;
					}
				} else if (c == ')') {
					r->depth--;
					pdepth--;
					r->pos++;
				} else if (c == '"') {
					r->pos++;
					while (r->pos < r->len) {
						if (r->src[r->pos] == '\\') {
							r->pos++;
							if (r->pos < r->len) {
								r->pos++;
							}
							continue;
						}
						if (r->src[r->pos] == '"') {
							r->pos++;
							break;
						}
						r->pos++;
					}
				} else if (c == '\'') {
					r->pos++;
					r->tokcount++;
				} else {
					while (r->pos < r->len
					    && !dlr_is_delim(r->src[r->pos])) {
						r->pos++;
					}
					r->tokcount++;
				}
			}
			return (pdepth == 0) ? 0 : -1;
		}
		if (c == ')') {
			return 0;
		}
		if (c == '"') {
			r->pos++;
			while (r->pos < r->len) {
				if (r->src[r->pos] == '\\') {
					r->pos++;
					if (r->pos < r->len) {
						r->pos++;
					}
					continue;
				}
				if (r->src[r->pos] == '"') {
					r->pos++;
					return 0;
				}
				r->pos++;
			}
			return -1;
		}
		if (c == '\'') {
			r->pos++;
			return dlr_skip_sexp(r);
		}
		while (r->pos < r->len && !dlr_is_delim(r->src[r->pos])) {
			r->pos++;
		}
	}
	return 0;
}

/* Read one bare-atom value for a bool/int variable.  Answers 0 with the
 * atom in `buf' (`*vlen` bytes, unterminated span length); 1 when what
 * sat there was no atom at all -- a string, a list or a stray paren is
 * consumed and malformed, and an overlong symbol is left for the pair
 * loop's skip to consume on its next pass; -1 when the reader gave up. */
static int dlr_read_atom_value(
    struct dlr *r, char *buf, size_t bufsz, int *vlen)
{
	int slen;

	if (r->src[r->pos] == '"' || r->src[r->pos] == '('
	    || r->src[r->pos] == ')') {
		if (dlr_skip_sexp(r) != 0) {
			return -1;
		}
		return 1;
	}
	slen = dlr_read_sym(r, buf, bufsz);
	if (slen < 0) {
		return 1;
	}
	*vlen = slen;
	return 0;
}

/* One (VAR . ATOM) pair for a bool/int variable, whole: the atom read,
 * then dispatched to its slot.  A string, a list or a stray paren where
 * the atom belongs is one malformed entry. */
static int dlr_apply_atom(
    struct dlr *r, struct local_settings *out, enum local_var_kind kind)
{
	char symval[64];
	int vlen = 0;
	int arc = dlr_read_atom_value(r, symval, sizeof(symval), &vlen);

	if (arc < 0) {
		return -1;
	}
	if (arc > 0) {
		out->malformed_entries++;
		return 0;
	}
	if (kind == LOCAL_VAR_BOOL) {
		localvars_apply_bool(out, symval, vlen);
	} else if (kind == LOCAL_VAR_INDENT_TABS) {
		apply_indent_tabs(out, symval, vlen);
	} else {
		apply_int_token(out, kind, symval, vlen);
	}
	return 0;
}

static int dlr_apply_pair(struct dlr *r, struct local_settings *out)
{
	char varname[128];
	enum local_var_kind kind;
	int vlen;

	dlr_skip_ws(r);
	if (r->pos >= r->len || r->src[r->pos] != '(') {
		if (dlr_skip_sexp(r) != 0) {
			return -1;
		}
		out->malformed_entries++;
		return 0;
	}
	r->pos++;
	r->depth++;
	r->tokcount++;
	if (r->depth > DL_MAX_NESTING || r->tokcount > DL_MAX_TOKENS) {
		return -1;
	}

	vlen = dlr_read_sym(r, varname, sizeof(varname));
	if (vlen < 0) {
		dlr_skip_ws(r);
		if (r->pos < r->len && r->src[r->pos] == ')') {
			r->pos++;
			r->depth--;
		}
		out->malformed_entries++;
		return 0;
	}
	r->tokcount++;
	if (r->tokcount > DL_MAX_TOKENS) {
		return -1;
	}

	dlr_skip_ws(r);
	if (r->pos >= r->len || r->src[r->pos] != '.') {
		dlr_skip_ws(r);
		if (r->pos < r->len && r->src[r->pos] == ')') {
			r->pos++;
			r->depth--;
		}
		out->malformed_entries++;
		return 0;
	}
	r->pos++;
	r->tokcount++;
	if (r->tokcount > DL_MAX_TOKENS) {
		return -1;
	}

	dlr_skip_ws(r);
	if (r->pos >= r->len) {
		out->malformed_entries++;
		if (r->depth > 0) {
			r->depth--;
		}
		return 0;
	}

	kind = localvars_kind(varname);
	if (kind == LOCAL_VAR_STRING) {
		if (r->src[r->pos] == '"') {
			char buf[KG_COMPILE_COMMAND_MAX];
			int slen = dlr_read_str(r, buf, sizeof(buf));

			if (slen < 0 || slen >= KG_COMPILE_COMMAND_MAX) {
				if (slen == -2) {
					while (r->pos < r->len
					    && r->src[r->pos] != '"') {
						r->pos++;
					}
					if (r->pos < r->len) {
						r->pos++;
					}
				}
				out->malformed_entries++;
			} else {
				localvars_apply_string(out, buf, slen);
			}
		} else {
			if (dlr_skip_sexp(r) != 0) {
				return -1;
			}
			out->malformed_entries++;
		}
	} else if (kind == LOCAL_VAR_BOOL || kind == LOCAL_VAR_INDENT_TABS
	    || kind == LOCAL_VAR_TAB_WIDTH || kind == LOCAL_VAR_C_OFFSET) {
		if (dlr_apply_atom(r, out, kind) != 0) {
			return -1;
		}
	} else {
		/* "eval" and every other variable: consumed but not
		 * applied. */
		if (dlr_skip_sexp(r) != 0) {
			return -1;
		}
		out->ignored_entries++;
	}

	dlr_skip_ws(r);
	if (r->pos < r->len && r->src[r->pos] == ')') {
		r->pos++;
		r->depth--;
	}
	return 0;
}

/* ---- mode-scoped .dir-locals.el ----
 *
 * A file holds one entry per selector: ((nil . ALIST) (c-mode . ALIST)
 * ...).  `nil' applies to every visit; a mode entry only to a buffer in
 * that mode, merged after every `nil' one -- the precedence Emacs was
 * measured to give (mode wins however the file lists them; the nearest
 * file wins because only it is read at all).  Anything else -- `eval', a
 * "subdir/" string key, a mode no buffer answers to -- is consumed and
 * ignored entry by entry, so one unknown form cannot sink the settings
 * the file also carries. */

static const char *const dl_c_selectors[] = {
	"c-mode",
	"c-ts-mode",
	"c++-mode",
	"c++-ts-mode",
	NULL,
};

static const char *const dl_java_selectors[] = {
	"java-mode",
	"java-ts-mode",
	NULL,
};

/* Which settings list a selector feeds: 0 skips the entry, 1 is `nil',
 * 2 is the visited buffer's own mode.  kg has one C syntax row for .c,
 * .h and .cpp alike, so every C-family selector answers to it; the -ts-
 * spellings answer to the same rows because they name the same files. */
static int dl_selector_class(const char *selector, const char *mode_key)
{
	const char *const *tab;
	int i;

	if (strcmp(selector, "nil") == 0) {
		return 1;
	}
	if (!mode_key) {
		return 0;
	}
	if (strcmp(mode_key, "c-mode") == 0) {
		tab = dl_c_selectors;
	} else if (strcmp(mode_key, "java-mode") == 0) {
		tab = dl_java_selectors;
	} else {
		return 0;
	}
	for (i = 0; tab[i]; i++) {
		if (strcmp(selector, tab[i]) == 0) {
			return 2;
		}
	}
	return 0;
}

const char *dirlocals_mode_key(const struct editor_syntax *syntax)
{
	int id;

	if (!syntax) {
		return NULL;
	}
	id = syntax->id;
	if (id == KG_MODE_C) {
		return "c-mode";
	}
	if (id == KG_MODE_JAVA) {
		return "java-mode";
	}
	return NULL;
}

/* The ((VAR . VAL) ...) list one entry carries, each pair applied to
 * `target' in order so a duplicate's last spelling wins. */
static int dlr_parse_pair_list(struct dlr *r, struct local_settings *target)
{
	for (;;) {
		dlr_skip_ws(r);
		if (r->pos >= r->len) {
			return -1;
		}
		if (r->src[r->pos] == ')') {
			r->pos++;
			r->depth--;
			return 0;
		}
		if (dlr_apply_pair(r, target) != 0) {
			return -1;
		}
	}
}

/* One entry's variable list by class: `nil' and the buffer's mode parse
 * into their own settings; anything else is one skipped sexp.  A list
 * where a list belongs is the only shape parsed; anything else is one
 * malformed entry, consumed so the file after it still parses. */
static int dlr_parse_class_list(struct dlr *r, int entry_class,
    struct local_settings *nil_set, struct local_settings *mode_set)
{
	struct local_settings *target;

	if (entry_class == 0) {
		return dlr_skip_sexp(r);
	}
	target = entry_class == 1 ? nil_set : mode_set;
	dlr_skip_ws(r);
	if (r->pos >= r->len || r->src[r->pos] != '(') {
		target->malformed_entries++;
		return dlr_skip_sexp(r);
	}
	r->pos++;
	r->depth++;
	r->tokcount++;
	if (r->depth > DL_MAX_NESTING || r->tokcount > DL_MAX_TOKENS) {
		return -1;
	}
	return dlr_parse_pair_list(r, target);
}

/* One top-level entry, `(SELECTOR . REST)', whole: the selector decides
 * whose settings REST feeds, and the entry always ends past its own
 * closing paren.  A selector that is no symbol at all -- the "subdir/"
 * string key -- skips the same way an unknown mode does. */
static int dlr_parse_entry(struct dlr *r, const char *mode_key,
    struct local_settings *nil_set, struct local_settings *mode_set)
{
	char selector[128];
	int symbol = 0;
	int entry_class = 0;

	r->pos++;
	r->depth++;
	r->tokcount++;
	if (r->depth > DL_MAX_NESTING || r->tokcount > DL_MAX_TOKENS) {
		return -1;
	}

	dlr_skip_ws(r);
	if (r->pos < r->len && !dlr_is_delim(r->src[r->pos])) {
		if (dlr_read_sym(r, selector, sizeof(selector)) < 0) {
			return -1;
		}
		/* No token-budget check of its own: the dot gate below
		 * (or the skip's own accounting on the other branch)
		 * enforces it within one entry of here. */
		r->tokcount++;
		symbol = 1;
	} else {
		if (dlr_skip_sexp(r) != 0) {
			return -1;
		}
	}

	dlr_skip_ws(r);
	if (r->pos >= r->len || r->src[r->pos] != '.') {
		if (dlr_skip_sexp(r) != 0) {
			return -1;
		}
	} else {
		r->pos++;
		r->tokcount++;
		if (r->tokcount > DL_MAX_TOKENS) {
			return -1;
		}
		if (symbol) {
			entry_class = dl_selector_class(selector, mode_key);
		}
		if (dlr_parse_class_list(r, entry_class, nil_set, mode_set)
		    != 0) {
			return -1;
		}
	}

	dlr_skip_ws(r);
	if (r->pos < r->len && r->src[r->pos] == ')') {
		r->pos++;
		r->depth--;
		return 0;
	}
	return -1;
}

/* The file's entries into the two settings they feed.  Answers 1 for an
 * empty file (no entries, still success), 0 with both settings filled,
 * -1 with both untouched. */
static int dirlocals_parse_inner(struct dlr *r, const char *mode_key,
    struct local_settings *nil_set, struct local_settings *mode_set)
{
	dlr_skip_ws(r);
	if (r->pos >= r->len) {
		return 1;
	}
	if (r->src[r->pos] == '\'') {
		r->pos++;
		r->tokcount++;
		dlr_skip_ws(r);
	}
	if (r->pos >= r->len || r->src[r->pos] != '(') {
		return -1;
	}
	r->pos++;
	r->depth++;
	r->tokcount++;
	if (r->depth > DL_MAX_NESTING || r->tokcount > DL_MAX_TOKENS) {
		return -1;
	}
	for (;;) {
		dlr_skip_ws(r);
		if (r->pos >= r->len) {
			return -1;
		}
		if (r->src[r->pos] == ')') {
			r->pos++;
			r->depth--;
			return 0;
		}
		if (r->src[r->pos] != '(') {
			return -1;
		}
		if (dlr_parse_entry(r, mode_key, nil_set, mode_set) != 0) {
			return -1;
		}
	}
}

int dirlocals_parse_for_mode(const char *source, size_t source_len,
    struct local_settings *out, const char *mode_key)
{
	struct dlr r;
	struct local_settings nil_set, mode_set;

	local_settings_init(out);
	local_settings_init(&nil_set);
	local_settings_init(&mode_set);

	if (source_len > DL_MAX_FILESIZE) {
		return -1;
	}
	r.src = source;
	r.len = source_len;
	r.pos = 0;
	r.depth = 0;
	r.tokcount = 0;

	if (dirlocals_parse_inner(&r, mode_key, &nil_set, &mode_set) < 0) {
		return -1;
	}
	dlr_skip_ws(&r);
	local_settings_merge(out, &nil_set);
	local_settings_merge(out, &mode_set);
	return 0;
}

int dirlocals_parse(
    const char *source, size_t source_len, struct local_settings *out)
{
	return dirlocals_parse_for_mode(source, source_len, out, NULL);
}

/* ---- safe init.el S-expression parser (WITH_LISP=0 config loader) ---- */

void init_settings_init(struct init_settings *settings)
{
	if (!settings) {
		return;
	}
	memset(settings, 0, sizeof(*settings));
	settings->tab_width = KG_TAB_WIDTH;
}

static bool is_integer_token(const char *s)
{
	if (!s || !*s) {
		return false;
	}
	if (*s == '+' || *s == '-') {
		s++;
	}
	if (!*s) {
		return false;
	}
	while (*s) {
		if (!ascii_is_digit((unsigned char)*s)) {
			return false;
		}
		s++;
	}
	return true;
}

static void init_copy_setting(
    char *dst, size_t dst_size, bool *flag, const char *src, int slen)
{
	if (slen > 0 && slen < (int)dst_size) {
		memcpy(dst, src, (size_t)slen + 1);
		*flag = true;
	}
}

static void init_apply_setq_string(
    const char *varname, const char *sval, int svlen, struct init_settings *out)
{
	/* The string-valued init settings kg reads without an
	 * evaluator: overlong values are refused, never stored truncated. */
	if (strcmp(varname, "spell-language") == 0) {
		init_copy_setting(out->spell_language,
		    sizeof(out->spell_language), &out->spell_language_set, sval,
		    svlen);
	} else if (strcmp(varname, "spell-highlight-style") == 0) {
		init_copy_setting(out->spell_highlight_style,
		    sizeof(out->spell_highlight_style),
		    &out->spell_highlight_style_set, sval, svlen);
	}
}

static void init_apply_setq_atom(
    const char *varname, const char *atom, int alen, struct init_settings *out)
{
	if (strcmp(varname, "tab-width") == 0) {
		if (is_integer_token(atom)) {
			int val = atoi(atom);
			if (val >= 1 && val <= KG_TAB_WIDTH_MAX) {
				out->tab_width = val;
				out->tab_width_set = true;
			}
		}
	} else if (strcmp(varname, "inhibit-startup-screen") == 0) {
		if (strcasecmp(atom, "t") == 0) {
			out->inhibit_startup_screen = true;
			out->inhibit_startup_screen_set = true;
		} else if (strcasecmp(atom, "nil") == 0) {
			out->inhibit_startup_screen = false;
			out->inhibit_startup_screen_set = true;
		}
	} else if (strcmp(varname, "inhibit-startup-message") == 0) {
		if (strcasecmp(atom, "t") == 0) {
			out->inhibit_startup_message = true;
			out->inhibit_startup_message_set = true;
		} else if (strcasecmp(atom, "nil") == 0) {
			out->inhibit_startup_message = false;
			out->inhibit_startup_message_set = true;
		}
	} else if (strcmp(varname, "spell-highlight-style") == 0) {
		init_copy_setting(out->spell_highlight_style,
		    sizeof(out->spell_highlight_style),
		    &out->spell_highlight_style_set, atom, alen);
	}
}

static void init_apply_setq(struct dlr *r, struct init_settings *out)
{
	char varname[128];
	int vlen;

	dlr_skip_ws(r);
	vlen = dlr_read_sym(r, varname, sizeof(varname));
	if (vlen <= 0) {
		return;
	}
	r->tokcount++;
	dlr_skip_ws(r);
	if (r->pos >= r->len) {
		return;
	}
	if (r->src[r->pos] == '"') {
		char sval[1024];
		int svlen = dlr_read_str(r, sval, sizeof(sval));
		if (svlen >= 0) {
			r->tokcount++;
			init_apply_setq_string(varname, sval, svlen, out);
		}
	} else if (r->src[r->pos] == '(') {
		(void)dlr_skip_sexp(r);
	} else {
		if (r->src[r->pos] == '\'') {
			r->pos++;
			r->tokcount++;
			dlr_skip_ws(r);
		}
		char atom[128];
		int alen = dlr_read_sym(r, atom, sizeof(atom));
		if (alen <= 0) {
			return;
		}
		r->tokcount++;
		init_apply_setq_atom(varname, atom, alen, out);
	}
}

int init_config_parse(
    const char *source, size_t source_len, struct init_settings *out)
{
	struct dlr r;

	if (!out) {
		return -1;
	}
	init_settings_init(out);
	if (!source || source_len > DL_MAX_FILESIZE) {
		return -1;
	}

	r.src = source;
	r.len = source_len;
	r.pos = 0;
	r.depth = 0;
	r.tokcount = 0;

	for (;;) {
		char sym[128];
		int slen;

		dlr_skip_ws(&r);
		if (r.pos >= r.len) {
			break;
		}
		if (r.src[r.pos] != '(') {
			if (dlr_skip_sexp(&r) != 0) {
				return -1;
			}
			continue;
		}
		r.pos++;
		r.depth++;
		r.tokcount++;
		if (r.depth > DL_MAX_NESTING || r.tokcount > DL_MAX_TOKENS) {
			return -1;
		}

		slen = dlr_read_sym(&r, sym, sizeof(sym));
		if (slen > 0) {
			r.tokcount++;
			if (strcmp(sym, "setq") == 0) {
				init_apply_setq(&r, out);
			}
		}

		while (r.depth > 0 && r.pos < r.len) {
			dlr_skip_ws(&r);
			if (r.pos >= r.len) {
				return -1;
			}
			if (r.src[r.pos] == ')') {
				r.pos++;
				r.depth--;
				break;
			}
			if (dlr_skip_sexp(&r) != 0) {
				return -1;
			}
		}
	}
	return 0;
}

/* The `Local Variables:` block's shape: the comment prefix and suffix
 * that its opening line established -- whatever sat before and after the
 * marker there -- and the lines that opened and closed it. */
struct footer_block {
	const char *prefix;
	int prefix_len;
	const char *suffix;
	int suffix_len;
	int start_line;
	int end_line;
};

/* True when `ln` carries the block's comment prefix and suffix; the body
 * between them is handed back UNTRIMMED, because the continuation reader
 * needs the trailing backslash and the leading blanks a trim would eat.
 *
 * A line can satisfy both tests and still have no body, when the two
 * overlap -- prefix "aaa", suffix "aaa" and the line "aaaa" match at once
 * -- which is a line outside the envelope, not a body of length -2. */
static bool footer_line_body(const char *ln, int ll,
    const struct footer_block *b, const char **body, int *body_len)
{
	int n;

	if (ll < b->prefix_len) {
		return false;
	}
	if (b->prefix_len > 0
	    && memcmp(ln, b->prefix, (size_t)b->prefix_len) != 0) {
		return false;
	}
	if (b->suffix_len > 0
	    && (ll < b->suffix_len
		|| memcmp(ln + ll - b->suffix_len, b->suffix,
		       (size_t)b->suffix_len)
		    != 0)) {
		return false;
	}

	n = ll - b->prefix_len - b->suffix_len;
	if (n < 0) {
		return false;
	}
	*body = ln + b->prefix_len;
	*body_len = n;
	return true;
}

/* Copy the buffer's last FOOTER_TAIL_BYTES into `window`, rows rejoined
 * with newlines and the result NUL-terminated.  Emacs looks for the
 * footer near the end of the file, so what precedes that window is not a
 * candidate.  Returns the length written. */
static int footer_build_window(
    const erow *rows, int row_count, char *window, size_t window_size)
{
	const int cap = (int)window_size - 1;
	int total = 0, sofar = 0, wpos = 0, cut;

	for (int i = 0; i < row_count; i++) {
		total += rows[i].size + (i < row_count - 1 ? 1 : 0);
	}
	cut = total > FOOTER_TAIL_BYTES ? total - FOOTER_TAIL_BYTES : 0;

	for (int i = 0; i < row_count && wpos < cap; i++) {
		int row_start = sofar;
		int row_end = sofar + rows[i].size;

		if (row_end > cut) {
			int skip = row_start < cut ? cut - row_start : 0;
			int copy = row_end - (row_start + skip);

			if (copy > cap - wpos) {
				copy = cap - wpos;
			}
			memcpy(
			    window + wpos, rows[i].chars + skip, (size_t)copy);
			wpos += copy;
		}
		sofar = row_end;

		if (i < row_count - 1 && sofar >= cut && wpos < cap) {
			window[wpos++] = '\n';
		}
		sofar += (i < row_count - 1) ? 1 : 0;
	}
	window[wpos] = '\0';
	return wpos;
}

/* Split the window's last form-feed-delimited page into lines.  A block
 * before the last `\f` belongs to an earlier page and is not the file's
 * footer, so the split starts after it.  Returns the line count. */
static int footer_split_lines(
    const char *window, int wpos, const char **lines, int *line_lens)
{
	const char *end = window + wpos;
	const char *p = window;
	const char *ls;
	int nlines = 0;

	for (int j = wpos - 1; j >= 0; j--) {
		if (window[j] == '\f') {
			p = window + j + 1;
			break;
		}
	}

	ls = p;
	while (p < end && nlines < FOOTER_MAX_LINES) {
		if (*p == '\n') {
			lines[nlines] = ls;
			line_lens[nlines] = (int)(p - ls);
			nlines++;
			ls = p + 1;
		}
		p++;
	}
	if (ls < end && nlines < FOOTER_MAX_LINES) {
		lines[nlines] = ls;
		line_lens[nlines] = (int)(end - ls);
		nlines++;
	}
	return nlines;
}

/* Find the `Local Variables:` line, matched case-insensitively, and read
 * the comment envelope off it: whatever precedes the marker is the prefix
 * every line of the block must carry, whatever follows it the suffix. */
static bool footer_find_start(const char **lines, const int *line_lens,
    int nlines, struct footer_block *b)
{
	static const char marker[] = "Local Variables:";
	const int marker_len = (int)sizeof(marker) - 1;

	for (int li = 0; li < nlines; li++) {
		const char *ln = lines[li];
		int ll = line_lens[li];

		for (int p = 0; p <= ll - marker_len; p++) {
			int k = 0;

			while (k < marker_len
			    && tolower((unsigned char)ln[p + k])
				== tolower((unsigned char)marker[k])) {
				k++;
			}
			if (k < marker_len) {
				continue;
			}
			b->start_line = li;
			b->prefix = ln;
			b->prefix_len = p;
			b->suffix = ln + p + marker_len;
			b->suffix_len = ll - p - marker_len;
			return true;
		}
	}
	return false;
}

/* Find the block's `End:` line, inside the same envelope.  A block the
 * text runs out on is not a block at all. */
static bool footer_find_end(const char **lines, const int *line_lens,
    int nlines, struct footer_block *b)
{
	for (int li = b->start_line + 1; li < nlines; li++) {
		const char *body;
		int body_len;

		if (!footer_line_body(
			lines[li], line_lens[li], b, &body, &body_len)) {
			continue;
		}
		lv_trim_blanks(&body, &body_len);
		if (body_len == 4 && body[3] == ':'
		    && strncasecmp(body, "end", 3) == 0) {
			b->end_line = li;
			return true;
		}
	}
	return false;
}

/* Read a string value that a trailing backslash may continue onto the
 * lines below, which is the footer envelope's own rule -- the modeline
 * has no such thing.  `*li` enters as the line the value started on and
 * leaves as the last line consumed.  Every way of failing -- a quote left
 * open with nothing to continue it, a continuation that runs past `End:`
 * or out of the envelope, an accumulation that outgrows the variable --
 * is one malformed entry. */
static void footer_read_string(const char **lines, const int *line_lens,
    const struct footer_block *b, const char *value, int value_len, int *li,
    struct local_settings *out)
{
	char acc[KG_COMPILE_COMMAND_MAX];
	int acc_len;

	/* The `< 0` is not reachable -- lv_split_assignment() derives
	 * value_len from a colon strictly inside the span -- but nothing in
	 * this function says so, and a value_len the analyzer is free to
	 * assume negative makes both the memcpy() below and `acc[acc_len]`
	 * run backwards.  Same shape as reflow_word_stream()'s `len <= 0`.
	 * Without it clang-analyzer reports "Out of bound access to memory
	 * preceding 'acc'" and ci-06 fails. */
	if (value_len < 0 || value_len >= KG_COMPILE_COMMAND_MAX) {
		out->malformed_entries++;
		return;
	}
	acc_len = value_len;
	memcpy(acc, value, (size_t)value_len);

	for (;;) {
		char decoded[KG_COMPILE_COMMAND_MAX];
		const char *body;
		int body_len, rc;

		acc[acc_len] = '\0';
		rc = parse_quoted_string(acc, decoded, sizeof(decoded));
		if (rc >= 0) {
			localvars_apply_string(out, decoded, rc);
			return;
		}
		if (rc != -3) {
			break;
		}

		/* Unterminated: only a line ending in an unescaped
		 * backslash asks for the next one, and that backslash is
		 * the continuation marker rather than text.  The
		 * `acc_len > 0` is what makes dropping it safe to the
		 * analyzer as well as to the reader: an empty accumulator
		 * cannot end in a backslash, but only parse_quoted_string()
		 * knows that (it answers -1, not -3, for an empty value), and
		 * that is a function away. */
		if (acc_len <= 0
		    || !footer_line_body(
			lines[*li], line_lens[*li], b, &body, &body_len)
		    || !footer_body_ends_unescaped_bslash(body, body_len)) {
			break;
		}
		acc_len--;

		(*li)++;
		if (*li >= b->end_line) {
			break;
		}
		if (!footer_line_body(
			lines[*li], line_lens[*li], b, &body, &body_len)
		    || body_len >= KG_COMPILE_COMMAND_MAX - acc_len) {
			break;
		}
		memcpy(acc + acc_len, body, (size_t)body_len);
		acc_len += body_len;
	}
	out->malformed_entries++;
}

int localvars_parse_footer(
    const erow *rows, int row_count, struct local_settings *out)
{
	char window[FOOTER_TAIL_BYTES + FOOTER_WIN_EXTRA];
	const char *lines[FOOTER_MAX_LINES];
	int line_lens[FOOTER_MAX_LINES];
	struct footer_block block;
	int nlines, wpos;

	local_settings_init(out);
	if (row_count <= 0 || rows == NULL) {
		return -1;
	}

	wpos = footer_build_window(rows, row_count, window, sizeof(window));
	nlines = footer_split_lines(window, wpos, lines, line_lens);

	if (!footer_find_start(lines, line_lens, nlines, &block)
	    || !footer_find_end(lines, line_lens, nlines, &block)) {
		return -1;
	}

	for (int li = block.start_line + 1; li < block.end_line; li++) {
		const char *body, *value;
		int body_len, value_len;
		char name[64];

		if (!footer_line_body(
			lines[li], line_lens[li], &block, &body, &body_len)) {
			continue;
		}

		switch (lv_split_assignment(
		    body, body_len, name, sizeof(name), &value, &value_len)) {
		case LV_ASSIGN_BLANK:
			continue;
		case LV_ASSIGN_MALFORMED:
			out->malformed_entries++;
			continue;
		case LV_ASSIGN_OK:
			break;
		}

		switch (localvars_kind(name)) {
		case LOCAL_VAR_STRING:
			footer_read_string(lines, line_lens, &block, value,
			    value_len, &li, out);
			break;
		case LOCAL_VAR_BOOL:
			localvars_apply_bool(out, value, value_len);
			break;
		case LOCAL_VAR_TAB_WIDTH:
		case LOCAL_VAR_C_OFFSET:
			apply_int_token(
			    out, localvars_kind(name), value, value_len);
			break;
		case LOCAL_VAR_INDENT_TABS:
			apply_indent_tabs(out, value, value_len);
			break;
		default:
			out->ignored_entries++;
			break;
		}
	}

	return 0;
}
