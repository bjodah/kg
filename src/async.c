/* The editor's fixed asynchronous-subsystem aggregate. */

#include "async.h"

#include <string.h>

#include "lsp.h"

/* Keep this an exact sum: adding a subsystem means adding its public bound
 * here, then adding one collection and one poll below.  A literal public
 * bound keeps async.h standalone while this assertion prevents drift. */
static_assert(KG_ASYNC_WAIT_FDS_MAX == KG_LSP_WAIT_FDS_MAX,
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

int editor_async_wait_fds(int *fds, int max)
{
	int staged[KG_ASYNC_WAIT_FDS_MAX];
	int count;
	int unique = 0;
	int i;

	if (!fds || max < KG_ASYNC_WAIT_FDS_MAX) {
		return -1;
	}
	count = lsp_wait_fds(staged, KG_LSP_WAIT_FDS_MAX);
	if (count < 0 || count > KG_LSP_WAIT_FDS_MAX) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		if (staged[i] < 0) {
			return -1;
		}
		if (!fd_seen(staged, unique, staged[i])) {
			staged[unique++] = staged[i];
		}
	}
	memcpy(fds, staged, (size_t)unique * sizeof(*fds));
	return unique;
}

int editor_async_poll(void) { return lsp_poll(); }
