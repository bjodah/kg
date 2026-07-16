#include "localvars.h"
#include <ctype.h>
#include <string.h>

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
				const char *bv, *bve;
				int bv_len;
				char bname[12];
				int bnl;

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

				if (strcmp(name, "compile-command") == 0) {
					char decoded[KG_COMPILE_COMMAND_MAX];
					int rc = parse_quoted_string(
					    val, decoded, sizeof(decoded));
					if (rc < 0) {
						out->malformed_entries++;
					} else {
						out->compile_command_set = true;
						memcpy(out->compile_command,
						    decoded, (size_t)rc + 1);
					}
				} else if (strcmp(name, "buffer-read-only")
				    == 0) {
					bv = val;
					while (bv < seg_end
					    && (*bv == ' ' || *bv == '\t')) {
						bv++;
					}
					bve = seg_end;
					bv_len = (int)(bve - bv);
					if (bv_len < 0) {
						goto bad_ro;
					}
					bnl = (bv_len < 11) ? bv_len : 11;
					for (i = 0; i < bnl; i++) {
						bname[i] = (char)tolower(
						    (unsigned char)bv[i]);
					}
					bname[bnl] = '\0';
					if (strcmp(bname, "t") == 0) {
						out->buffer_read_only
						    = LOCAL_BOOL_TRUE;
					} else if (strcmp(bname, "nil") == 0) {
						out->buffer_read_only
						    = LOCAL_BOOL_FALSE;
					} else {
					bad_ro:
						out->malformed_entries++;
					}
				} else {
					out->ignored_entries++;
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

		if (strcmp(name, "compile-command") == 0) {
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
					out->compile_command_set = true;
					memcpy(out->compile_command, decoded,
					    (size_t)rc + 1);
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

		if (strcmp(name, "buffer-read-only") == 0) {
			const char *bv = val;
			int bv_len = val_len;
			char bname[12];
			int bnl;

			while (bv_len > 0
			    && (bv[bv_len - 1] == ' '
				|| bv[bv_len - 1] == '\t')) {
				bv_len--;
			}
			bnl = bv_len < 11 ? bv_len : 11;
			for (int i = 0; i < bnl; i++) {
				bname[i] = (char)tolower((unsigned char)bv[i]);
			}
			bname[bnl] = '\0';
			if (strcmp(bname, "t") == 0) {
				out->buffer_read_only = LOCAL_BOOL_TRUE;
			} else if (strcmp(bname, "nil") == 0) {
				out->buffer_read_only = LOCAL_BOOL_FALSE;
			} else {
				out->malformed_entries++;
			}
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
