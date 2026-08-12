/* Lifecycle of the optional debugger client: the facade legs src/dap.h
 * declares that the editor calls from editor_cleanup() and from the two
 * poll sites.  Compiled in every configuration, like src/lsp_core.c, so
 * the editor cannot tell which half it linked.
 *
 * dap_init() is not here: it creates the debugger keymaps, which reaches
 * the command layer, and this object is linked by every test binary (see
 * TEST_SRCS_OBJS in the Makefile).  It lives in src/dap_keymap.c.
 *
 * Stage 2 of doc/plans/dap/01-protocol.md built src/dap_transport.c but
 * nothing that holds one: an adapter for these legs to service, and a
 * descriptor for them to report, needs the client and session of stage 3.
 * Until then both halves are the same no-ops.
 */

#include "dap.h"

void dap_shutdown(void) { }

int dap_poll(void) { return 0; }

int dap_wait_fds(int *fds, int max)
{
	(void)fds;
	(void)max;
	return 0;
}
