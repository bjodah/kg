#include "regex.h"
#include "def.h"
#include <string.h>

/* kg spells "no limit" for its own callers; the engine spells it for
 * its own.  They are handed straight through to each other, so they
 * have to be the same value. */
static_assert(KG_REGEX_LIMIT_NONE == RE_LIMIT_NONE,
    "kg's unbounded spelling must be the engine's");

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

	if (requested <= 0) {
		return 0;
	}
	if (requested >= len) {
		return len;
	}
	start = utf8_glyph_start(text, len, requested);
	/* utf8_glyph_start() only moves back for a glyph that really
	 * covers `requested`, so its end is strictly past it. */
	return start == requested
	    ? requested
	    : start + utf8_glyph_span_at(text, len, start);
}

int kg_regex_next_offset(const char *text, int len, const struct kg_span *match)
{
	int start = match->start;
	int end = match->end;

	if (end > start) {
		return end > len ? len : end;
	}
	/* An empty match consumed nothing, so the scan has to step over a
	 * whole glyph itself; at the end of the subject there is none. */
	if (start < 0 || start >= len) {
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

/* What a failed match attempt means to a caller.  An exhausted step
 * budget is its own answer, never "no match": the subject may well
 * contain one.  A compiled program the engine cannot use -- a bad
 * pattern, or storage too small for it -- is reported as the bad pattern
 * it came from. */
static int kg_regex_exec_status(re_status status)
{
	if (status == RE_STATUS_NO_MATCH) {
		return KG_REGEX_NOMATCH;
	}
	if (status == RE_STATUS_TOO_COMPLEX) {
		return KG_REGEX_TOO_COMPLEX;
	}
	return KG_REGEX_BADPAT;
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
	return kg_regex_match_forward_bounded(
	    rx, text, start_offset, KG_REGEX_LIMIT_NONE, out);
}

int kg_regex_match_forward_bounded(const struct kg_regex *rx, const char *text,
    int start_offset, int limit, struct kg_match *out)
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
	/* The limit goes to the engine rather than to a filter over its
	 * answer, which is the whole point: it is enforced inside
	 * match_atom(), the one place this engine consumes, so every
	 * alternative, repetition and group above it backtracks under it.
	 * A limit past the subject, and KG_REGEX_LIMIT_NONE, are the
	 * unbounded call there too, so no normalization happens here.  The
	 * anchors are untouched by it: `\'` still asks about `text`'s
	 * terminator, not about `limit`. */
	status = re_exec_bounded(rx->regex, text, from, limit, NULL, &res);

	if (status != RE_STATUS_OK) {
		return kg_regex_exec_status(status);
	}
	if (!kg_regex_take_match(out ? out : &scratch, &res, text, text_len)) {
		return KG_REGEX_NOMATCH;
	}
	return KG_REGEX_OK;
}

int kg_regex_match_backward(const struct kg_regex *rx, const char *text,
    int before, struct kg_match *out)
{
	return kg_regex_match_backward_bounded(rx, text, 0, before, out);
}

int kg_regex_match_backward_bounded(const struct kg_regex *rx,
    const char *text, int start_offset, int limit, struct kg_match *out)
{
	if (!rx || !rx->regex || !text) {
		return KG_REGEX_NOMATCH;
	}

	int offset;
	int found = 0;
	struct kg_match last_match;
	int text_len = (int)strlen(text);
	int end_bound;

	if (limit < 0 && limit != KG_REGEX_LIMIT_NONE) {
		return KG_REGEX_NOMATCH;
	}
	/* One quantity from here down: the window's upper end in bytes of
	 * `text`.  "No limit" is the end of the subject for the empty-match
	 * rule below -- which is what kg_regex_match_backward(text, len) has
	 * always meant -- and the engine reads any limit at or past the
	 * subject as unbounded, so the same value serves both. */
	end_bound = (limit == KG_REGEX_LIMIT_NONE || limit > text_len)
	    ? text_len
	    : limit;
	offset = kg_utf8_forward_boundary(
	    text, text_len, start_offset < 0 ? 0 : start_offset);

	while (offset <= text_len) {
		re_match_result res;
		struct kg_match cur;
		/* THE SAME LIMIT TO EVERY CANDIDATE, which is what makes this
		 * sweep agree with a bounded forward search: a start whose
		 * preferred branch crosses `end_bound` yields its shorter
		 * branch here instead of producing an over-limit match that a
		 * filter would then drop, taking the start with it. */
		re_status status = re_exec_bounded(
		    rx->regex, text, offset, end_bound, NULL, &res);
		if (status != RE_STATUS_OK) {
			/* Only a genuine no-match ends the scan with whatever
			 * it has found.  Anything else leaves it unfinished, so
			 * "the last match before `limit`" was never
			 * established -- an earlier one is not the answer to
			 * the question that was asked, and the caller gets the
			 * same reason the forward wrapper would give it. */
			int why = kg_regex_exec_status(status);

			if (why != KG_REGEX_NOMATCH) {
				return why;
			}
			break;
		}
		if (!kg_regex_take_match(&cur, &res, text, text_len)) {
			break;
		}

		int start = cur.spans[0].start;
		int end = cur.spans[0].end;
		int next;

		if (start == end) {
			/* Not redundant, and not the engine's rule: an empty
			 * match exactly AT the window's end is an ordinary
			 * engine result and is not a match BEFORE it. */
			if (start < end_bound) {
				last_match = cur;
				found = 1;
			}
		} else {
			/* The engine enforced `end_bound` while matching, so a
			 * consuming match already ends at or before it; this
			 * stays as the one place that would catch an engine
			 * that did not. */
			if (end <= end_bound) {
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
