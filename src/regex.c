#include "regex.h"
#include "def.h"
#include <string.h>

/* First byte of the glyph containing byte `pos`: walk back over
 * continuation bytes to a lead byte whose glyph really covers `pos`.  A
 * stray continuation byte is a glyph of its own, matching
 * utf8_glyph_span_at()'s rule for corrupt sequences. */
static int utf8_glyph_start(const char *text, int len, int pos)
{
	int limit = pos > 3 ? pos - 3 : 0;
	int lead = pos;

	while (lead > limit && utf8_is_cont((unsigned char)text[lead])) {
		lead--;
	}
	if (lead != pos && utf8_glyph_span_at(text, len, lead) > pos - lead) {
		return lead;
	}
	return pos;
}

/* One past the last byte of the glyph containing byte `pos`, which must
 * be inside the subject.  Never exceeds `len`: utf8_glyph_span_at() only
 * reports a multi-byte glyph when every continuation byte is present. */
static int utf8_glyph_end(const char *text, int len, int pos)
{
	int start = utf8_glyph_start(text, len, pos);

	return start + utf8_glyph_span_at(text, len, start);
}

int kg_utf8_forward_boundary(const char *text, int len, int requested)
{
	int start;

	if (len < 0) {
		len = 0;
	}
	if (!text || requested <= 0) {
		return 0;
	}
	if (requested >= len) {
		return len;
	}
	start = utf8_glyph_start(text, len, requested);
	if (start == requested) {
		return requested;
	}
	/* utf8_glyph_start() only moves back for a glyph that really
	 * covers `requested`, so its end is strictly past it. */
	return start + utf8_glyph_span_at(text, len, start);
}

int kg_regex_next_offset(const char *text, int len, const struct kg_span *match)
{
	int start, end;

	if (!text || !match || len < 0) {
		return -1;
	}
	start = match->start;
	end = match->end;
	if (start < 0 || end < start) {
		return -1;
	}
	if (end > start) {
		return end > len ? len : end;
	}
	/* An empty match consumed nothing, so the scan has to step over a
	 * whole glyph itself; at the end of the subject there is none. */
	if (start >= len) {
		return -1;
	}
	return start + utf8_glyph_span_at(text, len, start);
}

/* Reject a span the engine got wrong, then widen the surviving one to
 * whole glyphs so no boundary lands inside a UTF-8 sequence.  Empty
 * spans stay empty and only move to the start of their glyph.  Returns 0
 * when the span is out of bounds for `text`.
 *
 * The engine steps by glyph itself now, so a well-formed match already
 * arrives on glyph boundaries; this stays as defence in depth, and it
 * still does real work when `start_offset` lands mid-glyph. */
static int kg_span_snap(struct kg_span *sp, const char *text, int len)
{
	int start, end;

	if (sp->start < 0 || sp->end < sp->start || sp->start > len
	    || sp->end > len) {
		return 0;
	}
	start = utf8_glyph_start(text, len, sp->start);
	end = sp->start == sp->end ? start
				   : utf8_glyph_end(text, len, sp->end - 1);
	sp->start = start;
	sp->end = end;
	return 1;
}

/* Copy an engine result into `out`, validating every span against the
 * subject.  A bad span 0 means the engine's answer is garbage and the
 * whole match is dropped (returns 0); a bad capture is reported as the
 * "did not participate" span the engine itself uses. */
static int kg_regex_take_match(
    struct kg_match *out, const re_match_result *res, const char *text, int len)
{
	int i, n;

	n = res->nspans > RE_MAX_SPANS ? RE_MAX_SPANS : res->nspans;
	if (n < 1) {
		out->nspans = 0;
		return 0;
	}
	out->nspans = n;
	for (i = 0; i < n; i++) {
		out->spans[i].start = res->spans[i].start;
		out->spans[i].end = res->spans[i].end;
		if (kg_span_snap(&out->spans[i], text, len)) {
			continue;
		}
		if (i == 0) {
			out->nspans = 0;
			return 0;
		}
		out->spans[i].start = -1;
		out->spans[i].end = -1;
	}
	return 1;
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
	struct kg_match scratch;
	int text_len = (int)strlen(text);
	int from;
	re_status status;

	/* A request outside the subject stays no match, as re_exec() reports
	 * it: normalizing it would fold "past the end" onto "at the end", and
	 * a caller walking empty matches would never leave the last one. */
	if (start_offset < 0 || start_offset > text_len) {
		return KG_REGEX_NOMATCH;
	}
	/* Resume on a glyph boundary: an offset inside a sequence would let
	 * the engine match from a continuation byte, and kg_span_snap()
	 * would then hand back a match starting before the request. */
	from = kg_utf8_forward_boundary(text, text_len, start_offset);
	status = re_exec(rx->regex, text, from, &res);

	if (status != RE_STATUS_OK) {
		return KG_REGEX_NOMATCH;
	}
	if (!kg_regex_take_match(out ? out : &scratch, &res, text, text_len)) {
		return KG_REGEX_NOMATCH;
	}
	return KG_REGEX_OK;
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
		struct kg_match cur;
		re_status status = re_exec(rx->regex, text, offset, &res);
		if (status != RE_STATUS_OK) {
			break;
		}
		if (!kg_regex_take_match(&cur, &res, text, text_len)) {
			break;
		}

		int start = cur.spans[0].start;
		int end = cur.spans[0].end;
		int next;

		if (start == end) {
			if (start < before) {
				last_match = cur;
				found = 1;
			}
		} else {
			if (end <= before) {
				last_match = cur;
				found = 1;
			}
		}
		/* The next candidate start is the glyph after this match's
		 * first byte -- for an empty match that is exactly
		 * kg_regex_next_offset(), and for a consuming one it is less
		 * than the match end on purpose: skipping to the end would
		 * drop the overlapping matches this scan exists to find.
		 * Stepping by whole glyphs keeps the engine off continuation
		 * bytes.  Snapping can pull `start` back before `offset`, and
		 * a confused engine could report a match behind it too, so
		 * the scan position still has to advance every iteration. */
		next = kg_utf8_forward_boundary(
		    text, text_len, (start > offset ? start : offset) + 1);
		if (next <= offset) {
			break;
		}
		offset = next;
	}

	if (found) {
		if (out) {
			*out = last_match;
		}
		return KG_REGEX_OK;
	}
	return KG_REGEX_NOMATCH;
}
