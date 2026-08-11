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
#include "lsp_sync.h"

/* The whole budget for winding every running server down, split between
 * them by the registry.  Three tenths of a second is chosen against what
 * the user experiences: it is under the threshold at which quitting an
 * editor feels like it hung, and it is far more than a `shutdown`/`exit`
 * exchange with a healthy server takes -- clangd answers in single-digit
 * milliseconds.  A server that wants longer than this does not get it: kg
 * is exiting, and the transport's SIGKILL is the backstop. */
#define LSP_SHUTDOWN_GRACE_MS 300u

/* Nothing is started here.  Servers are spawned lazily by the first command
 * that needs one, so an editor session that never runs an xref command
 * never spawns anything -- which is why there is no configuration to read
 * and no table to build.  The one thing that must happen once is the wiring
 * between the registry and the document sync: the registry tells whoever is
 * listening that it is about to dispose of a client, and the sync layer is
 * what listens, so that a reclaimed instance leaves no shadow behind
 * pointing at a freed client (src/lsp_sync.h). */
void lsp_init(void) { lsp_sync_install(); }

void lsp_shutdown(void) { lsp_server_shutdown_all(LSP_SHUTDOWN_GRACE_MS); }

int lsp_poll(void) { return lsp_server_poll_all(); }

int lsp_wait_fds(int *fds, int max) { return lsp_server_wait_fds(fds, max); }

#else /* !KG_USE_LSP */

void lsp_init(void) { }

void lsp_shutdown(void) { }

int lsp_poll(void) { return 0; }

int lsp_wait_fds(int *fds, int max)
{
	(void)fds;
	(void)max;
	return 0;
}

#endif /* KG_USE_LSP */
