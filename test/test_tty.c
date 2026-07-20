/* test_tty.c — unit tests for tty signal handling */

#include "../src/def.h"
#include "test.h"
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

/* Stubs for tty.c dependencies */
void win_reflow(void) { }
void editor_refresh_screen(void) { }
int autorevert_poll(void) { return 0; }
int macro_next_key(void) { return -1; }
void macro_on_key(int key) { (void)key; }

static void test_sigwinch_handling(void)
{
	/* pending_resize is static in tty.c, but we can call handle_sig_winch
	 * and verify that editor_process_pending_signals() detects it and
	 * returns 1. */

	/* First, ensure calling it without signal returns 0 */
	CHECK(editor_process_pending_signals() == 0);

	/* Coalesced signals still request one resize, and the handler does not
	 * disturb errno in the interrupted code. */
	errno = EBUSY;
	handle_sig_winch(SIGWINCH);
	CHECK(errno == EBUSY);
	handle_sig_winch(SIGWINCH);

	/* editor_process_pending_signals() should now detect it and return 1 */
	CHECK(editor_process_pending_signals() == 1);

	/* And subsequent calls should return 0 again */
	CHECK(editor_process_pending_signals() == 0);
}

/* The main loop skips redraws while editor_input_flood() reports a
 * paste-sized input queue, so a large paste costs a handful of
 * refreshes instead of one per byte.  A lone queued byte (ordinary
 * typing) must not suppress the redraw. */
static void test_input_flood_detects_paste_sized_queue(void)
{
	int fds[2];
	char burst[128];
	char byte = 'x';

	memset(burst, 'x', sizeof(burst));
	if (pipe(fds) != 0) {
		CHECK(!"pipe failed");
		return;
	}
	CHECK(editor_input_flood(fds[0]) == 0);
	CHECK(write(fds[1], &byte, 1) == 1);
	CHECK(editor_input_flood(fds[0]) == 0);
	CHECK(write(fds[1], burst, sizeof(burst)) == sizeof(burst));
	CHECK(editor_input_flood(fds[0]) == 1);
	close(fds[0]);
	close(fds[1]);
}

int main(void)
{
	RUN(test_sigwinch_handling);
	RUN(test_input_flood_detects_paste_sized_queue);
	return test_summary();
}
