/* Lifecycle of the optional debugger client: the facade legs src/dap.h
 * declares that the editor calls from editor_cleanup() and from the two
 * poll sites.  Compiled in every configuration, like src/lsp_core.c, so
 * the editor cannot tell which half it linked.
 *
 * dap_init() is not here: it creates the debugger keymaps, which reaches
 * the command layer, and this object is linked by every test binary (see
 * TEST_SRCS_OBJS in the Makefile).  It lives in src/dap_keymap.c.
 *
 * Stages 2 and 3 built src/dap_transport.c and src/dap_client.c but
 * nothing that held one, so both halves were the same no-ops.  Stage 4's
 * session registry is what holds one, and these legs are that registry seen
 * from the editor: poll every session, report every session's descriptors,
 * and end every session when the editor exits.
 */

#include "dap.h"

#include "dap_ui.h"

/* The panes' debounced repaint, installed by dap_commands_init().  It is a
 * pointer for a link reason rather than a design one: src/dap_ui.c reaches
 * buffers and windows, so it is not in the object set every test binary
 * links, and this file is.  A NULL hook is an editor whose panes are
 * repainted by nothing, which is exactly what a suite with no editor in it
 * wants. */
static int (*ui_poll)(void);

void dap_set_ui_poll(int (*fn)(void)) { ui_poll = fn; }

#ifdef KG_USE_DAP

#include "dap_breakpoint.h"
#include "dap_decor.h"
#include "dap_exec.h"
#include "dap_java.h"
#include "dap_session.h"

/* The whole budget for ending every running session, split between them by
 * the registry -- lsp_core.c's number and its reasoning: it is under the
 * threshold at which quitting an editor feels like it hung, and it is far
 * more than a `disconnect` or `terminate` exchange takes against either
 * measured adapter (debugpy ~41 ms per round trip, lldb-dap under 10).  An
 * adapter that wants longer does not get it; the transport's kill backstop
 * is what collects a child kg started. */
#define DAP_SHUTDOWN_GRACE_MS 300u

void dap_shutdown(void)
{
	/* Before the sessions, because a Java one that has not connected yet
	 * has no session for the line below to end -- and because a session
	 * that HAS connected must not have its owner checked again after it
	 * is gone (src/dap_java.h). */
	dap_java_session_ended();
	dap_session_shutdown_all(DAP_SHUTDOWN_GRACE_MS);
	/* The marks come off before the table they are a projection of goes
	 * away, since a decoration never outlives what it describes. */
	dap_decor_reset();
	/* The table outlives every session, but not the editor: this is
	 * where its sources and their markers go. */
	dap_breakpoint_reset();
}

/* Both legs of a poll: the sessions, and the stop model's own tick -- the
 * debounced `thread` re-request, the reaping of a session that has died,
 * and the relaunch a client-side `dap-restart` is waiting to start.  Each
 * reports a repaint the way lsp_poll() does, and either one may want one. */
int dap_poll(void)
{
	int changed = dap_session_poll_all();

	changed |= dap_exec_poll();
	/* Fourth leg, and it is a leg rather than a hook because it can
	 * START a session: a Java one spends its first seconds waiting for
	 * a language server, and that wait advances here (src/dap_java.h). */
	changed |= dap_java_poll();
	/* An edit moved a breakpoint's anchor and the debounce has run out:
	 * the adapter is told where the breakpoint is NOW, or it keeps
	 * stopping at the line it was on when the session started
	 * (src/dap_breakpoint.h).  Not a repaint of its own -- the edit
	 * already painted the mark at its new line, and what the adapter
	 * answers arrives through the session poll above. */
	if (dap_breakpoint_resync_due()) {
		dap_breakpoint_sync();
	}
	if (changed) {
		dap_decor_refresh();
	}
	/* Third leg, and deliberately after the others: the panes are a
	 * projection of what the two above just learned, and their repaint
	 * is debounced -- this is the tick that lets a burst of events
	 * become one repaint (src/dap_ui.h). */
	if (ui_poll) {
		changed |= ui_poll();
	}
	return changed;
}

int dap_wait_fds(int *fds, int max) { return dap_session_wait_fds(fds, max); }

#else /* !KG_USE_DAP */

void dap_shutdown(void) { }

int dap_poll(void) { return 0; }

int dap_wait_fds(int *fds, int max)
{
	(void)fds;
	(void)max;
	return 0;
}

#endif /* KG_USE_DAP */
