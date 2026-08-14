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

#include "announce.h"
#include "lsp_server.h"
#include "lsp_sync.h"

/* src/lsp.h spells the endpoint's two bounds as numbers so it can stay
 * free-standing; this is where they are held to the announce scanner's own,
 * since the copy out of a cached announce is a memcpy into these fields. */
static_assert(KG_LSP_SIBLING_HOST_MAX == KG_ANNOUNCE_MAX_HOST_BYTES,
    "the facade's host bound must be the announce scanner's");
static_assert(KG_LSP_SIBLING_SECRET_MAX == KG_ANNOUNCE_MAX_SECRET_BYTES,
    "the facade's secret bound must be the announce scanner's");

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

enum kg_lsp_sibling_status lsp_sibling_endpoint(const char *language,
    const char *abs_path, enum kg_lsp_sibling_intent intent,
    struct kg_lsp_sibling_endpoint *out)
{
	return lsp_server_sibling_endpoint(language, abs_path, intent, out);
}

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

/* No client, so no server, so no sibling -- and saying which of those it is
 * matters: the debugger's Java adapter reports "this kg was built without
 * the language server" rather than "no Java server is running", which is
 * the difference between a build to change and a server to start. */
enum kg_lsp_sibling_status lsp_sibling_endpoint(const char *language,
    const char *abs_path, enum kg_lsp_sibling_intent intent,
    struct kg_lsp_sibling_endpoint *out)
{
	(void)language;
	(void)abs_path;
	(void)intent;
	(void)out;
	return KG_LSP_SIBLING_NOT_BUILT;
}

#endif /* KG_USE_LSP */

/* Compiled in both halves, because both answer with these codes. */
const char *lsp_sibling_status_text(enum kg_lsp_sibling_status status)
{
	switch (status) {
	case KG_LSP_SIBLING_OK:
		return "ok";
	case KG_LSP_SIBLING_NOT_BUILT:
		return "this kg was built without the language server";
	case KG_LSP_SIBLING_UNSUPPORTED:
		return "this language server announces no debug adapter";
	case KG_LSP_SIBLING_NOT_RUNNING:
		return "the language server could not be started";
	case KG_LSP_SIBLING_STARTING:
		return "waiting for the language server to initialize";
	case KG_LSP_SIBLING_NONE:
		return "the language server has not announced its debug "
		       "adapter";
	case KG_LSP_SIBLING_DEAD:
		return "the language server died";
	}
	return "unknown language server error";
}
