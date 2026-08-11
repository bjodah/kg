#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "../fe/fe.h"
#include "def.h"
#include "lisp_internal.h"
#include "marker.h"

/* ---- Line and character motion, and character-set skipping -----------
 *
 * Emacs' motion primitives, in kg's own coordinate spaces.  Two rules run
 * through everything here:
 *
 *   * a position is a 1-based CODEPOINT offset on the Lisp side and a
 *     (row, byte column) pair on the editor side, and lisp_buffer.c owns
 *     the conversion.  Nothing below invents a third space;
 *   * a motion that runs out of buffer CLAMPS.  Emacs signals
 *     `beginning-of-buffer'/`end-of-buffer' there, which kg cannot: fe's
 *     `signal' is gated on its own condition table (fe_eval.c's
 *     condition_parents[]) and neither name is in it, so raising one would
 *     be an fe language change rather than a kg native.  Clamping is what
 *     kg's existing `forward-word' already does, so the whole family
 *     answers the same way; the divergence is recorded per native in
 *     test/lisp-compat/features.json and the fe-side work in doc/TODO.md.
 *
 * The RETURN VALUES are Emacs', measured on 31.0.90, and they are not
 * uniform: `forward-line' answers the shortfall, `skip-chars-forward' the
 * signed distance travelled, and the rest answer nil. */

/* Row `point` sits on, and the row count, for the exec buffer. */
static int lisp_point_row(FeContext *context, const struct editor_buffer *b)
{
	int row, col;

	buffer_position_to_row_col(
	    b, lisp_exec_point_byte(context), &row, &col);
	return row;
}

/* Move point to the start of `row`, which must be a valid row index. */
static void lisp_goto_row_start(const struct editor_buffer *b, int row)
{
	(void)kg_marker_set_position(
	    lisp_exec_point_marker(), buffer_row_col_to_position(b, row, 0));
}

/* (forward-line &optional N): N lines forward, to the beginning of a
 * line, answering how many of N were not travelled.
 *
 * Emacs' shortfall rule, measured: going forward it is N minus the
 * newlines crossed, minus one more when the motion stopped at end of
 * buffer on a line that has no newline of its own -- so a three-line
 * buffer with no final newline answers 2, not 3, for (forward-line 5)
 * from its first line.  Going backward it is N plus the lines travelled.
 * An empty buffer travels nothing and answers N either way. */
FeObject *native_forward_line(FeContext *context, FeObject *arguments)
{
	long count = lisp_optional_count(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	int row, target;

	FeRequireNoArguments(context, arguments);
	if (b->numrows <= 0) {
		return FeMakeInteger(context, (int64_t)count);
	}
	row = lisp_point_row(context, b);
	if (count >= 0 && count > (long)(b->numrows - 1 - row)) {
		int last = b->numrows - 1;
		long shortfall = count - (last - row);

		/* End of buffer, not the start of a line, unless the last row
		 * is empty -- which is exactly how a buffer with a final
		 * newline is spelled here. */
		(void)kg_marker_set_position(lisp_exec_point_marker(),
		    buffer_row_col_to_position(b, last, b->row[last].size));
		if (b->row[last].size > 0) {
			shortfall--;
		}
		return FeMakeInteger(context, (int64_t)shortfall);
	}
	if (count < 0 && -count > (long)row) {
		lisp_goto_row_start(b, 0);
		return FeMakeInteger(context, (int64_t)(count + row));
	}
	target = (int)(row + count);
	lisp_goto_row_start(b, target);
	return FeMakeInteger(context, 0);
}

/* (forward-char &optional N) / (backward-char &optional N): N codepoints,
 * answering nil.
 *
 * A count that runs past an end moves as far as it can and THEN signals
 * `end-of-buffer' or `beginning-of-buffer' -- Emacs' own order, measured
 * on the pinned 31.0.90: `(list (condition-case e (forward-char 20)
 * (error e)) (point))' in a three-character buffer is `((end-of-buffer)
 * 4)', so the point move is not undone by the raise.  Landing exactly ON
 * an end is not a signal; only a count that could not be spent is.  The
 * condition names the end that was reached rather than the function that
 * was called, which is why a negative count to `forward-char' can raise
 * `beginning-of-buffer'.
 *
 * These clamped silently until the Phase 20 fe pin put both names in fe's
 * condition table; `forward-word' and `forward-line' still clamp, and so
 * does Emacs. */
static FeObject *lisp_move_chars(
    FeContext *context, FeObject *arguments, long sign)
{
	long count = lisp_optional_count(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	long here = lisp_exec_point_char(context);
	long max = lisp_buffer_char_length(b);
	long target;
	bool past_end;

	FeRequireNoArguments(context, arguments);
	/* The count is already bounded to +-INT_MAX by lisp_optional_count,
	 * so the sum cannot overflow a long on any platform kg builds on. */
	target = here + sign * count;
	past_end = target > max;
	if (target < 0 || past_end) {
		lisp_exec_goto_char(b, past_end ? max : 0);
		lisp_raise_buffer_edge(context, past_end);
	}
	lisp_exec_goto_char(b, target);
	return FeNil(context);
}

FeObject *native_forward_char(FeContext *context, FeObject *arguments)
{
	return lisp_move_chars(context, arguments, 1);
}

FeObject *native_backward_char(FeContext *context, FeObject *arguments)
{
	return lisp_move_chars(context, arguments, -1);
}

/* The row (beginning-of-line N) and (end-of-line N) name: N counts from
 * the current line, so N = 1 is this line, 2 the next and 0 the previous,
 * clamped to the buffer. */
static int lisp_line_target_row(
    FeContext *context, const struct editor_buffer *b, FeObject **arguments)
{
	long count = lisp_optional_count(context, arguments);
	long target = lisp_point_row(context, b) + count - 1;

	if (target < 0) {
		return 0;
	}
	if (target > b->numrows - 1) {
		return b->numrows - 1;
	}
	return (int)target;
}

/* (beginning-of-line &optional N).  Emacs has both this and
 * `move-beginning-of-line'; kg had only the second, which is the command
 * C-a runs and takes no count.  This is the one package code calls. */
FeObject *native_beginning_of_line(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);
	int target;

	if (b->numrows <= 0) {
		(void)lisp_optional_count(context, &arguments);
		FeRequireNoArguments(context, arguments);
		return FeNil(context);
	}
	target = lisp_line_target_row(context, b, &arguments);
	FeRequireNoArguments(context, arguments);
	lisp_goto_row_start(b, target);
	return FeNil(context);
}

/* (end-of-line &optional N): the line's last character, before its
 * newline -- which is the row's own end here, row separators not being
 * part of any row. */
FeObject *native_end_of_line(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);
	int target;

	if (b->numrows <= 0) {
		(void)lisp_optional_count(context, &arguments);
		FeRequireNoArguments(context, arguments);
		return FeNil(context);
	}
	target = lisp_line_target_row(context, b, &arguments);
	FeRequireNoArguments(context, arguments);
	(void)kg_marker_set_position(lisp_exec_point_marker(),
	    buffer_row_col_to_position(b, target, b->row[target].size));
	return FeNil(context);
}

/* ---- skip-chars-forward / -backward -----------------------------------
 *
 * The SPEC is Emacs' character-set syntax, minus one thing: `[:alpha:]'
 * and the rest of the named classes are not recognised and are read as
 * the ordinary characters they are made of.  Recorded rather than
 * approximated, because a half-populated class table would be worse than
 * a missing one.
 *
 * What is here is measured against 31.0.90: a leading `^' negates (and a
 * bare "^" therefore matches everything, while "" matches nothing), `-'
 * between two characters is a range, a backslash quotes the next
 * character, and the comparison is over CODEPOINTS, so a spec naming a
 * multi-byte character skips whole glyphs. */

static constexpr int lisp_skip_ranges_max = 64;

struct lisp_skip_set {
	bool negated;
	int count;
	struct {
		long from, to;
	} ranges[lisp_skip_ranges_max];
};

/* Next codepoint of SPEC, honouring a backslash quote; advances *i. */
static long lisp_skip_next_char(const char *spec, int length, int *i)
{
	long value;

	if (spec[*i] == '\\' && *i + 1 < length) {
		(*i)++;
	}
	value = lisp_decode_char(spec, length, *i);
	*i += utf8_glyph_span_at(spec, length, *i);
	return value;
}

/* Parse SPEC into `set`, raising when it names more ranges than kg holds
 * -- a bound, not a silent truncation. */
static void lisp_skip_parse(
    FeContext *context, const char *spec, int length, struct lisp_skip_set *set)
{
	int i = 0;

	set->negated = false;
	set->count = 0;
	if (length > 0 && spec[0] == '^') {
		set->negated = true;
		i = 1;
	}
	while (i < length) {
		long from = lisp_skip_next_char(spec, length, &i);
		long to = from;

		if (i < length && spec[i] == '-' && i + 1 < length) {
			i++;
			to = lisp_skip_next_char(spec, length, &i);
		}
		if (set->count >= lisp_skip_ranges_max) {
			FeHandleError(context, "skip-chars: set is too large");
		}
		set->ranges[set->count].from = from;
		set->ranges[set->count].to = to;
		set->count++;
	}
}

static bool lisp_skip_member(const struct lisp_skip_set *set, long value)
{
	int i;

	for (i = 0; i < set->count; i++) {
		if (value >= set->ranges[i].from
		    && value <= set->ranges[i].to) {
			return !set->negated;
		}
	}
	return set->negated;
}

/* The codepoint at 0-based offset `off`, with a row separator reading as
 * '\n'.  `off` is inside the buffer. */
static long lisp_char_at_offset(const struct editor_buffer *b, long off)
{
	int row, col;

	lisp_rowcol_of_char_offset(b, off, &row, &col);
	if (col >= b->row[row].size) {
		return '\n';
	}
	return lisp_decode_char(b->row[row].chars, b->row[row].size, col);
}

/* The optional LIM argument, as a 0-based offset, defaulting to `fallback`. */
static long lisp_skip_limit(FeContext *context, const struct editor_buffer *b,
    FeObject **arguments, long fallback)
{
	FeObject *object;

	if (FeIsNil(*arguments)) {
		return fallback;
	}
	object = FeGetNextArgument(context, arguments);
	if (FeIsNil(object)) {
		return fallback;
	}
	return lisp_offset_argument(context, b, object);
}

static FeObject *lisp_skip_chars(
    FeContext *context, FeObject *arguments, bool forward)
{
	FeObject *spec_object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	struct lisp_skip_set set;
	long here, limit, moved = 0;
	size_t spec_length;
	char *spec;

	lisp_check_string(context, spec_object);
	spec = copy_fe_string(context, spec_object, &spec_length);
	/* Parked, because the parse and the argument checks below both
	 * raise and a raise longjmps past every free() here. */
	state.scratch = spec;
	if (spec_length > INT_MAX) {
		FeHandleError(context, "skip-chars: set is too large");
	}
	lisp_skip_parse(context, spec, (int)spec_length, &set);
	here = lisp_exec_point_char(context);
	limit = lisp_skip_limit(
	    context, b, &arguments, forward ? lisp_buffer_char_length(b) : 0);
	FeRequireNoArguments(context, arguments);
	release_scratch();

	if (forward) {
		while (here + moved < limit
		    && lisp_skip_member(
			&set, lisp_char_at_offset(b, here + moved))) {
			moved++;
		}
	} else {
		while (here + moved > limit
		    && lisp_skip_member(
			&set, lisp_char_at_offset(b, here + moved - 1))) {
			moved--;
		}
	}
	lisp_exec_goto_char(b, here + moved);
	return FeMakeInteger(context, (int64_t)moved);
}

FeObject *native_skip_chars_forward(FeContext *context, FeObject *arguments)
{
	return lisp_skip_chars(context, arguments, true);
}

FeObject *native_skip_chars_backward(FeContext *context, FeObject *arguments)
{
	return lisp_skip_chars(context, arguments, false);
}
