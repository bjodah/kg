#include <stdio.h>
#include <string.h>

#include "../fe/fe.h"
#include "bufhandle.h"
#include "def.h"
#include "lisp_internal.h"
#include "localvars.h"
#include "marker.h"
#include "regex.h"

/* ---- search-forward/-backward, re-search-forward/-backward -----------
 *
 * kg's regex engine (src/regex.h) matches within one NUL-terminated row
 * at a time -- the same architecture src/search.c's incremental search
 * uses -- so, exactly as for C-s, a match here can never span a row
 * boundary.  These natives add the two things C-s does not have: a hard
 * BOUND and no wraparound, which is search-forward/search-backward's
 * documented contract in Emacs and not incremental search's.
 *
 * Case folding (case-fold-search) is not implemented: every search here
 * is case-sensitive, unlike Emacs' default.  Recorded as deferred in the
 * sub-plan's Phase 3 notes rather than silently approximated.
 *
 * A literal search string may hold an embedded NUL (copy_fe_string keeps
 * its real length), and is matched with explicit-length comparisons, so
 * it is not subject to regex.h's NUL-terminated-subject limitation the
 * way a pattern necessarily is. */

/* One match, row-relative: the row it was found on and its byte columns.
 * Mirrors struct kg_lisp_match_data's shape because it becomes one on a
 * successful search. */
struct lisp_search_hit {
	int found_row;
	struct kg_match match;
};

/* Try one row, forward, for the first match starting at or after
 * `from_col` whose end does not exceed `limit_col`.  Returns 1 with
 * *hit filled, 0 for no match in this row, -1 for KG_REGEX_TOO_COMPLEX.
 *
 * A match that starts within bounds but ends past `limit_col` ends the
 * search in this row rather than trying a later start: every later
 * literal match starts no earlier and is the same length, so it cannot
 * end sooner, and editor_query_replace_regexp() already makes the same
 * simplification for regex matches (its "match straddles the region
 * end" check stops rather than searching on). */
static int lisp_search_row_forward(erow *row, int from_col, int limit_col,
    bool regexp, const struct kg_regex *rx, const char *needle,
    size_t needle_len, struct lisp_search_hit *hit)
{
	if (from_col < 0) {
		from_col = 0;
	}
	if (from_col > row->size) {
		return 0;
	}
	if (regexp) {
		int status = kg_regex_match_forward(
		    rx, row->chars, from_col, &hit->match);

		if (status == KG_REGEX_TOO_COMPLEX) {
			return -1;
		}
		if (status != KG_REGEX_OK) {
			return 0;
		}
		return hit->match.spans[0].end <= limit_col;
	}
	if (needle_len == 0) {
		/* An empty search string matches immediately, as in Emacs. */
		if (from_col > limit_col) {
			return 0;
		}
		hit->match.nspans = 1;
		hit->match.spans[0] = (struct kg_span) { from_col, from_col };
		return 1;
	}
	{
		const char *found = memmem(row->chars + from_col,
		    (size_t)(row->size - from_col), needle, needle_len);
		int start, end;

		if (!found) {
			return 0;
		}
		start = (int)(found - row->chars);
		end = start + (int)needle_len;
		if (end > limit_col) {
			return 0;
		}
		hit->match.nspans = 1;
		hit->match.spans[0] = (struct kg_span) { start, end };
		return 1;
	}
}

/* Row-by-row, start_row..bound_row, no wraparound.  Raises on
 * cancellation or a too-complex pattern rather than returning either as
 * "no match", which would be a lie. */
static int lisp_search_forward(FeContext *ctx, struct editor_buffer *b,
    int start_row, int start_col, int bound_row, int bound_col, bool regexp,
    const struct kg_regex *rx, const char *needle, size_t needle_len,
    struct lisp_search_hit *hit)
{
	int row;

	for (row = start_row; row <= bound_row && row < b->numrows; row++) {
		int from_col = (row == start_row) ? start_col : 0;
		int limit_col
		    = (row == bound_row) ? bound_col : b->row[row].size;
		int rc;

		if (state.interrupt_check && state.interrupt_check()) {
			FeRaiseCompletion(
			    ctx, FeCompletionQuit, "evaluation cancelled");
		}
		rc = lisp_search_row_forward(&b->row[row], from_col, limit_col,
		    regexp, rx, needle, needle_len, hit);
		if (rc < 0) {
			FeHandleError(
			    ctx, "search: regular expression too complex");
		}
		if (rc > 0) {
			hit->found_row = row;
			return 1;
		}
	}
	return 0;
}

/* The last literal match of `needle` in [limit_col, before_col) of `row`.
 * Naive O(row length), matching isearch_find_last_before()'s own
 * approach in src/search.c. */
static int lisp_search_row_backward_literal(erow *row, int before_col,
    int limit_col, const char *needle, size_t needle_len,
    struct lisp_search_hit *hit)
{
	int best = -1;
	int col;

	if (needle_len == 0) {
		if (before_col < limit_col) {
			return 0;
		}
		hit->match.nspans = 1;
		hit->match.spans[0]
		    = (struct kg_span) { before_col, before_col };
		return 1;
	}
	for (col = limit_col; col + (int)needle_len <= before_col; col++) {
		if (memcmp(row->chars + col, needle, needle_len) == 0) {
			best = col;
		}
	}
	if (best < 0) {
		return 0;
	}
	hit->match.nspans = 1;
	hit->match.spans[0] = (struct kg_span) { best, best + (int)needle_len };
	return 1;
}

/* Row-by-row, start_row..bound_row, no wraparound.
 *
 * The regex case is a known, documented simplification, not full Emacs
 * backward search: kg_regex_match_backward() answers "the last match in
 * this row ending at or before `before_col`", with no lower bound of its
 * own (see its header comment in src/regex.h and the CLAUDE.md note this
 * follows).  A row is tried exactly once here; if that one match starts
 * before `limit_col`, the row is treated as having no eligible match at
 * all, even though an earlier, in-bounds match may exist between
 * `limit_col` and it.  Emacs would find that match; this does not. */
static int lisp_search_backward(FeContext *ctx, struct editor_buffer *b,
    int start_row, int start_col, int bound_row, int bound_col, bool regexp,
    const struct kg_regex *rx, const char *needle, size_t needle_len,
    struct lisp_search_hit *hit)
{
	int row;

	for (row = start_row; row >= bound_row; row--) {
		int before_col
		    = (row == start_row) ? start_col : b->row[row].size;
		int limit_col = (row == bound_row) ? bound_col : 0;

		if (state.interrupt_check && state.interrupt_check()) {
			FeRaiseCompletion(
			    ctx, FeCompletionQuit, "evaluation cancelled");
		}
		if (before_col < limit_col) {
			continue;
		}
		if (regexp) {
			int status = kg_regex_match_backward(
			    rx, b->row[row].chars, before_col, &hit->match);

			if (status == KG_REGEX_TOO_COMPLEX) {
				FeHandleError(ctx,
				    "search: regular expression too complex");
			}
			if (status == KG_REGEX_OK
			    && hit->match.spans[0].start >= limit_col) {
				hit->found_row = row;
				return 1;
			}
			continue;
		}
		if (lisp_search_row_backward_literal(&b->row[row], before_col,
			limit_col, needle, needle_len, hit)) {
			hit->found_row = row;
			return 1;
		}
	}
	return 0;
}

/* An optional BOUND argument: a 1-based position like any other, or
 * `default_off` (a 0-based codepoint offset) when omitted or nil. */
static long lisp_search_bound(FeContext *context, struct editor_buffer *b,
    FeObject **arguments, long default_off)
{
	FeObject *object;

	if (FeIsNil(*arguments)) {
		return default_off;
	}
	object = FeGetNextArgument(context, arguments);
	if (FeIsNil(object)) {
		return default_off;
	}
	return lisp_offset_argument(context, b, object);
}

enum lisp_search_dir { LISP_SEARCH_FORWARD, LISP_SEARCH_BACKWARD };

static FeObject *lisp_search(FeContext *context, FeObject *arguments,
    enum lisp_search_dir dir, bool regexp)
{
	FeObject *pattern_object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	long point_off = lisp_exec_point_char(context);
	long default_bound
	    = dir == LISP_SEARCH_FORWARD ? lisp_buffer_char_length(b) : 0;
	long bound_off;
	int start_row, start_col, bound_row, bound_col;
	char *pattern;
	size_t pattern_len;
	struct kg_regex rx;
	struct lisp_search_hit hit = { 0 };
	int found;
	size_t match_byte;

	pattern = copy_fe_string(context, pattern_object, &pattern_len);
	/* Parked in state.scratch, the same way lisp_string.c and
	 * lisp_buffer.c park theirs: lisp_search_bound() and
	 * FeRequireNoArguments() below can both raise, and a raise longjmps
	 * past every free() in this function.  .ci/ci-04's LeakSanitizer
	 * caught exactly that. */
	state.scratch = pattern;
	bound_off = lisp_search_bound(context, b, &arguments, default_bound);
	FeRequireNoArguments(context, arguments);

	if (regexp) {
		int status = kg_regex_compile(&rx, pattern, 0);

		if (status == KG_REGEX_TOODEEP) {
			release_scratch();
			FeHandleError(context, "regexp too complex to compile");
		}
		if (status != KG_REGEX_OK) {
			char message[400];

			(void)snprintf(message, sizeof(message),
			    "invalid regexp: %s", pattern);
			release_scratch();
			FeHandleError(context, message);
		}
	}

	buffer_position_to_row_col(
	    b, lisp_byte_of_char_offset(b, point_off), &start_row, &start_col);
	buffer_position_to_row_col(
	    b, lisp_byte_of_char_offset(b, bound_off), &bound_row, &bound_col);

	if (b->numrows == 0) {
		found = 0;
	} else if (dir == LISP_SEARCH_FORWARD) {
		found = lisp_search_forward(context, b, start_row, start_col,
		    bound_row, bound_col, regexp, &rx, pattern, pattern_len,
		    &hit);
	} else {
		found = lisp_search_backward(context, b, start_row, start_col,
		    bound_row, bound_col, regexp, &rx, pattern, pattern_len,
		    &hit);
	}
	release_scratch();

	if (!found) {
		return FeNil(context);
	}

	state.match.valid = true;
	state.match.buffer = state.exec.buffer;
	state.match.row = hit.found_row;
	state.match.match = hit.match;

	match_byte = buffer_row_col_to_position(b, hit.found_row,
	    dir == LISP_SEARCH_FORWARD ? hit.match.spans[0].end
				       : hit.match.spans[0].start);
	(void)kg_marker_set_position(lisp_exec_point_marker(), match_byte);
	return lisp_position(context, lisp_char_offset_of(b, match_byte));
}

FeObject *native_search_forward(FeContext *context, FeObject *arguments)
{
	return lisp_search(context, arguments, LISP_SEARCH_FORWARD, false);
}

FeObject *native_search_backward(FeContext *context, FeObject *arguments)
{
	return lisp_search(context, arguments, LISP_SEARCH_BACKWARD, false);
}

FeObject *native_re_search_forward(FeContext *context, FeObject *arguments)
{
	return lisp_search(context, arguments, LISP_SEARCH_FORWARD, true);
}

FeObject *native_re_search_backward(FeContext *context, FeObject *arguments)
{
	return lisp_search(context, arguments, LISP_SEARCH_BACKWARD, true);
}

/* ---- match-beginning / match-end --------------------------------------
 * Read the last search's captures out of struct kg_lisp_match_data
 * (state.match), addressed through this module rather than a bare
 * file-scope global.  It outlives the frame that set it -- a search and
 * a later, separate (match-beginning 0) are ordinarily two different
 * top-level forms -- and is resolved against the buffer it names, not
 * whatever the exec buffer happens to be when read. */

static FeObject *lisp_match_bound(
    FeContext *context, FeObject *arguments, bool want_end)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	long n = (long)lisp_finite(context, object);
	struct editor_buffer *b;
	int col;

	FeRequireNoArguments(context, arguments);
	if (!state.match.valid || n < 0 || n >= state.match.match.nspans
	    || state.match.match.spans[n].start < 0) {
		return FeNil(context);
	}
	b = buf_resolve(state.match.buffer);
	if (b == NULL) {
		FeHandleError(context, "match-data: buffer is dead");
	}
	col = want_end ? state.match.match.spans[n].end
		       : state.match.match.spans[n].start;
	return lisp_position(context,
	    lisp_char_offset_of(
		b, buffer_row_col_to_position(b, state.match.row, col)));
}

FeObject *native_match_beginning(FeContext *context, FeObject *arguments)
{
	return lisp_match_bound(context, arguments, false);
}

FeObject *native_match_end(FeContext *context, FeObject *arguments)
{
	return lisp_match_bound(context, arguments, true);
}
