/* compile_nav.h — navigable compilation diagnostics: next-error/previous-error
 *
 * compile.c streams compiler output through src/compile_parse.h's pure
 * parser and knows nothing about markers, decorations, buffers or point.
 * This module is what turns its parsed diagnostics into something a user
 * can jump to: it installs itself into compile.c's optional hook (see
 * struct compile_diag_hooks in compile.h), and on every committed output
 * line, records file/line/column plus a marker and a decoration anchored
 * inside *compilation*.
 *
 * Nothing in compile.c names a symbol from this file -- compile_nav_install()
 * is the one call that wires the two together, made once from init_editor().
 * A caller that never makes it (test/test_compile.c's lightweight streaming
 * tests) still links and runs; output is simply not tracked.
 *
 * Recompilation invalidation: a fresh *compilation* buffer -- which
 * buf_prepare_special_text() gives every compile, including a recompile --
 * already detaches every marker and decoration that pointed into the old
 * one (kg_buffer_adopt_rows(), see buffer.c).  compile_nav_reset() rides
 * that: it discards the previous run's records (releasing each one's lazily
 * created *source*-buffer marker, which lives in a different buffer that
 * adoption does not touch) and starts a new generation, which is also what
 * invalidates the cycle cursor below -- a cursor from an earlier generation
 * is treated as never having visited anything.
 *
 * The cycle cursor next-error/previous-error share is not a field of the
 * record store: compile_nav_cursor_advance() is a pure function of one, so
 * the store says what a compile produced and the cursor says where the user
 * is in it, kept apart on purpose (a later slice, e.g. occur, will want a
 * second cursor over a different store). */

#ifndef KG_COMPILE_NAV_H
#define KG_COMPILE_NAV_H

#include "bufhandle.h"
#include "compile_parse.h"
#include <stdbool.h>
#include <stdint.h>

/* Install this module's hooks into compile.c.  Called once, from
 * init_editor(). */
void compile_nav_install(void);

/* M-g n / M-g p, bound through the keymap only (see src/kbd.c). */
void editor_next_error(int fd);
void editor_previous_error(int fd);

/* ---- The record store's two hook implementations ----
 *
 * These match struct compile_diag_hooks exactly, which is what lets
 * compile_nav_install() hand them to compile.c without a wrapper.  They are
 * ordinary public functions rather than static ones behind the hook only
 * because the native test drives them directly, the same seam compile.c's
 * hook uses, without needing a real child process. */
void compile_nav_reset(struct kg_buffer_handle compilation_buffer,
    const char *initial_cwd, size_t initial_cwd_len);
void compile_nav_ingest_line(
    const char *line, size_t len, size_t line_start_pos);

/* ---- The pure cycle-cursor step ---- */

struct compile_nav_cursor {
	int index; /* -1 = nothing visited yet this run */
	uint32_t generation; /* which run's diagnostics `index` counts into */
};

/* The cursor after moving `step` diagnostics (positive = forward/next-error,
 * negative = backward/previous-error) through `count` records currently at
 * `current_generation`.  A cursor from an earlier generation -- a compile
 * happened since it was last used -- is treated the same as one that has
 * never visited anything, which is this module's whole recompilation-
 * invalidation policy for the cursor (the record store's own is
 * compile_nav_reset()'s, above).  The first move from "never visited"
 * lands on the first record going forward, the last one going backward.
 * `*clamped` is set when `step` ran past either end -- next-error/
 * previous-error turn that into a status message instead of a further
 * move -- and, degenerately, whenever `count` is 0, which always reports
 * index -1. */
struct compile_nav_cursor compile_nav_cursor_advance(
    struct compile_nav_cursor cursor, uint32_t current_generation, size_t count,
    int step, bool *clamped);

/* ---- Test-only introspection of the record store ----
 *
 * Nothing outside test/ calls these: they let the native suite read back
 * what compile_nav_reset()/compile_nav_ingest_line() built without needing
 * a running editor or a real child process. */
size_t compile_nav_test_count(void);
bool compile_nav_test_diag(size_t index, struct compile_diag *out);
bool compile_nav_test_line_marker_pos(size_t index, size_t *out_pos);

#endif /* KG_COMPILE_NAV_H */
