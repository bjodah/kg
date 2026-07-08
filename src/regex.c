#include "regex.h"
#include <string.h>

static void kg_regex_copy_match(
    struct kg_match *out, const re_match_result *res)
{
	out->nspans = res->nspans > RE_MAX_SPANS ? RE_MAX_SPANS : res->nspans;
	for (int i = 0; i < out->nspans; i++) {
		out->spans[i].start = res->spans[i].start;
		out->spans[i].end = res->spans[i].end;
	}
}

int kg_regex_compile(struct kg_regex *rx, const char *pattern, int flags)
{
	if (!rx || !pattern) {
		return KG_REGEX_BADPAT;
	}

	rx->storage_size = sizeof(rx->storage);
	re_flags re_f = RE_FLAG_NONE;
	if (flags & KG_REGEX_ICASE) {
		re_f = RE_FLAG_ICASE;
	}

	re_t out_re = NULL;
	re_status status = re_compile_checked(
	    pattern, re_f, rx->storage, &rx->storage_size, &out_re);

	if (status == RE_STATUS_OK) {
		rx->regex = out_re;
		return KG_REGEX_OK;
	} else if (status == RE_STATUS_TOO_COMPLEX) {
		return KG_REGEX_TOODEEP;
	} else {
		return KG_REGEX_BADPAT;
	}
}

int kg_regex_match_forward(const struct kg_regex *rx, const char *text,
    int start_offset, struct kg_match *out)
{
	if (!rx || !rx->regex || !text) {
		return KG_REGEX_NOMATCH;
	}

	re_match_result res;
	re_status status = re_exec(rx->regex, text, start_offset, &res);

	if (status == RE_STATUS_OK) {
		if (out) {
			kg_regex_copy_match(out, &res);
		}
		return KG_REGEX_OK;
	} else {
		return KG_REGEX_NOMATCH;
	}
}

int kg_regex_match_backward(const struct kg_regex *rx, const char *text,
    int before, struct kg_match *out)
{
	if (!rx || !rx->regex || !text) {
		return KG_REGEX_NOMATCH;
	}

	int offset = 0;
	int found = 0;
	struct kg_match last_match;
	int text_len = (int)strlen(text);

	while (offset <= text_len) {
		re_match_result res;
		re_status status = re_exec(rx->regex, text, offset, &res);
		if (status != RE_STATUS_OK) {
			break;
		}

		int start = res.spans[0].start;
		int end = res.spans[0].end;

		if (start == end) {
			if (start < before) {
				kg_regex_copy_match(&last_match, &res);
				found = 1;
			}
			offset = start + 1;
		} else {
			if (end <= before) {
				kg_regex_copy_match(&last_match, &res);
				found = 1;
			}
			offset = start + 1;
		}
	}

	if (found) {
		if (out) {
			*out = last_match;
		}
		return KG_REGEX_OK;
	}
	return KG_REGEX_NOMATCH;
}
