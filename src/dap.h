#ifndef KG_DAP_H
#define KG_DAP_H

/* The one editor-facing facade of the optional debugger client
 * (doc/plans/2026-08-11-dap.md).  Everything below exists in both build
 * configurations -- src/dap_core.c and src/dap_keymap.c are compiled
 * whatever WITH_DAP says, the way src/lsp_core.c is -- so no caller carries
 * a KG_USE_DAP conditional, and a WITH_DAP=0 editor simply calls entry
 * points that do nothing.
 *
 * Stage 1 is the axis, this facade and the three keymaps, and nothing else:
 * there is no transport, no client and no command yet.  The protocol
 * modules (transport, client, config, session, breakpoints) land in later
 * stages behind these same four functions; their headers are included by
 * their real C consumers, never from here.
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

/* How many descriptors dap_wait_fds() may write.  Zero, and honestly so:
 * stage 1 has no transport, so there is no descriptor for the facade to
 * report and none to size an array with.  It rises with src/dap_transport.c
 * (stage 2), which is where the first one exists, and src/async.c's
 * static_assert is what makes that raise a compile-time question rather
 * than a silently truncated wait. */
#define KG_DAP_WAIT_FDS_MAX 0

/* The descriptors the editor's idle wait should include, so that the next
 * dap_poll() happens when an adapter writes rather than when the idle tick
 * comes round; at most `max` are written to `fds`, and the count is the
 * return value. */
int dap_wait_fds(int *fds, int max);

#endif /* KG_DAP_H */
