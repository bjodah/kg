/* Lifecycle of the optional LSP client, and the home of the facade
 * src/lsp.h declares.  Compiled in every configuration, like
 * src/lisp_core.c: the WITH_LSP=0 build links these entry points too, so
 * the editor calls them unconditionally.
 *
 * Stage 3 of doc/plans/2026-08-08-lsp.md is where the halves start to
 * differ, as Stage 0 said they would.  The KG_USE_LSP half now has a
 * registry to poll and instances to shut down; the other half is the same
 * three no-ops it has always been, and the editor cannot tell which one it
 * linked.
 */

#include "lsp.h"

#ifdef KG_USE_LSP

#include "lsp_server.h"

/* The whole budget for winding every running server down, split between
 * them by the registry.  Three tenths of a second is chosen against what
 * the user experiences: it is under the threshold at which quitting an
 * editor feels like it hung, and it is far more than a `shutdown`/`exit`
 * exchange with a healthy server takes -- clangd answers in single-digit
 * milliseconds.  A server that wants longer than this does not get it: kg
 * is exiting, and the transport's SIGKILL is the backstop. */
#define LSP_SHUTDOWN_GRACE_MS 300u

/* Nothing to do.  Servers are started lazily by the first command that
 * needs one, so an editor session that never runs an xref command never
 * spawns anything -- which is why there is no configuration to read here
 * and no table to build. */
void lsp_init(void) { }

void lsp_shutdown(void) { lsp_server_shutdown_all(LSP_SHUTDOWN_GRACE_MS); }

int lsp_poll(void) { return lsp_server_poll_all(); }

#else /* !KG_USE_LSP */

void lsp_init(void) { }

void lsp_shutdown(void) { }

int lsp_poll(void) { return 0; }

#endif /* KG_USE_LSP */
