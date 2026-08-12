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

#ifdef KG_USE_DAP

#include "dap_breakpoint.h"
#include "dap_decor.h"
#include "dap_exec.h"
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
	if (changed) {
		dap_decor_refresh();
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
