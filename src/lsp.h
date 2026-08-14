#ifndef KG_LSP_H
#define KG_LSP_H

#include <stddef.h>
#include <stdint.h>

/* The one editor-facing facade of the optional LSP client
 * (doc/plans/2026-08-08-lsp.md).  Everything below exists in both build
 * configurations -- src/lsp_core.c is compiled whatever WITH_LSP says, the
 * way src/lisp_core.c is -- so no caller carries a KG_USE_LSP conditional,
 * and a WITH_LSP=0 editor simply calls entry points that do nothing.
 *
 * Stage 0 is the axis and this facade, and nothing else: the whole client
 * is inert in both configurations.  The protocol modules (transport, JSON,
 * client, server registry, document sync) land in later stages behind these
 * same three functions, plus the request entry points xref needs -- which
 * are deliberately not guessed at here.
 *
 * This header names no editor type on purpose, so it stays free-standing
 * (`make header-check`).
 */

/* Called once from init_editor(), and once from editor_cleanup() beside the
 * other subsystem shutdowns.  Neither starts or stops a server: instances
 * are spawned lazily by the first command that needs one, and shutdown is
 * what gives the running ones a chance to exit gracefully. */
void lsp_init(void);
void lsp_shutdown(void);

/* Services the client's children from the two poll sites (the main loop and
 * tty.c's idle path), reaping and reading whatever is ready.  Returns
 * nonzero when that changed something the user could see, i.e. when a
 * repaint is wanted -- compilation_poll()'s convention, and read the same
 * way by the idle loop. */
int lsp_poll(void);

/* How many descriptors lsp_wait_fds() may write: every instance the
 * registry holds times every descriptor a transport waits on
 * (LSP_SERVER_MAX_INSTANCES x LSP_TRANSPORT_WAIT_FDS_MAX, which
 * src/lsp_server.c asserts against at compile time).  Spelled as a
 * number here rather than as those two names because this header stays
 * free-standing, and because it is what a caller sizes an array with. */
#define KG_LSP_WAIT_FDS_MAX 12

/* The descriptors the editor's idle wait should include, so that the next
 * lsp_poll() happens when a server writes rather than when the idle tick
 * comes round; at most `max` are written to `fds`, and the count is the
 * return value.  Zero from the WITH_LSP=0 half, and zero whenever nothing
 * is running, which is a wait with only the terminal in it -- exactly what
 * the editor waited on before this existed. */
int lsp_wait_fds(int *fds, int max);

/* ------------------------- the sibling endpoint ----------------------- */

/* Some language servers announce a SECOND server beside themselves, on the
 * same process and the same standard output.  Oracle's nbcode is the one
 * kg has: started with both of its listen-hash options it announces a Java
 * language server and a Java Debug Server Adapter, and the second is what
 * kg's Java debugging talks to (doc/plans/dap/03-java.md).
 *
 * This is the whole of the seam between the two protocol clients, and it is
 * deliberately a narrow one.  It exists here, on the LSP facade, so that:
 *
 *   - the debugger asks about a LANGUAGE and a FILE and gets back an
 *     address, a secret and a generation.  No LSP method name, client
 *     handle or transport type reaches src/dap_session.c, and the debugger
 *     never owns, signals or reaps the child -- the language server does,
 *     and disconnecting a debug session must leave it running, since it is
 *     the user's Java server;
 *   - a WITH_LSP=0 build answers the same question with the same
 *     vocabulary (KG_LSP_SIBLING_NOT_BUILT), so the debugger reports a
 *     clean "not available here" rather than failing to link.
 *
 * The generation is what makes a cached endpoint safe.  It identifies the
 * child that announced it: a server that died and was restarted announces
 * a new port under a new generation, and a debugger holding the old one
 * compares generations rather than reconnecting to a port that may now
 * belong to somebody else.  Every status but OK leaves `out` untouched. */
enum kg_lsp_sibling_status {
	KG_LSP_SIBLING_OK = 0,
	/* This kg has no LSP client compiled in at all. */
	KG_LSP_SIBLING_NOT_BUILT,
	/* No language of that name, or its server announces no sibling --
	 * which is what a Java buffer whose KG_LSP_SERVER_JAVA points at
	 * jdt.ls looks like. */
	KG_LSP_SIBLING_UNSUPPORTED,
	/* Nothing is running for that file's workspace root and nothing
	 * could be started. */
	KG_LSP_SIBLING_NOT_RUNNING,
	/* A server is running but has not finished initializing.  The
	 * ordering rule is not negotiable: the LSP session must be up and
	 * `initialized` before the debug socket is connected, because the
	 * debug connection captures the language session's state when it is
	 * constructed.  Ask again. */
	KG_LSP_SIBLING_STARTING,
	/* Initialized, and no sibling announce has arrived yet.  Ask
	 * again; the announce is not ordered against the handshake. */
	KG_LSP_SIBLING_NONE,
	/* The server that announced it has died, so the endpoint is
	 * invalid.  A session holding one is dead too. */
	KG_LSP_SIBLING_DEAD,
};

/* Bounds, spelled as numbers so this header stays free-standing (the
 * KG_LSP_WAIT_FDS_MAX rule above).  src/lsp_core.c holds them to
 * src/announce.h's own with a static_assert. */
#define KG_LSP_SIBLING_HOST_MAX 64u
#define KG_LSP_SIBLING_SECRET_MAX 256u

struct kg_lsp_sibling_endpoint {
	char host[KG_LSP_SIBLING_HOST_MAX];
	unsigned short port;
	/* The handshake secret, copied out rather than pointed at, and NOT a
	 * string: it is bytes with a length.  Nothing that formats a
	 * user-visible message may be handed this -- the announce channel
	 * redacts it on the way to *lsp-log*, and a caller that logs it
	 * undoes that. */
	unsigned char secret[KG_LSP_SIBLING_SECRET_MAX];
	size_t secret_len;
	uint64_t generation;
};

/* Whether the caller wants a server started for it.
 *
 * KG_LSP_SIBLING_START is the debugger beginning a session: its first step
 * in a Java buffer is to need that server, and a user who has not yet run
 * an xref command in this project should not have to.
 *
 * KG_LSP_SIBLING_EXISTING is the same caller CHECKING on the owner of a
 * session it already has, and it is a different question with a different
 * right answer: a server that has died must be reported dead, not
 * resurrected under a session whose endpoint belonged to the corpse. */
enum kg_lsp_sibling_intent {
	KG_LSP_SIBLING_START = 0,
	KG_LSP_SIBLING_EXISTING,
};

/* Ask for `language`'s sibling endpoint for the workspace `abs_path`
 * belongs to.
 *
 * It never blocks.  A server that is still initializing answers STARTING,
 * and the caller asks again on its next poll. */
enum kg_lsp_sibling_status lsp_sibling_endpoint(const char *language,
    const char *abs_path, enum kg_lsp_sibling_intent intent,
    struct kg_lsp_sibling_endpoint *out);

/* One line naming what a status means, for a status line or an error.
 * Never NULL. */
const char *lsp_sibling_status_text(enum kg_lsp_sibling_status status);

#endif /* KG_LSP_H */
