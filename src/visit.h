#ifndef KG_VISIT_H
#define KG_VISIT_H

#include <stdbool.h>

#include "bufhandle.h"

/* "Show me that file, at that place" -- the one navigation primitive every
 * results buffer needs, and the reason it is its own module rather than a
 * corner of one of them.
 *
 * It grew inside src/compile_nav.c, where next-error had to open a
 * diagnostic's source; xref needs exactly the same thing for a definition,
 * and a second copy of "reuse the buffer if it is open, refuse a directory,
 * refuse a file that is not there, then place point" is how the two would
 * drift apart in the details that matter -- which of them opens an empty
 * buffer for a missing file, which of them leaves the results list visible.
 *
 * Nothing here knows what a diagnostic or a definition is.  The results
 * buffer a visit should get out of the way of is a parameter, not a global.
 */

/* Move focus out of `escape_from`'s window before a visit, so a results
 * buffer stays visible while its result is displayed elsewhere.  Only when
 * there are at least two windows AND the current one is showing that
 * buffer; otherwise a visit reuses whatever window the user is already in,
 * like any other buffer switch.  Picks the next window in the C-x o cycle
 * rather than hunting for one already showing the target -- a deliberately
 * simple rule, not an attempt to reproduce display-buffer's window reuse.
 *
 * A zeroed handle names no buffer and therefore matches no window, which is
 * how a caller with no results buffer to escape says so.
 *
 * Public because compile_nav.c's marker-based revisit path takes the same
 * decision without going through editor_visit_file_position(): it already
 * knows the buffer, so it selects it directly. */
void editor_visit_escape_window(struct kg_buffer_handle escape_from);

/* Open (or reuse) `path`'s buffer and put point at `line`:`col` -- both
 * 1-based, `col` counted in bytes of the row's chars, which is
 * doc/coordinates.md's **chars** space and what editor_goto_line_direct()
 * takes.
 *
 * `who` is the command's name, and prefixes every message this reports:
 * a path that stat() cannot see, a path that is a directory, and a buffer
 * table with no room are all refused and said out loud rather than handed
 * to buf_open_path(), which would happily start a new empty buffer for a
 * file that does not exist.
 *
 * Returns true only when the visit happened: the named file is the current
 * buffer and point is where it was asked to be. */
bool editor_visit_file_position(const char *who, const char *path, int line,
    int col, struct kg_buffer_handle escape_from);

#endif /* KG_VISIT_H */
