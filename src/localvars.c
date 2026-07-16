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
