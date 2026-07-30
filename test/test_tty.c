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

/* tty_write() writes through write_all(), so this binary links fileio.o
 * for the real loop rather than reimplementing it; these are the prompt
 * and dired entry points fileio.c reaches for and nothing here calls. */
int dired_fill_current(const char *dir)
{
	(void)dir;
	return 0;
}
int editor_confirm_yn(int fd, const char *fmt, ...)
{
	(void)fd;
	(void)fmt;
	return 0;
}
void editor_prompt_prefill_dir(char *buf, int bufsize)
{
	(void)bufsize;
	buf[0] = '\0';
}
int editor_read_line_path(int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	(void)bufsize;
	buf[0] = '\0';
	return MINIBUF_CANCELLED;
}

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

/* Mocks for the write syscall behind write_all(), in the shape
 * test_buffer.c's test_write_all uses. */
static int mock_eintr_count;
static int mock_short_count;
static size_t mock_written;

static ssize_t mock_write_retryable(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	if (mock_eintr_count > 0) {
		mock_eintr_count--;
		errno = EINTR;
		return -1;
	}
	if (mock_short_count > 0 && count > 2) {
		mock_short_count--;
		mock_written += 2;
		return 2;
	}
	mock_written += count;
	return (ssize_t)count;
}

static ssize_t mock_write_error(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	(void)count;
	errno = EACCES;
	return -1;
}

static ssize_t mock_write_zero(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	(void)count;
	return 0;
}

/* A frame that stops half-written leaves the screen lying about the
 * buffer, so tty_write() reports only "all of it went out" or "the
 * terminal is gone". */
static void test_tty_write_completes_or_reports_loss(void)
{
	const char frame[] = "\x1b[H\x1b[2Jhello";
	size_t len = sizeof(frame) - 1;

	mock_eintr_count = 3;
	mock_short_count = 2;
	mock_written = 0;
	editor_write_fn = mock_write_retryable;
	CHECK(tty_write(frame, len) == 0);
	CHECK(mock_written == len); /* every byte, retries included */
	CHECK(mock_eintr_count == 0);
	CHECK(mock_short_count == 0);

	editor_write_fn = mock_write_error;
	CHECK(tty_write(frame, len) == -1);

	/* A write(2) that reports zero bytes on a non-empty buffer is not
	 * progress; retrying it forever would hang the editor. */
	editor_write_fn = mock_write_zero;
	CHECK(tty_write(frame, len) == -1);

	/* Nothing to write is nothing to fail at. */
	CHECK(tty_write(frame, 0) == 0);

	editor_write_fn = write;
}

int main(void)
{
	RUN(test_sigwinch_handling);
	RUN(test_input_flood_detects_paste_sized_queue);
	RUN(test_tty_write_completes_or_reports_loss);
	return test_summary();
}
