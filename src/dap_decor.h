#ifndef KG_DAP_DECOR_H
#define KG_DAP_DECOR_H

#include <stddef.h>

/* The debugger's marks in ordinary source buffers: a breakpoint, a
 * breakpoint the adapter has not verified, and the line the program is
 * stopped on.
 *
 * THESE ARE PROJECTIONS AND NOTHING ELSE.  The breakpoint table
 * (src/dap_breakpoint.h) and the stop model (src/dap_exec.h) are the source
 * of truth; this module owns no state a user could lose, only the decoration
 * handles it made last time.  So the whole of its logic is "delete what I
 * made, make it again from the tables", called whenever either table can
 * have changed and whenever a buffer appears or goes.  A projection that
 * tried to be incremental would be a second copy of the table, which is the
 * bug this shape cannot have.
 *
 * A breakpoint marks the FIRST CHARACTER of its line rather than the whole
 * of it: kg has no fringe and no margin, so column one is the gutter, and a
 * whole line in breakpoint colour would drown the code it is about.  The
 * stopped line is the one exception -- it covers the line, because "where
 * am I" is the question a debugger exists to answer -- and it is given the
 * lower priority of the two, so a breakpoint on the stopped line is still
 * visible in column one.
 */

/* How many decorations one projection may make.  Past this the marks are
 * dropped rather than the buffer being filled: the tables still hold every
 * breakpoint, and the bound is on what is PAINTED. */
#define DAP_DECOR_MAX 512

/* Subscribe to the buffer lifecycle.  Called from dap_init(), and by a test
 * that wants the wiring back after a kg_event_queue_init(); idempotent. */
void dap_decor_init(void);

/* Delete every decoration this module made and unsubscribe.  Editor
 * shutdown, and every test's setup. */
void dap_decor_reset(void);

/* Rebuild.  Cheap enough to call on every visible change: it is bounded by
 * the breakpoint table and by the buffers that are actually open. */
void dap_decor_refresh(void);

/* How many decorations the last projection made, for the tests. */
size_t dap_decor_count(void);

#endif /* KG_DAP_DECOR_H */
