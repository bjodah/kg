#include "localvars.h"
#include "def.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void local_settings_init(struct local_settings *settings)
{
	memset(settings, 0, sizeof(*settings));
	settings->buffer_read_only = LOCAL_BOOL_UNSET;
	settings->compile_command[0] = '\0';
	settings->compile_command_set = false;
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

	destination->ignored_entries += source->ignored_entries;
	destination->malformed_entries += source->malformed_entries;
}

/* ---- The variables, and what their values mean ----
 *
 * kg understands two file-local variables through three envelope
 * grammars.  The grammars differ in how a name and a value are *found*
 * -- semicolon splitting inside `-*- ... -*-`, a prefixed/suffixed block
 * with backslash continuation, or a non-evaluating sexp reader -- and
 * that scanning stays with each parser.  What a name means, and what a
 * value means once it has been found, is stated only here, so a third
 * variable is one row and not three edits. */

enum local_var_kind localvars_kind(const char *name)
{
	static const struct {
		const char *name;
		enum local_var_kind kind;
	} table[] = {
		{ "compile-command", LOCAL_VAR_STRING },
		{ "buffer-read-only", LOCAL_VAR_BOOL },
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
void localvars_apply_bool(struct local_settings *out, const char *text, int len)
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
		out->buffer_read_only = LOCAL_BOOL_TRUE;
	} else if (strcmp(token, "nil") == 0) {
		out->buffer_read_only = LOCAL_BOOL_FALSE;
	} else {
		out->malformed_entries++;
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

int localvars_parse_modeline(
    const erow *rows, int row_count, struct local_settings *out)
{
	int cand, i;
	const char *line, *start_marker;
	const char *search_from, *end_marker;
	const char *props;
	int len, props_len, remaining;

	local_settings_init(out);

	if (row_count < 1 || rows == NULL) {
		return -1;
	}

	cand = 0;
	if (row_count >= 2 && rows[0].size >= 2 && rows[0].chars[0] == '#'
	    && rows[0].chars[1] == '!') {
		cand = 1;
	}

	line = rows[cand].chars;
	len = rows[cand].size;

	start_marker = NULL;
	for (i = 0; i <= len - 3; i++) {
		if (line[i] == '-' && line[i + 1] == '*'
		    && line[i + 2] == '-') {
			start_marker = line + i;
			break;
		}
	}
	if (!start_marker) {
		return -1;
	}

	search_from = start_marker + 3;
	remaining = len - (int)(search_from - line);
	if (remaining < 3) {
		return -1;
	}
	end_marker = NULL;
	for (i = 0; i <= remaining - 3; i++) {
		if (search_from[i] == '-' && search_from[i + 1] == '*'
		    && search_from[i + 2] == '-') {
			end_marker = search_from + i;
			break;
		}
	}
	if (!end_marker) {
		return -1;
	}

	props = start_marker + 3;
	props_len = (int)(end_marker - props);

	{
		const char *seg = props;
		const char *pend = props + props_len;

		while (seg < pend) {
			bool in_q = false;
			bool esc = false;
			int pdepth = 0;
			const char *sep = NULL;
			const char *p;

			for (p = seg; p < pend; p++) {
				if (esc) {
					esc = false;
					continue;
				}
				if (*p == '\\') {
					esc = true;
					continue;
				}
				if (*p == '"') {
					in_q = !in_q;
					continue;
				}
				if (in_q) {
					continue;
				}
				if (*p == '(') {
					pdepth++;
					continue;
				}
				if (*p == ')' && pdepth > 0) {
					pdepth--;
					continue;
				}
				if (*p == ';' && pdepth == 0) {
					sep = p;
					break;
				}
			}

			{
				const char *seg_start = seg;
				const char *seg_end = sep ? sep : pend;
				int seg_len;
				const char *colon;
				const char *name_start;
				int name_len;
				char name[64];
				int nl;
				const char *val;

				while (seg_start < seg_end
				    && (*seg_start == ' '
					|| *seg_start == '\t')) {
					seg_start++;
				}
				while (seg_end > seg_start
				    && (seg_end[-1] == ' '
					|| seg_end[-1] == '\t')) {
					seg_end--;
				}
				seg_len = (int)(seg_end - seg_start);
				if (seg_len <= 0) {
					goto next_seg;
				}

				in_q = false;
				esc = false;
				colon = NULL;
				for (p = seg_start; p < seg_end; p++) {
					if (esc) {
						esc = false;
						continue;
					}
					if (*p == '\\') {
						esc = true;
						continue;
					}
					if (*p == '"') {
						in_q = !in_q;
						continue;
					}
					if (!in_q && *p == ':') {
						colon = p;
						break;
					}
				}
				if (!colon) {
					out->malformed_entries++;
					goto next_seg;
				}

				name_start = seg_start;
				name_len = (int)(colon - seg_start);
				while (name_len > 0
				    && (name_start[name_len - 1] == ' '
					|| name_start[name_len - 1] == '\t')) {
					name_len--;
				}
				if (name_len <= 0) {
					out->malformed_entries++;
					goto next_seg;
				}

				nl = (name_len < 63) ? name_len : 63;
				for (i = 0; i < nl; i++) {
					name[i] = (char)tolower(
					    (unsigned char)name_start[i]);
				}
				name[nl] = '\0';

				val = colon + 1;
				while (val < seg_end
				    && (*val == ' ' || *val == '\t')) {
					val++;
				}

				switch (localvars_kind(name)) {
				case LOCAL_VAR_STRING: {
					char decoded[KG_COMPILE_COMMAND_MAX];
					int rc = parse_quoted_string(
					    val, decoded, sizeof(decoded));
					if (rc < 0) {
						out->malformed_entries++;
					} else {
						localvars_apply_string(
						    out, decoded, rc);
					}
					break;
				}
				case LOCAL_VAR_BOOL:
					localvars_apply_bool(
					    out, val, (int)(seg_end - val));
					break;
				default:
					out->ignored_entries++;
					break;
				}
			}

		next_seg:
			seg = sep ? sep + 1 : pend;
		}
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
	DL_MAX_FILESIZE = 65536,
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

			if (c == ' ' || c == '\t' || c == '\n' || c == '\r'
			    || c == '\f') {
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
	} else if (kind == LOCAL_VAR_BOOL) {
		if (r->src[r->pos] == '"' || r->src[r->pos] == '('
		    || r->src[r->pos] == ')') {
			if (dlr_skip_sexp(r) != 0) {
				return -1;
			}
			out->malformed_entries++;
		} else {
			char symval[12];
			int svlen = dlr_read_sym(r, symval, sizeof(symval));

			if (svlen < 0) {
				out->malformed_entries++;
			} else {
				localvars_apply_bool(out, symval, svlen);
			}
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

int dirlocals_parse(
    const char *source, size_t source_len, struct local_settings *out)
{
	struct dlr r;
	struct local_settings tmp;

	local_settings_init(out);
	local_settings_init(&tmp);

	if (source_len > DL_MAX_FILESIZE) {
		return -1;
	}

	r.src = source;
	r.len = source_len;
	r.pos = 0;
	r.depth = 0;
	r.tokcount = 0;

	dlr_skip_ws(&r);
	if (r.pos >= r.len) {
		return 0;
	}

	if (r.src[r.pos] == '\'') {
		r.pos++;
		r.tokcount++;
		dlr_skip_ws(&r);
	}

	if (r.pos >= r.len || r.src[r.pos] != '(') {
		return -1;
	}
	r.pos++;
	r.depth++;
	r.tokcount++;
	if (r.depth > DL_MAX_NESTING || r.tokcount > DL_MAX_TOKENS) {
		return -1;
	}

	for (;;) {
		dlr_skip_ws(&r);
		if (r.pos >= r.len) {
			return -1;
		}

		if (r.src[r.pos] == ')') {
			r.pos++;
			r.depth--;
			break;
		}

		if (r.src[r.pos] != '(') {
			return -1;
		}
		r.pos++;
		r.depth++;
		r.tokcount++;
		if (r.depth > DL_MAX_NESTING || r.tokcount > DL_MAX_TOKENS) {
			return -1;
		}

		{
			char selector[128];
			int slen = dlr_read_sym(&r, selector, sizeof(selector));

			if (slen < 0) {
				return -1;
			}
			r.tokcount++;
			if (r.tokcount > DL_MAX_TOKENS) {
				return -1;
			}

			dlr_skip_ws(&r);
			if (r.pos >= r.len || r.src[r.pos] != '.') {
				if (dlr_skip_sexp(&r) != 0) {
					return -1;
				}
				goto close_entry;
			}
			r.pos++;
			r.tokcount++;
			if (r.tokcount > DL_MAX_TOKENS) {
				return -1;
			}

			if (strcmp(selector, "nil") == 0) {
				dlr_skip_ws(&r);
				if (r.pos >= r.len || r.src[r.pos] != '(') {
					tmp.malformed_entries++;
					if (dlr_skip_sexp(&r) != 0) {
						return -1;
					}
					goto close_entry;
				}
				r.pos++;
				r.depth++;
				r.tokcount++;
				if (r.depth > DL_MAX_NESTING
				    || r.tokcount > DL_MAX_TOKENS) {
					return -1;
				}

				for (;;) {
					dlr_skip_ws(&r);
					if (r.pos >= r.len) {
						return -1;
					}
					if (r.src[r.pos] == ')') {
						r.pos++;
						r.depth--;
						break;
					}
					if (dlr_apply_pair(&r, &tmp) != 0) {
						return -1;
					}
				}
			} else {
				if (dlr_skip_sexp(&r) != 0) {
					return -1;
				}
			}
		}

	close_entry:
		dlr_skip_ws(&r);
		if (r.pos < r.len && r.src[r.pos] == ')') {
			r.pos++;
			r.depth--;
		} else {
			return -1;
		}
	}

	dlr_skip_ws(&r);
	local_settings_merge(out, &tmp);
	return 0;
}

int localvars_parse_footer(
    const erow *rows, int row_count, struct local_settings *out)
{
	char window[FOOTER_TAIL_BYTES + FOOTER_WIN_EXTRA];
	const char *lines[FOOTER_MAX_LINES];
	int line_lens[FOOTER_MAX_LINES];
	int nlines, wpos, total, cut, sofar, eff_start, eff_len;
	int start_line, end_line, prefix_len, suffix_len;
	const char *prefix, *suffix;

	local_settings_init(out);
	if (row_count <= 0 || rows == NULL) {
		return -1;
	}

	total = 0;
	for (int i = 0; i < row_count; i++) {
		total += rows[i].size;
		if (i < row_count - 1) {
			total++;
		}
	}

	cut = total > FOOTER_TAIL_BYTES ? total - FOOTER_TAIL_BYTES : 0;
	wpos = 0;
	sofar = 0;

	for (int i = 0;
	    i < row_count && wpos < FOOTER_TAIL_BYTES + FOOTER_WIN_EXTRA - 1;
	    i++) {
		int row_start = sofar;
		int row_end = sofar + rows[i].size;

		if (row_end > cut) {
			int skip = row_start < cut ? cut - row_start : 0;
			int copy = row_end - (row_start + skip);
			int room
			    = FOOTER_TAIL_BYTES + FOOTER_WIN_EXTRA - 1 - wpos;

			if (copy > room) {
				copy = room;
			}
			memcpy(window + wpos, rows[i].chars + skip, copy);
			wpos += copy;
		}
		sofar = row_end;

		if (i < row_count - 1 && sofar >= cut
		    && wpos < FOOTER_TAIL_BYTES + FOOTER_WIN_EXTRA - 1) {
			window[wpos++] = '\n';
		}
		sofar += (i < row_count - 1) ? 1 : 0;
	}
	window[wpos] = '\0';

	eff_start = 0;
	for (int j = wpos - 1; j >= 0; j--) {
		if (window[j] == '\f') {
			eff_start = j + 1;
			break;
		}
	}
	eff_len = wpos - eff_start;

	nlines = 0;
	{
		const char *p = window + eff_start;
		const char *end = p + eff_len;
		const char *ls = p;

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
	}

	start_line = -1;
	prefix = NULL;
	prefix_len = 0;
	suffix = NULL;
	suffix_len = 0;

	{
		const char marker[] = "Local Variables:";
		const int marker_len = (int)(sizeof(marker) - 1);

		for (int li = 0; li < nlines; li++) {
			const char *ln = lines[li];
			int ll = line_lens[li];

			for (int p = 0; p <= ll - marker_len; p++) {
				int match = 1;
				for (int k = 0; k < marker_len; k++) {
					if (tolower((unsigned char)ln[p + k])
					    != tolower(
						(unsigned char)marker[k])) {
						match = 0;
						break;
					}
				}
				if (match) {
					start_line = li;
					prefix = ln;
					prefix_len = p;
					suffix = ln + p + marker_len;
					suffix_len = ll - p - marker_len;
					break;
				}
			}
			if (start_line >= 0) {
				break;
			}
		}
	}

	if (start_line < 0) {
		return -1;
	}

	end_line = -1;
	for (int li = start_line + 1; li < nlines; li++) {
		const char *ln = lines[li];
		int ll = line_lens[li];
		const char *body;
		int body_len;

		if (ll < prefix_len) {
			continue;
		}
		if (prefix_len > 0 && memcmp(ln, prefix, prefix_len) != 0) {
			continue;
		}
		if (suffix_len > 0
		    && (ll < suffix_len
			|| memcmp(ln + ll - suffix_len, suffix, suffix_len)
			    != 0)) {
			continue;
		}

		body = ln + prefix_len;
		body_len = ll - prefix_len - suffix_len;

		while (body_len > 0 && (body[0] == ' ' || body[0] == '\t')) {
			body++;
			body_len--;
		}
		while (body_len > 0
		    && (body[body_len - 1] == ' '
			|| body[body_len - 1] == '\t')) {
			body_len--;
		}

		if (body_len == 4 && tolower((unsigned char)body[0]) == 'e'
		    && tolower((unsigned char)body[1]) == 'n'
		    && tolower((unsigned char)body[2]) == 'd'
		    && body[3] == ':') {
			end_line = li;
			break;
		}
	}

	if (end_line < 0) {
		return -1;
	}

	for (int li = start_line + 1; li < end_line;) {
		const char *ln = lines[li];
		int ll = line_lens[li];
		const char *body;
		int body_len;
		const char *colon;
		const char *name_start;
		int name_len;
		int nl_copy;
		char name[64];
		const char *val;
		int val_len;
		int in_q, esc;
		enum local_var_kind kind;

		if (ll < prefix_len) {
			goto skip_line;
		}
		if (prefix_len > 0 && memcmp(ln, prefix, prefix_len) != 0) {
			goto skip_line;
		}
		if (suffix_len > 0
		    && (ll < suffix_len
			|| memcmp(ln + ll - suffix_len, suffix, suffix_len)
			    != 0)) {
			goto skip_line;
		}

		body = ln + prefix_len;
		body_len = ll - prefix_len - suffix_len;

		while (body_len > 0 && (body[0] == ' ' || body[0] == '\t')) {
			body++;
			body_len--;
		}
		while (body_len > 0
		    && (body[body_len - 1] == ' '
			|| body[body_len - 1] == '\t')) {
			body_len--;
		}

		if (body_len <= 0) {
			goto skip_line;
		}

		in_q = 0;
		esc = 0;
		colon = NULL;
		for (int i = 0; i < body_len; i++) {
			if (esc) {
				esc = 0;
				continue;
			}
			if (body[i] == '\\') {
				esc = 1;
				continue;
			}
			if (body[i] == '"') {
				in_q = !in_q;
				continue;
			}
			if (!in_q && body[i] == ':') {
				colon = body + i;
				break;
			}
		}

		if (!colon) {
			out->malformed_entries++;
			goto skip_line;
		}

		name_start = body;
		name_len = (int)(colon - body);
		while (name_len > 0
		    && (name_start[name_len - 1] == ' '
			|| name_start[name_len - 1] == '\t')) {
			name_len--;
		}
		if (name_len <= 0) {
			out->malformed_entries++;
			goto skip_line;
		}

		nl_copy = name_len < 63 ? name_len : 63;
		for (int i = 0; i < nl_copy; i++) {
			name[i] = (char)tolower((unsigned char)name_start[i]);
		}
		name[nl_copy] = '\0';

		val = colon + 1;
		while (val < body + body_len && (*val == ' ' || *val == '\t')) {
			val++;
		}
		val_len = body_len - (int)(val - body);

		kind = localvars_kind(name);
		if (kind == LOCAL_VAR_STRING) {
			char acc[KG_COMPILE_COMMAND_MAX];
			int acc_len, cli;

			if (val_len >= KG_COMPILE_COMMAND_MAX) {
				out->malformed_entries++;
				goto next_li;
			}
			acc_len = val_len;
			memcpy(acc, val, val_len);

			cli = li;
			for (;;) {
				char decoded[KG_COMPILE_COMMAND_MAX];
				int rc;

				acc[acc_len] = '\0';
				rc = parse_quoted_string(
				    acc, decoded, sizeof(decoded));
				if (rc >= 0) {
					localvars_apply_string(
					    out, decoded, rc);
					break;
				}
				if (rc != -3) {
					out->malformed_entries++;
					break;
				}

				{
					const char *cl = lines[cli];
					int cll = line_lens[cli];
					const char *cb = cl + prefix_len;
					int cbl = cll - prefix_len - suffix_len;

					if (!footer_body_ends_unescaped_bslash(
						cb, cbl)) {
						out->malformed_entries++;
						break;
					}
					acc_len--;

					cli++;
					if (cli >= end_line) {
						out->malformed_entries++;
						break;
					}

					{
						const char *nl = lines[cli];
						int nll = line_lens[cli];

						if (nll < prefix_len
						    || (prefix_len > 0
							&& memcmp(nl, prefix,
							       prefix_len)
							    != 0)
						    || (suffix_len > 0
							&& (nll < suffix_len
							    || memcmp(nl + nll
								       - suffix_len,
								   suffix,
								   suffix_len)
								!= 0))) {
							out->malformed_entries++;
							break;
						}

						{
							const char *nb
							    = nl + prefix_len;
							int nbl = nll
							    - prefix_len
							    - suffix_len;
							int room
							    = KG_COMPILE_COMMAND_MAX
							    - acc_len;

							if (nbl >= room) {
								out->malformed_entries++;
								break;
							}
							memcpy(acc + acc_len,
							    nb, nbl);
							acc_len += nbl;
						}
					}
				}
			}
			li = cli + 1;
			continue;
		}

		if (kind == LOCAL_VAR_BOOL) {
			localvars_apply_bool(out, val, val_len);
			goto next_li;
		}

		out->ignored_entries++;

	next_li:
		li++;
		continue;
	skip_line:
		li++;
	}

	return 0;
}
