#ifndef KG_FILELINE_H
#define KG_FILELINE_H

#include <stddef.h>

/* "What does line N of that file say?" -- the one thing a results listing
 * needs about a place it is only naming, and the reason it is its own
 * module rather than a corner of src/xref.c.
 *
 * A listing shows results from files the editor has no business opening:
 * two hundred references would be two hundred buffers, and the buffer
 * table holds far fewer.  But a file that *is* open is the only correct
 * source for its own text -- an unsaved edit is what the user sees, and a
 * preview read from disk beside it would be a listing that disagrees with
 * the buffer RET lands in.  So the answer comes from an open buffer when
 * there is one and from a bounded read otherwise, and both are bounded:
 * nothing here allocates, opens a buffer, or reads a whole file into
 * memory.
 *
 * src/visit.h is the neighbour: that one shows a place, this one describes
 * one.  Occur- and grep-style listings want exactly this text.
 */

/* Longest preview in bytes.  A minified line is a single row of tens of
 * thousands of bytes, and a listing row wider than a screen is worth no
 * more than one that fits: this is the constraint that keeps a *xref* line
 * bounded whatever the file holds.  Two hundred is past any terminal a
 * listing is read on and short enough that two hundred of them are a
 * rounding error.  A cut may land inside a UTF-8 sequence, which costs one
 * replacement glyph on screen -- display_glyph_at() decides that, as it
 * does for every other byte kg did not write. */
#define KG_LINE_PREVIEW_MAX 200

/* Line `line` of `path` -- 1-based, counting the way an editor does --
 * trimmed for display into `out`, and the number of bytes written.
 *
 * Zero, with `out` left an empty string, is the whole of "there is no
 * preview": no such path, a path that cannot be read, a line past the end
 * of the file, a line that is blank, and a line too far into a file to
 * reach within the read budget.  A caller prints what it would have
 * printed anyway.
 *
 * What comes back is one displayable line and never more: leading blanks
 * are dropped (the indentation of a result says nothing about the result),
 * trailing blanks with them, and every control byte -- tab, CR, NUL, the
 * newline that would otherwise split one result across two listing rows --
 * becomes a space.  A row of a listing indexed by row number depends on
 * that last property.
 *
 * `out_size` bounds the answer; KG_LINE_PREVIEW_MAX + 1 is the size that
 * makes the cap this module's rather than the caller's. */
size_t kg_file_line_preview(
    const char *path, int line, char *out, size_t out_size);

#endif /* KG_FILELINE_H */
