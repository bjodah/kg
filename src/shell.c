/* ============================ Shell commands ==============================
 *
 * M-!  shell-command            Run a shell command.  Without a prefix
 *                               argument, display the output (echo area for
 *                               a short single line, otherwise a nonselecting
 *                               *Shell Command Output* window).  With a
 *                               prefix argument, insert stdout at point.
 * M-|  shell-command-on-region  Pipe the region through a shell command.
 *                               Without a prefix argument, display the
 *                               output and leave the region untouched.  With
 *                               a prefix argument, replace the region with
 *                               stdout; the original region is left in the
 *                               kill ring as a safety net.
 *
 * The child is spawned via /bin/sh -c so the user can use pipes, redirects,
 * and shell builtins.  Stdin/stdout are connected to pipes; stderr goes to
 * /dev/null so it cannot disturb the editor display.  No tty is shared with
 * the child, so interactive commands (less, vi, ...) will not work — by
 * design, M-! is for non-interactive filters.
 *
 * process.c does the spawning (see process.h), so the child is a process
 * group leader like a compilation's is: whatever it starts can be signalled
 * with it rather than surviving it. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "def.h"
#include "process.h"

#define SHELL_INITIAL_CAP 4096

/* Concurrently write `in` (inlen bytes) to wfd and read everything from rfd
 * into a newly-malloc'd buffer.  Either fd may be -1 to skip that side.
 * Both fds are closed before return.  Returns the output buffer on success
 * (NUL-terminated, *out_len set), NULL on allocation failure. */
static char *pump_io(int rfd, int wfd, const char *in, int inlen, int *out_len)
{
	struct pollfd pfd[2];
	char *buf;
	int buf_cap = SHELL_INITIAL_CAP;
	int buf_len = 0;
	int written = 0;
	int n;

	buf = malloc(buf_cap);
	if (!buf) {
		kg_close_fd(&rfd);
		kg_close_fd(&wfd);
		*out_len = 0;
		return NULL;
	}

	if (rfd >= 0) {
		fcntl(rfd, F_SETFL, O_NONBLOCK);
	}
	if (wfd >= 0) {
		fcntl(wfd, F_SETFL, O_NONBLOCK);
	}

	/* If there's no input to send, close the write side immediately so the
	 * child sees EOF on stdin and isn't left blocking on a read. */
	if (wfd >= 0 && (in == NULL || inlen <= 0)) {
		kg_close_fd(&wfd);
	}

	while (rfd >= 0 || wfd >= 0) {
		/* Both slots are always polled: poll() ignores an entry whose
		 * fd is negative and reports revents 0 for it, so the side
		 * that has already been closed needs no bookkeeping here. */
		pfd[0].fd = rfd;
		pfd[0].events = POLLIN;
		pfd[1].fd = wfd;
		pfd[1].events = POLLOUT;

		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (buf_len + 1024 >= buf_cap) {
				char *nb;
				buf_cap *= 2;
				nb = realloc(buf, buf_cap);
				if (!nb) {
					free(buf);
					kg_close_fd(&rfd);
					kg_close_fd(&wfd);
					*out_len = 0;
					return NULL;
				}
				buf = nb;
			}
			n = read(rfd, buf + buf_len, buf_cap - buf_len - 1);
			if (n > 0) {
				buf_len += n;
			} else {
				kg_close_fd(&rfd);
			}
		}

		if (pfd[1].revents & (POLLOUT | POLLHUP | POLLERR)) {
			n = write(wfd, in + written, inlen - written);
			if (n > 0) {
				written += n;
				if (written >= inlen) {
					kg_close_fd(&wfd);
				}
			} else if (n < 0 && errno == EAGAIN) {
				;
			} else {
				kg_close_fd(&wfd);
			}
		}
	}

	buf[buf_len] = '\0';
	*out_len = buf_len;
	return buf;
}

/* Run `cmd` via /bin/sh -c.  Optionally write `in` (inlen bytes) to its stdin.
 * On success returns a malloc'd buffer of the child's stdout and sets *out_len.
 * Returns NULL on fork/pipe failure.  Exposed (rather than static) so the
 * test suite can exercise it without driving the editor through a PTY. */
char *shell_run(const char *cmd, const char *in, int inlen, int *out_len,
    struct shell_run_status *status)
{
	void (*old_sigpipe)(int);
	struct kg_spawn_request request = {
		.command = cmd,
		.directory = NULL,
		.stdin_fd = -1,
		.stderr_to_output = false,
		.nonblocking_output = false,
	};
	int in_rd = -1, in_wr = -1;
	int out_rd = -1;
	int pump_rfd, pump_wfd;
	int p[2];
	int saved_errno;
	struct kg_process_status result;
	char *out;
	pid_t pid = -1; /* -1 sentinel: no child spawned yet, nothing to reap */

	if (status) {
		memset(status, 0, sizeof(*status));
	}

	if (kg_pipe_cloexec(p) < 0) {
		goto fail;
	}
	in_rd = p[0];
	in_wr = p[1];

	request.stdin_fd = in_rd;
	if (kg_process_spawn(&request, &pid, &out_rd) != 0) {
		goto fail;
	}
	kg_close_fd(&in_rd);

	/* pump_io() owns both ends from here and closes them, so the fail
	 * path below must not see them again. */
	pump_rfd = out_rd;
	pump_wfd = in_wr;
	out_rd = in_wr = -1;

	/* If the child exits early we don't want EPIPE to kill us. */
	old_sigpipe = signal(SIGPIPE, SIG_IGN);

	out = pump_io(pump_rfd, pump_wfd, in, inlen, out_len);
	saved_errno = errno;
	signal(SIGPIPE, old_sigpipe);
	if (!out) {
		/* pump_io() ran out of memory and has already closed both
		 * pipes.  A child that is neither reading nor writing them --
		 * `sleep 60`, or anything it left running -- would otherwise
		 * keep the editor blocked in the reap below for as long as it
		 * cares to live, so kill the group and take its children with
		 * it. */
		kg_process_signal_group(pid, SIGKILL);
	}
	if (kg_process_wait(pid, &result) == 0 && status) {
		status->known = true;
		status->exited = result.exited;
		status->exit_code = result.exit_code;
		status->signal_number = result.signal_number;
	}
	errno = saved_errno;
	return out;

fail:
	/* Only reachable before the child exists: a pipe that could not be
	 * created, or a fork that failed.  Nothing to reap. */
	saved_errno = errno;
	kg_close_fd(&in_rd);
	kg_close_fd(&in_wr);
	kg_close_fd(&out_rd);
	*out_len = 0;
	errno = saved_errno;
	return NULL;
}

/* Insert text at point as a single undoable yank. */
static void insert_as_yank(const char *text, int len)
{
	int filerow = wcur()->rowoff + wcur()->cy;
	int filecol = wcur()->coloff + wcur()->cx;

	if (len <= 0) {
		return;
	}

	undo_push(
	    bcur(), UNDO_YANK_TEXT, filerow, filecol, 0, (char *)text, len);
	editor_insert_text_raw(text, len);
}

/* Reject scalar values utf8_glyph_span_at() lets through as structurally
 * plausible (right lead byte, right continuation-byte count) but that are
 * not valid Unicode: overlong encodings for E0/F0, UTF-16 surrogates for
 * ED, and values above U+10FFFF for F4.  Only called for a span > 1 glyph,
 * so buf[start+1] is known to exist. */
static bool utf8_scalar_is_valid(const char *buf, int start)
{
	unsigned char lead = (unsigned char)buf[start];
	unsigned char second = (unsigned char)buf[start + 1];

	switch (lead) {
	case 0xE0:
		return second >= 0xA0 && second <= 0xBF;
	case 0xED:
		return second >= 0x80 && second <= 0x9F;
	case 0xF0:
		return second >= 0x90 && second <= 0xBF;
	case 0xF4:
		return second >= 0x80 && second <= 0x8F;
	default:
		return true;
	}
}

/* True if `out` (out_len bytes, one optional trailing newline) can go
 * straight into the echo area: exactly one logical line, valid UTF-8, no
 * control bytes (the renderer in display.c passes ESC sequences through
 * verbatim so the completion picker can highlight entries, which would
 * otherwise let shell output inject terminal escapes), and short enough in
 * display columns (not bytes) to leave `reserved_columns` free for a
 * suffix the caller will append, while also fitting statusmsg's fixed byte
 * capacity. */
bool shell_output_fits_echo(
    const char *out, int out_len, int available_columns, int reserved_columns)
{
	int stop = out_len;
	int columns = 0;
	int budget = available_columns - reserved_columns;
	int i;

	if (out_len <= 0) {
		return false;
	}
	if (out[out_len - 1] == '\n') {
		stop = out_len - 1;
	}
	if (stop <= 0 || stop >= (int)sizeof(editor.statusmsg)) {
		return false;
	}
	if (budget < 0) {
		budget = 0;
	}
	for (i = 0; i < stop;) {
		unsigned char ch = (unsigned char)out[i];
		int span;

		if (ch < 0x20 || ch == 0x7f) {
			return false;
		}
		if (ch < 0x80) {
			if (++columns > budget) {
				return false;
			}
			i++;
			continue;
		}
		span = utf8_glyph_span_at(out, stop, i);
		if (span == 1 || !utf8_scalar_is_valid(out, i)) {
			return false; /* malformed or invalid UTF-8 */
		}
		columns += utf8_width_at(out, stop, i);
		if (columns > budget) {
			return false;
		}
		i += span;
	}
	return true;
}

/* Small note appended to completion messages when the command's exit
 * status is available and indicates something the base wording alone
 * wouldn't convey (nonzero exit code, or death by signal). */
static void shell_status_suffix(
    const struct shell_run_status *status, char *buf, size_t bufsize)
{
	buf[0] = '\0';
	if (!status) {
		return;
	}
	if (!status->known) {
		snprintf(buf, bufsize, " (exit status unavailable)");
	} else if (status->exited && status->exit_code != 0) {
		snprintf(buf, bufsize, " (exit %d)", status->exit_code);
	} else if (!status->exited && status->signal_number != 0) {
		snprintf(buf, bufsize, " (signal %d)", status->signal_number);
	}
}

static void display_shell_output(
    const char *out, int out_len, const struct shell_run_status *status)
{
	char suffix[32];

	shell_status_suffix(status, suffix, sizeof(suffix));

	if (!out || out_len <= 0) {
		if (suffix[0]) {
			editor_set_status_message(
			    "Shell command produced no output%s", suffix);
		} else {
			editor_set_status_message(
			    "Shell command succeeded with no output");
		}
		return;
	}

	if (shell_output_fits_echo(
		out, out_len, win_total_cols, (int)strlen(suffix))) {
		int len = (out[out_len - 1] == '\n') ? out_len - 1 : out_len;
		editor_set_status_message("%.*s%s", len, out, suffix);
		return;
	}

	int index = buf_prepare_special_text(
	    "*Shell Command Output*", &text_syntax, 1);
	if (index < 0
	    || buf_append_special_text(index, out, (size_t)out_len) != 0) {
		return;
	}

	/* Never steal the current editing window to show shell output: only
	 * route to another window when one can be found or split without
	 * disturbing the buffer the user is actively editing.  The output
	 * stays reachable through the special buffer either way. */
	if (win_can_display_buffer_other_window(index)) {
		win_display_buffer_other_window(index);
		win_position_at_end(index);
		editor_set_status_message("Shell command finished%s", suffix);
	} else {
		editor_set_status_message(
		    "Shell command output is in *Shell Command Output*%s",
		    suffix);
	}
}

static void handle_shell_result(const char *out, int out_len, int insert_output,
    int is_region, const struct shell_run_status *status)
{
	char suffix[32];

	if (!out) {
		editor_set_status_message(
		    "Shell command failed: %s", strerror(errno));
		return;
	}
	if (!insert_output) {
		display_shell_output(out, out_len, status);
		return;
	}
	shell_status_suffix(status, suffix, sizeof(suffix));
	if (is_region) {
		editor_kill_region();
		insert_as_yank(out, out_len);
		editor_set_status_message("Replaced region (%d byte%s out)%s",
		    out_len, out_len == 1 ? "" : "s", suffix);
	} else {
		int start_row = editor_current_filerow_or_eof();
		int start_col = editor_current_filecol();
		insert_as_yank(out, out_len);
		bcur()->mark_set = 1;
		bcur()->mark_row = editor_current_filerow_or_eof();
		bcur()->mark_col = editor_current_filecol();
		editor_cursor_goto(start_row, start_col);
		editor_set_status_message("Inserted %d byte%s%s", out_len,
		    out_len == 1 ? "" : "s", suffix);
	}
}

/* Shared by editor_shell_command() and editor_shell_command_on_region() so
 * M-p/M-n recall commands run from either entry point. */
static struct minibuf_history shell_command_history;

/* M-! shell-command: prompt, run, display or insert stdout at point. */
void editor_shell_command(int fd, int insert_output)
{
	char cmd[256] = { 0 };
	char *out;
	int out_len = 0;
	struct shell_run_status status;
	enum minibuf_result result;

	if (insert_output && bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}
	result = editor_read_line_with_history(
	    fd, "Shell command: ", cmd, sizeof(cmd), &shell_command_history);
	if (result != MINIBUF_ACCEPTED || !cmd[0]) {
		return;
	}

	out = shell_run(cmd, NULL, 0, &out_len, &status);
	handle_shell_result(out, out_len, insert_output, 0, &status);
	free(out);
}

/* M-| shell-command-on-region: pipe region through cmd, display or replace with
 * stdout.
 */
void editor_shell_command_on_region(int fd, int insert_output)
{
	char cmd[256] = { 0 };
	char *region, *out;
	int region_len = 0, out_len = 0;
	struct shell_run_status status;
	enum minibuf_result result;

	if (insert_output && bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}
	if (!bcur()->mark_set) {
		editor_set_status_message("No mark set");
		return;
	}

	region = editor_get_region_text(&region_len);
	if (!region) {
		editor_set_status_message("Empty region");
		return;
	}

	result = editor_read_line_with_history(fd,
	    "Shell command on region: ", cmd, sizeof(cmd),
	    &shell_command_history);
	if (result == MINIBUF_ACCEPTED && cmd[0]) {
		out = shell_run(cmd, region, region_len, &out_len, &status);
		handle_shell_result(out, out_len, insert_output, 1, &status);
		free(out);
	}
	free(region);
}
