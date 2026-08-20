#include <limits.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fe/fe.h"
#include "bufhandle.h"
#include "def.h"
#include "lisp_internal.h"
#include "localvars.h"
#include "marker.h"
#include "re.h"
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
 * `limit_col` REACHES THE ENGINE for the regexp kind (bytes of
 * row->chars, the space every column here is in), so a preferred branch
 * that would cross BOUND is shortened, loses to another alternative at
 * the same start, or gives way to a later start -- all three inside the
 * matcher.  This used to run the pattern against the whole row and then
 * test the answer's end against `limit_col`, which reaches none of them:
 * the engine's unbounded answer IS the over-BOUND one, so rejecting it
 * ended the row and Emacs' `(re-search-forward "a.*b\|x" 4 t)' over
 * "axxxb" answered nil here and 3 there (adversarial-review Finding 2).
 *
 * The LITERAL kind keeps its post-test, where the same reasoning is
 * exact rather than a simplification: every later literal match starts
 * no earlier and is the same length, so it cannot end sooner. */
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
		int status = kg_regex_match_forward_bounded(
		    rx, row->chars, from_col, limit_col, &hit->match);

		if (status == KG_REGEX_TOO_COMPLEX) {
			return -1;
		}
		return status == KG_REGEX_OK;
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
 * BOUND's two ends are one window here: backward, Emacs' BOUND is the
 * lower one (the match may not START before it) and point is the upper
 * one (it may not END past it), which is exactly
 * kg_regex_match_backward_bounded()'s [start_offset, limit].  Both are
 * bytes of row->chars.
 *
 * This used to sweep from column 0 with no limit and then test the one
 * answer's start against `limit_col`, and the review's defect was in
 * that sweep rather than in the test: a candidate start whose preferred
 * branch crossed point was dropped instead of shortened, so a row could
 * report no eligible match while holding one.  With the window inside
 * the sweep, the last match it keeps is the one with the greatest start,
 * and there is nothing left to filter.
 *
 * What is still NOT full Emacs backward search: which match kg prefers
 * among those in the window.  kg takes the plain forward match at the
 * latest candidate start; an empty match exactly at point is excluded
 * (src/regex.h).  r2-bound-backward-empty-at-point records the one
 * measured consequence. */
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
			int status = kg_regex_match_backward_bounded(rx,
			    b->row[row].chars, limit_col, before_col,
			    &hit->match);

			if (status == KG_REGEX_TOO_COMPLEX) {
				FeHandleError(ctx,
				    "search: regular expression too complex");
			}
			if (status == KG_REGEX_OK) {
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

/* Emacs' NOERROR, which is THREE answers rather than a flag, measured on
 * 31.0.91 and frozen by frontier-search-noerror-* before any of it
 * existed here: nil RAISES `search-failed' carrying the pattern and
 * leaves point, `t' answers nil and leaves point, and ANY OTHER value
 * answers nil AND MOVES POINT TO THE LIMIT -- to BOUND when one was
 * given, and to point-max forward or point-min backward when it was not.
 * The third is not optional: `t' and `'move' differ only in where point
 * ends up, so a caller that wants the move has no other way to ask.  All
 * four search names take it, on 26.2's FIXEDCASE precedent that one
 * argument means one thing across a family. */
enum lisp_search_noerror {
	LISP_SEARCH_RAISE,
	LISP_SEARCH_ANSWER_NIL,
	LISP_SEARCH_MOVE,
};

/* The optional third argument.  `t' is compared by identity because fe
 * interns its symbols, which is how src/lisp_cmd.c asks the same
 * question. */
static enum lisp_search_noerror lisp_search_noerror(
    FeContext *context, FeObject **arguments)
{
	FeObject *object;

	if (FeIsNil(*arguments)) {
		return LISP_SEARCH_RAISE;
	}
	object = FeGetNextArgument(context, arguments);
	if (FeIsNil(object)) {
		return LISP_SEARCH_RAISE;
	}
	if (object == FeMakeSymbol(context, "t")) {
		return LISP_SEARCH_ANSWER_NIL;
	}
	return LISP_SEARCH_MOVE;
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
	enum lisp_search_noerror noerror;
	int found;
	size_t match_byte;

	/* (search-forward 1) and (re-search-forward 1) are both
	 * (wrong-type-argument stringp 1) on Emacs, measured; ahead of the
	 * accessor so it is that and not fe's own prose. */
	lisp_check_string(context, pattern_object);
	pattern = copy_fe_string(context, pattern_object, &pattern_len);
	/* Parked in state.scratch, the same way lisp_string.c and
	 * lisp_buffer.c park theirs: lisp_search_bound() and
	 * FeRequireNoArguments() below can both raise, and a raise longjmps
	 * past every free() in this function.  .ci/ci-04's LeakSanitizer
	 * caught exactly that. */
	park_scratch(pattern);
	bound_off = lisp_search_bound(context, b, &arguments, default_bound);
	noerror = lisp_search_noerror(context, &arguments);
	/* A FOURTH argument is Emacs' COUNT, which kg does not implement:
	 * `wrong-number-of-arguments' by name rather than accepted and
	 * ignored, the Phase 14 `intern'-OBARRAY precedent.  Measured
	 * demand is zero -- no s.el call passes one -- and
	 * frontier-search-count-argument pins the condition. */
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
		if (noerror == LISP_SEARCH_MOVE) {
			(void)kg_marker_set_position(lisp_exec_point_marker(),
			    lisp_byte_of_char_offset(b, bound_off));
		}
		if (noerror == LISP_SEARCH_RAISE) {
			lisp_raise_search_failed(context, pattern_object);
		}
		return FeNil(context);
	}

	state.match.valid = true;
	state.match.on_string = false;
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

/* One end of group N, as the object `match-beginning'/`match-end' answers:
 * nil for a group that did not participate or that the last match does
 * not have, an integer for a string match, a buffer POSITION otherwise.
 * `match-data' below is the second caller, which is why this is a
 * function of N rather than of an argument list. */
static FeObject *lisp_match_span(FeContext *context, long n, bool want_end)
{
	struct editor_buffer *b;
	int col;

	if (!state.match.valid || n >= state.match.match.nspans
	    || state.match.match.spans[n].start < 0) {
		return FeNil(context);
	}
	/* A string match already holds Emacs' own units -- 0-based CHARACTER
	 * indices into the subject, not 1-based buffer positions -- because
	 * string-match converted them where the subject was still in hand.
	 * Nothing below this point is reachable for one: there is no buffer
	 * to resolve and no row to add. */
	if (state.match.on_string) {
		return FeMakeInteger(context,
		    (int64_t)(want_end ? state.match.match.spans[n].end
				       : state.match.match.spans[n].start));
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

static FeObject *lisp_match_bound(
    FeContext *context, FeObject *arguments, bool want_end)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	long n = (long)lisp_finite(context, object, "integerp");

	FeRequireNoArguments(context, arguments);
	/* A negative group index is Emacs' args-out-of-range, measured:
	 * (match-beginning -1) is (args-out-of-range -1 0).  A group *past*
	 * the last one is nil there and here -- the range error is only for
	 * the side that cannot name a group at all. */
	if (n < 0) {
		lisp_raise_args_out_of_range(
		    context, object, FeMakeInteger(context, 0));
	}
	return lisp_match_span(context, n, want_end);
}

FeObject *native_match_beginning(FeContext *context, FeObject *arguments)
{
	return lisp_match_bound(context, arguments, false);
}

FeObject *native_match_end(FeContext *context, FeObject *arguments)
{
	return lisp_match_bound(context, arguments, true);
}

/* ---- match-data / set-match-data --------------------------------------
 *
 * The whole match register as one value, which is what `save-match-data'
 * is built out of and what Phase 24's frontier probe found s.el reaching
 * for.  Both are C rather than prelude Lisp for one reason: the register
 * is C -- there is no Lisp-visible object it could be read out of.
 *
 * WHAT KG ANSWERS, AND WHERE IT DIVERGES.  Emacs' `match-data' returns
 * MARKERS for a buffer match unless its INTEGERS argument says otherwise;
 * kg always answers integers, which is Emacs' own `(match-data t)'.  kg
 * has no marker in the register to hand out -- `match-beginning' resolves
 * a row and a byte column into a position at the read -- and a marker
 * would promise something kg cannot keep, that the saved spans follow a
 * later edit.  INTEGERS is accepted and ignored for the same reason
 * `set-match-data''s RESEAT is: an integer list is what both sides of the
 * round trip already speak.
 *
 * `set-match-data' therefore records what it is given as a STRING match
 * whatever produced it, which is not a shortcut: the numbers in the list
 * are already in the units `match-beginning' answers, so replaying them
 * unconverted is what makes (set-match-data (match-data)) the identity
 * it has to be for `save-match-data' to work over either kind of
 * search. */

FeObject *native_match_data(FeContext *context, FeObject *arguments)
{
	FeObject *list = FeNil(context);
	size_t gc = FeSaveGC(context);
	long n;

	if (!FeIsNil(arguments)) {
		(void)FeGetNextArgument(context, &arguments); /* INTEGERS */
	}
	FeRequireNoArguments(context, arguments);
	if (!state.match.valid) {
		return FeNil(context);
	}
	/* Backwards, so the pairs come out group 0 first, and re-rooting
	 * the head after each cons -- the idiom lisp_cmd.c's name list
	 * uses: every allocation pushes its own result, so restoring the
	 * checkpoint and pushing the head keeps the stack one deep. */
	for (n = state.match.match.nspans - 1; n >= 0; n--) {
		list = FeCons(context, lisp_match_span(context, n, true), list);
		FeRestoreGC(context, gc);
		FePushGC(context, list);
		list
		    = FeCons(context, lisp_match_span(context, n, false), list);
		FeRestoreGC(context, gc);
		FePushGC(context, list);
	}
	FeRestoreGC(context, gc);
	return list;
}

/* One end out of the list `set-match-data' was given: nil is a group that
 * did not participate, which the register spells as a negative start. */
static int lisp_match_element(FeContext *context, FeObject **list)
{
	FeObject *object;

	if (FeIsNil(*list)) {
		return -1;
	}
	object = FeCar(context, *list);
	*list = FeCdr(context, *list);
	if (FeIsNil(object)) {
		return -1;
	}
	return (int)lisp_finite(context, object, "integer-or-marker-p");
}

FeObject *native_set_match_data(FeContext *context, FeObject *arguments)
{
	FeObject *list = FeGetNextArgument(context, &arguments);
	struct kg_lisp_match_data fresh = { 0 };
	int n = 0;

	if (!FeIsNil(arguments)) {
		(void)FeGetNextArgument(context, &arguments); /* RESEAT */
	}
	FeRequireNoArguments(context, arguments);
	fresh.on_string = true;
	while (!FeIsNil(list) && n < RE_MAX_SPANS) {
		int start = lisp_match_element(context, &list);
		int end = lisp_match_element(context, &list);

		fresh.match.spans[n].start = start;
		fresh.match.spans[n].end = start < 0 ? -1 : end;
		n++;
	}
	fresh.match.nspans = n;
	fresh.valid = n > 0;
	state.match = fresh;
	return FeNil(context);
}

/* ---- string-match / regexp-quote --------------------------------------
 *
 * The regex-from-Lisp seam Phase 15 exists to open, and a seam is all it
 * is: the engine is src/regex.h's, already used by C-s and by
 * re-search-forward above.  What is new is the SUBJECT.
 *
 * Coordinates.  The engine reports BYTE spans into a NUL-terminated
 * subject (doc/coordinates.md's third space is display columns and does
 * not appear here at all).  Emacs' string-match reports 0-based CHARACTER
 * indices.  The conversion happens HERE, once, while the subject is still
 * in hand, and what is stored in state.match for a string match is
 * therefore already in characters -- which is why match-beginning/-end
 * need neither the subject nor a buffer to answer for one.  The buffer
 * search above stores byte columns and converts at the read instead,
 * because there the row is still reachable and the subject is not.
 *
 * Two properties of the engine are inherited rather than papered over,
 * and both are recorded in test/lisp-compat/features.json: the subject is
 * NUL-terminated, so matching stops at an embedded NUL; and `^'/`$'
 * anchor the whole subject rather than each line of it, because kg's
 * buffer search hands the engine one row at a time and never needed a
 * line anchor. */

/* Copy PATTERN and SUBJECT into one allocation, NUL-terminated, and park
 * it in state.scratch: the engine needs both alive at once, and a raise
 * between here and release_scratch() longjmps past every free().  Returns
 * the block; *subject_out points into it. */
static char *lisp_pattern_and_subject(FeContext *context, FeObject *pattern_obj,
    FeObject *subject_obj, size_t *pattern_len, char **subject_out,
    size_t *subject_len)
{
	size_t allocation;
	char *block;

	lisp_check_string(context, pattern_obj);
	lisp_check_string(context, subject_obj);
	*pattern_len = FeStringByteLength(context, pattern_obj);
	*subject_len = FeStringByteLength(context, subject_obj);
	if (ckd_add(&allocation, *pattern_len, *subject_len)
	    || ckd_add(&allocation, allocation, 2) || allocation > INT_MAX) {
		FeHandleError(context, "string is too large");
	}
	block = malloc(allocation);
	if (!block) {
		FeHandleError(context, "out of memory");
	}
	park_scratch(block);
	(void)FeCopyStringBytes(context, pattern_obj, block, *pattern_len);
	block[*pattern_len] = '\0';
	*subject_out = block + *pattern_len + 1;
	(void)FeCopyStringBytes(
	    context, subject_obj, *subject_out, *subject_len);
	(*subject_out)[*subject_len] = '\0';
	return block;
}

/* Compile PATTERN or raise, with the diagnostics lisp_search() uses. */
static void lisp_compile_or_raise(
    FeContext *context, struct kg_regex *rx, const char *pattern)
{
	int status = kg_regex_compile(rx, pattern, 0);
	char message[400];

	if (status == KG_REGEX_TOODEEP) {
		FeHandleError(context, "regexp too complex to compile");
	}
	if (status != KG_REGEX_OK) {
		(void)snprintf(
		    message, sizeof(message), "invalid regexp: %s", pattern);
		FeHandleError(context, message);
	}
}

/* An Emacs START index for `string-match': 0-based in characters,
 * negative counting back from the end, and past the end a range error --
 * measured, (string-match "a" "ab" 5) is (args-out-of-range "ab" 5). */
static int lisp_match_start(FeContext *context, FeObject *start_object,
    FeObject *subject_object, int chars)
{
	FeDouble value;

	if (FeIsNil(start_object)) {
		return 0;
	}
	value = lisp_finite(context, start_object, "integerp");
	if (value < 0) {
		value += (FeDouble)chars;
	}
	if (value < 0) {
		value = 0;
	}
	if (value > (FeDouble)chars) {
		lisp_raise_args_out_of_range(
		    context, subject_object, start_object);
	}
	return (int)value;
}

/* (string-match REGEXP STRING &optional START) */
FeObject *native_string_match(FeContext *context, FeObject *arguments)
{
	FeObject *pattern_object = FeGetNextArgument(context, &arguments);
	FeObject *subject_object = FeGetNextArgument(context, &arguments);
	FeObject *start_object = FeNil(context);
	size_t pattern_len, subject_len;
	char *pattern, *subject;
	struct kg_regex rx;
	struct kg_match match = { 0 };
	int chars, start, status, i;

	if (!FeIsNil(arguments)) {
		start_object = FeGetNextArgument(context, &arguments);
	}
	FeRequireNoArguments(context, arguments);
	pattern = lisp_pattern_and_subject(context, pattern_object,
	    subject_object, &pattern_len, &subject, &subject_len);
	chars = lisp_utf8_length(subject, (int)subject_len);
	start = lisp_match_start(context, start_object, subject_object, chars);
	lisp_compile_or_raise(context, &rx, pattern);
	status = kg_regex_match_forward(&rx, subject,
	    lisp_utf8_byte(subject, (int)subject_len, start), &match);
	if (status == KG_REGEX_TOO_COMPLEX) {
		FeHandleError(
		    context, "string-match: regular expression too complex");
	}
	if (status != KG_REGEX_OK) {
		release_scratch();
		return FeNil(context);
	}
	state.match = (struct kg_lisp_match_data) { 0 };
	state.match.valid = true;
	state.match.on_string = true;
	state.match.match.nspans = match.nspans;
	for (i = 0; i < match.nspans; i++) {
		int from = match.spans[i].start, to = match.spans[i].end;

		state.match.match.spans[i].start = from < 0
		    ? -1
		    : lisp_utf8_chars(subject, (int)subject_len, from);
		state.match.match.spans[i].end = to < 0
		    ? -1
		    : lisp_utf8_chars(subject, (int)subject_len, to);
	}
	release_scratch();
	return FeMakeInteger(
	    context, (int64_t)state.match.match.spans[0].start);
}

/* The characters Emacs 31.0.90's own regexp-quote escapes, measured:
 * (regexp-quote "a.*+?[]^$\\b(){}|-/") is
 * "a\\.\\*\\+\\?\\[]\\^\\$\\\\b(){}|-/", so `]' is NOT one of them -- a `]'
 * outside a bracket expression is an ordinary character in this dialect, and
 * escaping it would change nothing except the string. */
static bool lisp_regexp_special(char byte)
{
	return byte == '.' || byte == '*' || byte == '+' || byte == '?'
	    || byte == '[' || byte == '^' || byte == '$' || byte == '\\';
}

/* (regexp-quote S): S as a regexp matching itself literally. */
FeObject *native_regexp_quote(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeObject *result;
	size_t length, allocation, i, out = 0;
	char *block, *quoted;

	FeRequireNoArguments(context, arguments);
	lisp_check_string(context, object);
	length = FeStringByteLength(context, object);
	/* One allocation holding the source and its at-most-doubled
	 * rendering, the way native_string_equal() holds its two copies:
	 * one parked pointer is one thing for frame recovery to free. */
	if (ckd_mul(&allocation, length, 3)
	    || ckd_add(&allocation, allocation, 2)) {
		FeHandleError(context, "string is too large");
	}
	block = malloc(allocation);
	if (!block) {
		FeHandleError(context, "out of memory");
	}
	park_scratch(block);
	(void)FeCopyStringBytes(context, object, block, length);
	quoted = block + length;
	for (i = 0; i < length; i++) {
		if (lisp_regexp_special(block[i])) {
			quoted[out++] = '\\';
		}
		quoted[out++] = block[i];
	}
	result = FeMakeStringBytes(context, quoted, out);
	release_scratch();
	return result;
}

/* ---- looking-at -------------------------------------------------------
 *
 * (looking-at REGEXP): t when REGEXP matches the buffer text starting
 * exactly at point, setting the match data as a search does, and moving
 * point nowhere.
 *
 * It is anchored, and the engine has no anchored entry point -- but it
 * does not need one.  re_exec() reports the LEFTMOST match at or after
 * the offset it is given, so a match that starts at point is the one it
 * finds, and a reported start past point proves there is none there.
 * The spans are then the anchored match's own.
 *
 * Two inherited properties, both recorded in the manifest rather than
 * papered over: the subject is one row, so no pattern matches across a
 * line break the way Emacs' does; and `^'/`$' anchor that row, which is
 * exactly beginning- and end-of-line here and is therefore the one place
 * the row-at-a-time architecture happens to agree with Emacs. */
FeObject *native_looking_at(FeContext *context, FeObject *arguments)
{
	FeObject *pattern_object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	struct kg_match match = { 0 };
	struct kg_regex rx;
	char *pattern;
	size_t pattern_len;
	const char *subject = "";
	int row = 0, col = 0, status;

	lisp_check_string(context, pattern_object);
	pattern = copy_fe_string(context, pattern_object, &pattern_len);
	park_scratch(pattern);
	FeRequireNoArguments(context, arguments);
	lisp_compile_or_raise(context, &rx, pattern);
	release_scratch();
	/* An empty buffer has no row to hand the engine, and an empty
	 * subject is exactly what point is looking at there. */
	if (b->numrows > 0) {
		buffer_position_to_row_col(
		    b, lisp_exec_point_byte(context), &row, &col);
		subject = b->row[row].chars;
	}
	status = kg_regex_match_forward(&rx, subject, col, &match);
	if (status == KG_REGEX_TOO_COMPLEX) {
		FeHandleError(
		    context, "looking-at: regular expression too complex");
	}
	if (status != KG_REGEX_OK || match.spans[0].start != col) {
		return FeMakeBool(context, false);
	}
	state.match = (struct kg_lisp_match_data) { 0 };
	state.match.valid = true;
	state.match.on_string = false;
	state.match.buffer = state.exec.buffer;
	state.match.row = row;
	state.match.match = match;
	return FeMakeBool(context, true);
}
