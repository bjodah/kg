/* Lifecycle of the optional LSP client, and the home of the facade
 * src/lsp.h declares.  Compiled in every configuration, like
 * src/lisp_core.c: the WITH_LSP=0 build links these entry points too, so
 * the editor calls them unconditionally.
 *
 * Stage 0 of doc/plans/2026-08-08-lsp.md is the build axis and the facade,
 * with no protocol code behind it, so the file has no KG_USE_LSP split yet
 * -- both configurations want exactly these no-ops.  The split arrives with
 * the client (Stage 3), when the WITH_LSP=1 half gains a server registry to
 * initialize, children to poll and instances to shut down, and this half
 * stays what it is.
 */

#include "lsp.h"

void lsp_init(void) { }

void lsp_shutdown(void) { }

int lsp_poll(void) { return 0; }
