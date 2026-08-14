/* The editor's fixed asynchronous-subsystem aggregate. */

#include "async.h"

#include <assert.h>
#include <stddef.h>

#include "dap.h"
#include "lsp.h"

/* Keep this an exact sum: adding a subsystem means adding its public bound
 * here, then adding one collection and one poll below.  A literal public
 * bound keeps async.h standalone while this assertion prevents drift.
 * KG_DAP_WAIT_FDS_MAX was zero until a live transport was reachable from
 * the debugger facade, which is what the session registry made it; the
 * raise came through this assertion rather than through a truncated
 * wait. */
static_assert(
    KG_ASYNC_WAIT_FDS_MAX == KG_LSP_WAIT_FDS_MAX + KG_DAP_WAIT_FDS_MAX,
    "KG_ASYNC_WAIT_FDS_MAX must equal every subsystem wait-fd bound");

static int fd_seen(const int *fds, int count, int fd)
{
	int i;

	for (i = 0; i < count; i++) {
		if (fds[i] == fd) {
			return 1;
		}
	}
	return 0;
}

/* One row per asynchronous subsystem, which is the whole of "adding a
 * subsystem": a row here, its bound in the static_assert above, and its
 * poll below.
 *
 * A -1 out of this function is not a rejected argument but a subsystem
 * breaking its own contract -- a count past the bound it declares, or a
 * descriptor it never opened -- and the caller's answer to a -1 is a wait
 * with no subsystem descriptor in it at all, which is silent: the editor
 * drops back to its 100 ms tick and nothing says why.  So these are
 * assertions rather than results, and the returns behind them are
 * unreachable.
 *
 * Descriptors already staged are skipped: the registries hold different
 * children, but nothing in either contract says they can never name one
 * descriptor, and a duplicate in the wait is a descriptor polled twice. */
static const struct {
	int (*wait_fds)(int *, int);
	int bound;
} subsystems[] = {
	{ lsp_wait_fds, KG_LSP_WAIT_FDS_MAX },
	{ dap_wait_fds, KG_DAP_WAIT_FDS_MAX },
};

int editor_async_wait_fds(int *fds, int max)
{
	int staged[KG_ASYNC_WAIT_FDS_MAX];
	int scratch[KG_ASYNC_WAIT_FDS_MAX];
	int unique = 0;
	size_t s;
	int i;

	if (!fds || max < KG_ASYNC_WAIT_FDS_MAX) {
		return -1;
	}
	for (s = 0; s < sizeof(subsystems) / sizeof(*subsystems); s++) {
		int count
		    = subsystems[s].wait_fds(scratch, subsystems[s].bound);

		assert(count >= 0 && count <= subsystems[s].bound);
		if (count < 0 || count > subsystems[s].bound) {
			return -1;
		}
		for (i = 0; i < count; i++) {
			assert(scratch[i] >= 0);
			if (scratch[i] < 0) {
				return -1;
			}
			if (!fd_seen(staged, unique, scratch[i])) {
				staged[unique++] = scratch[i];
			}
		}
	}
	for (i = 0; i < unique; i++) {
		fds[i] = staged[i];
	}
	return unique;
}

/* Both are polled every time: a subsystem skipped because an earlier one
 * wanted a repaint would be a subsystem serviced only while the other is
 * quiet. */
int editor_async_poll(void)
{
	int repaint = lsp_poll() != 0;

	repaint |= dap_poll() != 0;
	return repaint;
}
