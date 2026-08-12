#ifndef KG_DAP_H
#define KG_DAP_H

/* The one editor-facing facade of the optional debugger client
 * (doc/plans/2026-08-11-dap.md).  Everything below exists in both build
 * configurations -- src/dap_core.c and src/dap_keymap.c are compiled
 * whatever WITH_DAP says, the way src/lsp_core.c is -- so no caller carries
 * a KG_USE_DAP conditional, and a WITH_DAP=0 editor simply calls entry
 * points that do nothing.
 *
 * What is behind it so far is the axis, the three keymaps and, since stages
 * 2 and 3, src/dap_transport.c and src/dap_client.c -- which no editor
 * module reaches and this header does not name: a client is created by
 * whoever wants an adapter, and nothing an editor can reach wants one yet,
 * so these four functions still have nothing to do.  The remaining protocol
 * modules (config, session, breakpoints) land in later stages behind these
 * same four functions; their headers are included by their real C
 * consumers, never from here.
 *
 * When commands do arrive, the WITH_DAP=0 story is the one the tree already
 * uses for the language-server commands (src/xref.c's `#else` half, and the
 * unconditional cmdtable rows in src/cmd.c): the row exists in both
 * configurations and the disabled half answers "kg was built without ...
 * support" rather than leaving `M-x` to report an unknown command.  What
 * the stub half must never do is the other thing -- create a map, subscribe
 * to an event, or otherwise act as if a feature `kg -V` denies were there.
 *
 * This header names no editor type on purpose, so it stays free-standing
 * (`make header-check`).
 */

/* Called once from init_editor(), and once from editor_cleanup() beside the
 * other subsystem shutdowns.  Neither starts nor stops an adapter: a
 * session begins with the command that asks for one, and shutdown is what
 * ends a running one before the editor goes.
 *
 * dap_init() runs before the init file is loaded, which is what the three
 * debugger maps need: `define-key` creates a map it cannot find at
 * KEYMAP_LAYER_MAJOR (src/lisp_cmd.c), so an init.el that ran first would
 * permanently claim `dap` and `dap-breakpoint` in the wrong layer.  The
 * maps are created inactive for the mirror-image reason -- a map that was
 * active from startup would shadow global keys before DAP owns any
 * state. */
void dap_init(void);
void dap_shutdown(void);

/* Services the client's adapters from the two poll sites (the main loop and
 * tty.c's idle path).  Returns nonzero when that changed something the user
 * could see, i.e. when a repaint is wanted -- lsp_poll()'s convention, and
 * read the same way by the idle loop. */
int dap_poll(void);

/* How many descriptors dap_wait_fds() may write.  Zero, and honestly so.
 * src/dap_transport.c (stage 2) has descriptors and reports them through
 * dap_transport_wait_fds(), and src/dap_client.c (stage 3) passes them on
 * through dap_client_wait_fds() -- but a client is only as reachable as
 * whatever holds one, and what holds one is the session of stage 4.  Until
 * then this facade has nothing to ask and dap_wait_fds() has nothing to
 * answer.  A bound raised ahead of that would size the editor's wait for
 * descriptors that provably cannot exist, which is a promise rather than a
 * fact; the raise belongs with the stage that gives dap_wait_fds()
 * something to report.  src/async.c's static_assert is what makes it a
 * compile-time question then rather than a silently truncated wait. */
#define KG_DAP_WAIT_FDS_MAX 0

/* The descriptors the editor's idle wait should include, so that the next
 * dap_poll() happens when an adapter writes rather than when the idle tick
 * comes round; at most `max` are written to `fds`, and the count is the
 * return value. */
int dap_wait_fds(int *fds, int max);

#endif /* KG_DAP_H */
