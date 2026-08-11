#ifndef KG_LSP_H
#define KG_LSP_H

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

#endif /* KG_LSP_H */
