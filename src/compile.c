#include "compile.h"
#include "def.h"
#include "event.h"
#include "kbd.h"
#include "process.h"
#include "winmgr.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform.h"
#else
#include <unistd.h>
#endif

#define COMPILATION_READ_CHUNK 4096
#define COMPILATION_TICK_BUDGET (64 * 1024)

static struct compilation_state g_compilation;

/* The programmatic seam's whole state (see compile.h).  One slot, because
 * one compilation runs at a time and a caller that asked while another's
 * answer was still undelivered was told BUSY.
 *
 * `generation` is nonzero exactly while a caller is owed a callback: it is
 * stamped when the call is accepted, compared by
 * compilation_cancel_programmatic(), and cleared by the delivery.  That one
 * number is what makes cancellation safe against every race -- an abandoned
 * caller's generation no longer matches, so the completion it would have
 * received is dropped rather than delivered into a freed context. */
static struct {
	unsigned generation;
	compilation_done_fn done;
	void *ctx;
	bool attached; /* the run in progress is this caller's */
	bool ready; /* its result is waiting for the safe point */
	struct compilation_result result;
} g_programmatic;

static unsigned g_programmatic_next_generation = 1;

/* Called from compilation_poll()'s finalize block, defined with the rest of
 * the seam below it. */
static void programmatic_finish(void);

/* Retained-output budget handed to the next compilation.  compilation_start()
 * copies it into the run's own state, so lowering it (tests do) never disturbs
 * a compilation already in progress. */
static size_t g_default_maximum_output = 8 * 1024 * 1024;

void compilation_set_maximum_output(size_t bytes)
{
	g_default_maximum_output = bytes;
}

/* Nothing here names compile_nav.c; see compile.h's struct
 * compile_diag_hooks. */
static const struct compile_diag_hooks *g_diag_hooks;

void compilation_set_diag_hooks(const struct compile_diag_hooks *hooks)
{
	g_diag_hooks = hooks;
}

/* Reset the streaming half of `s` for a fresh run and adopt `bytes` as its
 * retained-output budget.  `s` must either be zeroed or own a pending line.
 * Exposed so a test can drive the byte parser against a stack state. */
void compilation_stream_reset(struct compilation_state *s, size_t bytes)
{
	free(s->pending_line);
	s->pending_line = NULL;
	s->pending_line_length = 0;
	s->pending_line_cap = 0;
	s->displayed_pending_length = 0;
	s->stored_output = 0;
	s->maximum_output = bytes;
	s->truncated = false;
	s->truncation_marker_written = false;
	s->ansi_state = 0;
	s->pending_cr = false;
	s->committed_len = 0;
}

int compilation_resolve_directory(
    const char *filename, char *directory, size_t directory_size)
{
	const char *slash = NULL;
	size_t dirlen;

	/* is_special_buffer() already answers for a NULL name. */
	if (!is_special_buffer(filename)) {
		slash = strrchr(filename, '/');
	}
	if (!slash) {
		if (!getcwd(directory, directory_size)) {
			return -1;
		}
		return 0;
	}

	if (filename[0] == '/') {
		dirlen = (size_t)(slash - filename);
		if (dirlen >= directory_size) {
			return -1;
		}
		memcpy(directory, filename, dirlen);
		directory[dirlen] = '\0';
	} else {
		char cwd[PATH_MAX];
		size_t cwdlen;

		if (!getcwd(cwd, sizeof(cwd))) {
			return -1;
		}
		cwdlen = strlen(cwd);
		dirlen = (size_t)(slash - filename);
		if (cwdlen + 1 + dirlen >= directory_size) {
			return -1;
		}
		memcpy(directory, cwd, cwdlen);
		directory[cwdlen] = '/';
		memcpy(directory + cwdlen + 1, filename, dirlen);
		directory[cwdlen + 1 + dirlen] = '\0';
	}

	{
		char *resolved = realpath(directory, NULL);
		if (resolved) {
			size_t rlen = strlen(resolved);
			if (rlen < directory_size) {
				memcpy(directory, resolved, rlen + 1);
			}
			free(resolved);
		}
	}

	return 0;
}

/* The two call sites for g_diag_hooks, kept out of compilation_start() and
 * compilation_commit_line() themselves so adding the hook did not raise
 * either function's own complexity -- see .ci/pmccabe-baseline.json's entry
 * for each. */
static void compilation_report_diag_reset(
    struct kg_buffer_handle compilation_buffer, const char *directory)
{
	if (!g_diag_hooks || !g_diag_hooks->reset) {
		return;
	}
	g_diag_hooks->reset(compilation_buffer, directory, strlen(directory));
}

static void compilation_report_diag_line(
    struct compilation_state *s, size_t line_len, size_t line_start)
{
	if (!g_diag_hooks || !g_diag_hooks->ingest_line) {
		return;
	}
	g_diag_hooks->ingest_line(
	    s->pending_line ? s->pending_line : "", line_len, line_start);
}

/* Start a compilation of COMMAND in DIRECTORY. compilation_start is the
 * single place that records the command/directory that actually started, so
 * an ordinary start and a deferred restart both leave last_command and
 * last_directory consistent for a later recompile. When from_user is true the
 * caller has just switched into source_buffer, so focus is returned there;
 * for a deferred restart (launched from the top-level loop) we keep whatever
 * buffer the user currently occupies and only show output in another window.
 *
 * Returns whether a child is now running.  The two false paths -- no
 * compilation buffer, no child -- already told the user in the echo area;
 * the return exists for the programmatic seam, which owes its caller a
 * completion even when nothing ever ran. */
static void feed_release(void);

static bool compilation_start(const char *command, const char *directory,
    struct kg_buffer_handle source_buffer, bool from_user)
{
	int source_slot = buf_handle_slot(source_buffer);
	int cidx
	    = buf_prepare_special_text("*compilation*", &compilation_syntax, 1);
	if (cidx < 0) {
		editor_set_status_message(
		    "Failed to prepare compilation buffer");
		return false;
	}

	/* A real compilation takes the buffer back from any feed that was
	 * still open: the user's build wins, and interleaving two writers
	 * into one buffer would produce a transcript belonging to neither. */
	feed_release();

	g_compilation.have_last_command = true;
	strncpy(g_compilation.last_command, command,
	    sizeof(g_compilation.last_command));
	g_compilation.last_command[sizeof(g_compilation.last_command) - 1]
	    = '\0';
	strncpy(g_compilation.last_directory, directory,
	    sizeof(g_compilation.last_directory));
	g_compilation.last_directory[sizeof(g_compilation.last_directory) - 1]
	    = '\0';

	g_compilation.phase = COMPILATION_RUNNING;
	g_compilation.source_buffer = source_buffer;
	g_compilation.compilation_buffer = buf_handle(cidx);
	g_compilation.pipe_eof = false;
	g_compilation.child_reaped = false;
	g_compilation.wait_status = (struct kg_process_status) { 0 };
	compilation_stream_reset(&g_compilation, g_default_maximum_output);

	/* stdin from /dev/null, stderr into the same pipe as stdout, and a
	 * read end kg can drain without blocking its own input loop. */
	struct kg_spawn_request request = {
		.command = command,
		.directory = directory,
		.stdin_fd = -1,
		.stderr_to_output = true,
		.nonblocking_output = true,
	};
	pid_t pid;
	int out_fd;
	if (kg_process_spawn(&request, &pid, &out_fd) != 0) {
		editor_set_status_message(
		    "Cannot start compilation: %s", strerror(errno));
		g_compilation.phase = COMPILATION_IDLE;
		return false;
	}

	g_compilation.pid = pid;
	g_compilation.process_group = pid;
	g_compilation.output_fd = out_fd;

	char header[PATH_MAX + KG_COMPILE_COMMAND_MAX + 128];
	int header_len = snprintf(header, sizeof(header),
	    "Compilation started in %s\n\n$ %s\n\n", directory, command);
	buf_append_special_text(cidx, header, header_len);
	g_compilation.committed_len += (size_t)header_len;
	compilation_report_diag_reset(
	    g_compilation.compilation_buffer, directory);

	/* The source buffer may have been killed between the prompt and here;
	 * a compilation with nowhere to return to is still a compilation. */
	if (from_user && source_slot >= 0) {
		buf_select(source_slot);
	}
	win_display_buffer_other_window(cidx);
	win_position_at_end(cidx);

	editor_set_status_message("Compilation started: %s", command);
	return true;
}

/* Run `command` in `dir` on behalf of the buffer `source` names, dealing
 * first with a compilation that is already running: ask before killing it,
 * and on a yes signal it and queue this command to start once it dies.
 * Shared by M-x compile and M-x recompile, which differ only in where the
 * command and the directory come from. */
static void compilation_start_or_defer(int fd, const char *command,
    const char *dir, struct kg_buffer_handle source)
{
	if (compilation_is_running()) {
		if (!editor_confirm_yn(fd,
			"A compilation process is running; kill it? (y/n) ")) {
			editor_set_status_message("Compilation not started");
			return;
		}

		/* editor_read_key() polls the compilation while it waits for
		 * the answer, so it may have finished and gone idle meanwhile.
		 * Only signal/defer if it is genuinely still running; otherwise
		 * start the new command directly to avoid signalling a stale
		 * (possibly reused) process group. */
		if (compilation_is_running()) {
			kg_process_signal_group(
			    g_compilation.process_group, SIGINT);
			g_compilation.phase = COMPILATION_TERMINATING;
			g_compilation.restart_pending = true;
			strncpy(g_compilation.pending_command, command,
			    sizeof(g_compilation.pending_command));
			g_compilation.pending_command
			    [sizeof(g_compilation.pending_command) - 1] = '\0';
			strncpy(g_compilation.pending_directory, dir,
			    sizeof(g_compilation.pending_directory));
			g_compilation.pending_directory
			    [sizeof(g_compilation.pending_directory) - 1]
			    = '\0';
			g_compilation.pending_source_buffer = source;

			editor_set_status_message("Sent SIGINT to active "
						  "compilation, restart "
						  "pending...");
			return;
		}
	}

	(void)compilation_start(command, dir, source, true);
}

/* Emacs' compile-history: separate from every other prompt's ring. */
static struct minibuf_history compile_history;

void editor_compile(int fd)
{
	char prompt[KG_COMPILE_COMMAND_MAX];
	char dir[PATH_MAX];
	int rc;
	struct kg_buffer_handle source;

	strncpy(prompt, bcur()->compile_command, sizeof(prompt));
	prompt[sizeof(prompt) - 1] = '\0';

	rc = editor_read_line_with_history(
	    fd, "Compile command: ", prompt, sizeof(prompt), &compile_history);
	if (rc < 0) {
		return;
	}
	if (rc > 0) {
		editor_set_status_message("compile command too long");
		return;
	}
	if (prompt[0] == '\0') {
		editor_set_status_message("No compile command");
		return;
	}

	strncpy(
	    bcur()->compile_command, prompt, sizeof(bcur()->compile_command));
	bcur()->compile_command[sizeof(bcur()->compile_command) - 1] = '\0';
	bcur()->compile_command_user_override = 1;
	source = buf_handle(buf_current);

	if (compilation_resolve_directory(bcur()->filename, dir, sizeof(dir))
	    != 0) {
		if (!getcwd(dir, sizeof(dir))) {
			strcpy(dir, ".");
		}
	}

	compilation_start_or_defer(fd, prompt, dir, source);
}

void editor_recompile(int fd)
{
	char dir[PATH_MAX];
	char cmd_buf[KG_COMPILE_COMMAND_MAX];
	const char *command;
	struct kg_buffer_handle source;

	if (bcur()->filename
	    && strcmp(bcur()->filename, "*compilation*") == 0) {
		if (!g_compilation.have_last_command) {
			editor_set_status_message("No compile command");
			return;
		}
		strncpy(cmd_buf, g_compilation.last_command, sizeof(cmd_buf));
		cmd_buf[sizeof(cmd_buf) - 1] = '\0';
		command = cmd_buf;
		strncpy(dir, g_compilation.last_directory, sizeof(dir));
		dir[sizeof(dir) - 1] = '\0';
	} else {
		command = bcur()->compile_command;
		if (command[0] == '\0') {
			editor_set_status_message("No compile command");
			return;
		}
		if (compilation_resolve_directory(
			bcur()->filename, dir, sizeof(dir))
		    != 0) {
			if (!getcwd(dir, sizeof(dir))) {
				strcpy(dir, ".");
			}
		}
	}

	source = buf_handle(buf_current);

	compilation_start_or_defer(fd, command, dir, source);
}

/* Accept one output byte into the pending line, charging it to the budget as
 * it is accepted.  Past the budget the byte is dropped and the run is marked
 * truncated, so pending_line never holds a byte that was not paid for and its
 * capacity never exceeds what the budget could still fund.  Bytes later rubbed
 * out by \b or \r are not refunded: a stream that keeps erasing itself still
 * makes progress towards the cap instead of spinning.  A failed realloc counts
 * as truncation too — the byte is lost either way, and the user is entitled to
 * know the output is incomplete. */
static void compilation_append_char(struct compilation_state *s, char c)
{
	if (s->stored_output >= s->maximum_output) {
		s->truncated = true;
		return;
	}
	if (s->pending_line_length + 1 >= s->pending_line_cap) {
		size_t room = s->maximum_output - s->stored_output;
		size_t new_cap
		    = s->pending_line_cap == 0 ? 128 : s->pending_line_cap * 2;
		char *new_buf;

		/* Clamp geometric growth to what the budget can still fund.
		 * Phrased so the sum is only formed once it is known to be
		 * below new_cap, which cannot overflow. */
		if (new_cap - s->pending_line_length - 1 > room) {
			new_cap = s->pending_line_length + room + 1;
		}
		/* Both writes below -- the byte and the terminator after it --
		 * have to fit.  They already do: room is at least 1, because
		 * the budget check above returned when stored_output had
		 * caught up with maximum_output, so the clamped capacity is
		 * never below pending_line_length + 2.  Saying so costs one
		 * comparison and puts the property in front of the next edit
		 * to the growth above, as well as in front of the analyzer,
		 * which does not carry the subtraction's lower bound this
		 * far and reads the clamp as a one-byte allocation. */
		if (new_cap < s->pending_line_length + 2) {
			new_cap = s->pending_line_length + 2;
		}
		new_buf = realloc(s->pending_line, new_cap);
		if (!new_buf) {
			s->truncated = true;
			return;
		}
		s->pending_line = new_buf;
		s->pending_line_cap = new_cap;
	}
	s->pending_line[s->pending_line_length++] = c;
	s->pending_line[s->pending_line_length] = '\0';
	s->stored_output++;
}

/* Commit the completed pending line to the buffer as permanent output and
 * reset the pending line.  Its body was charged byte by byte as it arrived;
 * the terminating newline is charged here, and that is what bounds a stream
 * which is nothing but newlines.  The caller must have already removed any
 * mirrored copy of the pending line from the buffer's last row (see
 * displayed_pending_length). */
static void compilation_commit_line(struct compilation_state *s)
{
	int slot = buf_handle_slot(s->compilation_buffer);
	size_t line_start = s->committed_len;
	size_t line_len = s->pending_line_length;

	if (line_len > 0) {
		buf_append_special_text(slot, s->pending_line, line_len);
		s->committed_len += line_len;
	}
	if (s->stored_output < s->maximum_output) {
		buf_append_special_text(slot, "\n", 1);
		s->committed_len += 1;
		s->stored_output++;
	} else {
		s->truncated = true;
	}
	compilation_report_diag_line(s, line_len, line_start);
	s->pending_line_length = 0;
	if (s->pending_line) {
		s->pending_line[0] = '\0';
	}
}

/* Mirror the still-incomplete pending line into the buffer's last row so the
 * user sees partial progress.  These bytes are transient — they are removed
 * (via displayed_pending_length) before the next chunk is processed — but they
 * are already paid for, so the whole pending line is shown and what is on
 * screen is exactly what a later commit will keep. */
static void compilation_mirror_pending(struct compilation_state *s)
{
	if (s->pending_line_length > 0) {
		buf_append_special_text(buf_handle_slot(s->compilation_buffer),
		    s->pending_line, s->pending_line_length);
	}
	s->displayed_pending_length = s->pending_line_length;
}

void compilation_process_bytes(
    struct compilation_state *s, const char *bytes, size_t len)
{
	if (len == 0) {
		return;
	}

	buf_truncate_last_row(buf_handle_slot(s->compilation_buffer),
	    s->displayed_pending_length);
	s->displayed_pending_length = 0;

	for (size_t i = 0; i < len; i++) {
		unsigned char c = bytes[i];

		switch (s->ansi_state) {
		case 0:
			if (c == 27) {
				s->ansi_state = 1;
			} else if (c == '\r') {
				s->pending_cr = true;
			} else if (c == '\n') {
				s->pending_cr = false;
				compilation_commit_line(s);
			} else if (c == '\b') {
				if (s->pending_cr) {
					s->pending_line_length = 0;
					s->pending_cr = false;
				}
				if (s->pending_line_length > 0) {
					s->pending_line_length--;
					s->pending_line[s->pending_line_length]
					    = '\0';
				}
			} else {
				if (s->pending_cr) {
					s->pending_line_length = 0;
					s->pending_cr = false;
				}
				if (c == '\t' || c >= 32) {
					compilation_append_char(s, c);
				}
			}
			break;

		case 1:
			if (c == '[') {
				s->ansi_state = 2;
			} else if (c == ']') {
				s->ansi_state = 3;
			} else {
				s->ansi_state = 0;
			}
			break;

		case 2:
			if (c >= 0x40 && c <= 0x7E) {
				s->ansi_state = 0;
			}
			break;

		case 3:
			if (c == 0x07) {
				s->ansi_state = 0;
			} else if (c == 27) {
				s->ansi_state = 4;
			}
			break;

		case 4:
			if (c == '\\') {
				s->ansi_state = 0;
			} else {
				s->ansi_state = 3;
			}
			break;
		}
	}

	compilation_mirror_pending(s);
}

int compilation_poll(void)
{
	if (g_compilation.phase == COMPILATION_IDLE) {
		return 0;
	}

	int state_changed = 0;

	if (!g_compilation.pipe_eof && g_compilation.output_fd >= 0) {
		char buf[COMPILATION_READ_CHUNK];
		size_t read_total = 0;
		while (read_total < COMPILATION_TICK_BUDGET) {
#ifdef _WIN32
			ssize_t n = kg_fd_read_available(
			    g_compilation.output_fd, buf, sizeof(buf));
			if (n == -2) {
				break;
			}
#else
			ssize_t n
			    = read(g_compilation.output_fd, buf, sizeof(buf));
#endif
			if (n > 0) {
				compilation_process_bytes(
				    &g_compilation, buf, n);
				read_total += n;
				state_changed = 1;
			} else if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					break;
				}
				if (errno == EINTR) {
					continue;
				}
				g_compilation.pipe_eof = true;
				break;
			} else {
				g_compilation.pipe_eof = true;
				break;
			}
		}
	}

	if (!g_compilation.child_reaped && g_compilation.pid > 0
	    && kg_process_reap(g_compilation.pid, &g_compilation.wait_status)) {
		g_compilation.child_reaped = true;
	}

	if (g_compilation.pipe_eof && g_compilation.child_reaped) {
		/* Remove the mirrored copy of the pending line before
		 * committing it for real, otherwise the final unterminated
		 * line would appear twice. */
		int out_slot
		    = buf_handle_slot(g_compilation.compilation_buffer);

		buf_truncate_last_row(
		    out_slot, g_compilation.displayed_pending_length);
		g_compilation.displayed_pending_length = 0;
		if (g_compilation.pending_cr) {
			g_compilation.pending_line_length = 0;
			g_compilation.pending_cr = false;
		}
		if (g_compilation.pending_line_length > 0) {
			compilation_commit_line(&g_compilation);
		}

		char msg[128];
		int msg_len;
		if (g_compilation.truncated
		    && !g_compilation.truncation_marker_written) {
			msg_len = snprintf(msg, sizeof(msg),
			    "[kg: compilation output truncated after %zu "
			    "bytes]\n",
			    g_compilation.maximum_output);
			buf_append_special_text(out_slot, msg, msg_len);
			g_compilation.truncation_marker_written = true;
		}

		buf_append_special_text(out_slot, "\n", 1);

		if (g_compilation.wait_status.exited) {
			int code = g_compilation.wait_status.exit_code;
			msg_len = snprintf(msg, sizeof(msg),
			    "Compilation finished with exit code %d\n", code);
			editor_set_status_message(
			    "Compilation finished with exit code %d", code);
		} else if (g_compilation.wait_status.signal_number) {
			int sig = g_compilation.wait_status.signal_number;
			msg_len = snprintf(msg, sizeof(msg),
			    "Compilation terminated by signal %d\n", sig);
			editor_set_status_message(
			    "Compilation terminated by signal %d", sig);
		} else {
			msg_len = snprintf(
			    msg, sizeof(msg), "Compilation finished\n");
			editor_set_status_message("Compilation finished");
		}
		buf_append_special_text(out_slot, msg, msg_len);

		kg_close_fd(&g_compilation.output_fd);

		if (g_compilation.pending_line) {
			free(g_compilation.pending_line);
			g_compilation.pending_line = NULL;
		}
		g_compilation.pending_line_length = 0;
		g_compilation.pending_line_cap = 0;
		g_compilation.displayed_pending_length = 0;

		/* Clear process/lifecycle identity so a stale confirmation
		 * path cannot resurrect or re-finalize this run, or signal a
		 * since-reused process group. */
		g_compilation.pid = 0;
		g_compilation.process_group = 0;
		g_compilation.pipe_eof = false;
		g_compilation.child_reaped = false;

		g_compilation.phase = COMPILATION_IDLE;
		state_changed = 1;
		programmatic_finish();

		/* A queued restart is only launched from the top-level loop
		 * (see compilation_start_pending_restart), never from here:
		 * compilation_poll runs during minibuffer prompts, and starting
		 * a compilation switches buffers/windows, which must not happen
		 * underneath an unrelated prompt.  A programmatic completion is
		 * only DELIVERED from there (compilation_deliver_completion)
		 * for exactly the same reason -- which is why the call above
		 * records the result and runs nothing. */
	}

	return state_changed;
}

/* ------------------------- the programmatic seam ---------------------- */

/* Turn the run that has just finalized into the result its caller is owed.
 * Built here, at the end of the run, and delivered later: the callback must
 * not run from inside a poll, which is compile.h's contract and the reason
 * this only records. */
static void programmatic_finish(void)
{
	struct compilation_result *r = &g_programmatic.result;

	if (!g_programmatic.attached) {
		return;
	}
	g_programmatic.attached = false;
	memset(r, 0, sizeof(*r));
	r->truncated = g_compilation.truncated;
	if (g_compilation.wait_status.exited) {
		r->status = COMPILATION_DONE_EXITED;
		r->exit_code = g_compilation.wait_status.exit_code;
	} else if (g_compilation.wait_status.signal_number) {
		r->status = COMPILATION_DONE_SIGNALLED;
		r->signal_number = g_compilation.wait_status.signal_number;
	} else {
		/* Collected with neither an exit code nor a signal: the child
		 * was already gone (ECHILD) and nothing is known about how it
		 * went.  Reported as an exit of unknown status rather than as
		 * a signal kg never saw, and -1 is not a code any child can
		 * exit with. */
		r->status = COMPILATION_DONE_EXITED;
		r->exit_code = -1;
	}
	g_programmatic.ready = true;
}

void compilation_deliver_completion(void)
{
	struct compilation_result result = g_programmatic.result;
	compilation_done_fn done = g_programmatic.done;
	void *ctx = g_programmatic.ctx;

	if (!g_programmatic.ready) {
		return;
	}
	/* The slot is cleared BEFORE the callback runs, src/lsp_client.c's
	 * rule for the same reason: a callback may start the next
	 * compilation, and it must not find this one's answer still standing
	 * and be told BUSY by an answer that has already been delivered. */
	g_programmatic.ready = false;
	g_programmatic.generation = 0;
	g_programmatic.done = NULL;
	g_programmatic.ctx = NULL;
	if (done) {
		done(&result, ctx);
	}
}

enum compilation_start_result compilation_start_programmatic(
    const char *command, const char *directory, struct kg_buffer_handle source,
    compilation_done_fn done_fn, void *ctx, unsigned *generation_out)
{
	if (generation_out) {
		*generation_out = 0;
	}
	/* Busy is the whole of the "already running" policy: no prompt, no
	 * SIGINT, no queued restart.  An undelivered completion counts as
	 * busy too, since accepting now would overwrite an answer somebody is
	 * still owed. */
	if (compilation_is_running() || g_programmatic.generation != 0) {
		return COMPILATION_BUSY;
	}
	g_programmatic.generation = g_programmatic_next_generation++;
	if (g_programmatic_next_generation == 0) {
		g_programmatic_next_generation = 1;
	}
	g_programmatic.done = done_fn;
	g_programmatic.ctx = ctx;
	g_programmatic.attached = true;
	g_programmatic.ready = false;
	if (generation_out) {
		*generation_out = g_programmatic.generation;
	}
	/* from_user is false: this start belongs to a caller, not to a user
	 * who has just switched into a buffer, so the window arrangement is
	 * left as the deferred-restart path leaves it. */
	if (!compilation_start(command, directory, source, false)) {
		g_programmatic.attached = false;
		memset(
		    &g_programmatic.result, 0, sizeof(g_programmatic.result));
		g_programmatic.result.status = COMPILATION_DONE_SPAWN_FAILED;
		g_programmatic.ready = true;
	}
	return COMPILATION_ACCEPTED;
}

void compilation_cancel_programmatic(unsigned generation)
{
	if (generation == 0 || g_programmatic.generation != generation) {
		return;
	}
	/* The run itself is left alone: it is the user's compilation now,
	 * finishing into *compilation* as any other does.  Only the promise
	 * to call somebody back is dropped. */
	g_programmatic.generation = 0;
	g_programmatic.done = NULL;
	g_programmatic.ctx = NULL;
	g_programmatic.attached = false;
	g_programmatic.ready = false;
}

/* The editor is going away with a callback still owed.  It is delivered
 * here rather than dropped, so a caller can release what it owns -- and a
 * result that was already recorded is delivered as itself, since a run that
 * finished before the editor did has an answer worth more than
 * "cancelled". */
static void programmatic_abandon(void)
{
	if (g_programmatic.generation == 0) {
		return;
	}
	if (!g_programmatic.ready) {
		memset(
		    &g_programmatic.result, 0, sizeof(g_programmatic.result));
		g_programmatic.result.status = COMPILATION_DONE_CANCELLED;
		g_programmatic.attached = false;
		g_programmatic.ready = true;
	}
	compilation_deliver_completion();
}

/* Launch a restart that a previous compile/recompile deferred until the
 * running child exited. Called only from top-level input loops, where
 * changing the displayed buffer/window is safe. */
void compilation_start_pending_restart(void)
{
	if (g_compilation.restart_pending
	    && g_compilation.phase == COMPILATION_IDLE) {
		g_compilation.restart_pending = false;
		(void)compilation_start(g_compilation.pending_command,
		    g_compilation.pending_directory,
		    g_compilation.pending_source_buffer, false);
	}
}

int compilation_is_running(void)
{
	return g_compilation.phase != COMPILATION_IDLE;
}

void editor_kill_compilation(int fd)
{
	(void)fd;
	if (g_compilation.phase == COMPILATION_IDLE) {
		editor_set_status_message("No active compilation process");
		return;
	}

	/* A run whose group has been cleared has already been finalized:
	 * there is nothing left to signal, and -0 would mean kg's own
	 * group. */
	if (g_compilation.process_group <= 0) {
		return;
	}

	if (g_compilation.phase == COMPILATION_TERMINATING) {
		kg_process_signal_group(g_compilation.process_group, SIGKILL);
		editor_set_status_message(
		    "Sent SIGKILL to compilation process group");
		return;
	}

	kg_process_signal_group(g_compilation.process_group, SIGINT);
	g_compilation.phase = COMPILATION_TERMINATING;
	editor_set_status_message("Sent SIGINT to compilation process "
				  "group (repeat to SIGKILL)");
}

/* ----------------------------- the output feed ------------------------ */

/* One feed at a time, and never beside a real compilation: both write the
 * same buffer, and compile.h says which one wins. */
static struct compilation_state g_feed;
static bool g_feed_open;

static void feed_release(void)
{
	free(g_feed.pending_line);
	memset(&g_feed, 0, sizeof(g_feed));
	g_feed_open = false;
}

bool compilation_feed_begin(const char *label, const char *directory)
{
	char header[PATH_MAX + KG_COMPILE_COMMAND_MAX + 128];
	int header_len;
	int cidx;

	if (compilation_is_running()) {
		return false;
	}
	feed_release();
	cidx
	    = buf_prepare_special_text("*compilation*", &compilation_syntax, 1);
	if (cidx < 0) {
		return false;
	}
	compilation_stream_reset(&g_feed, g_default_maximum_output);
	g_feed.compilation_buffer = buf_handle(cidx);
	/* The same header an ordinary compilation writes, because what
	 * follows is read the same way and the directory line is what makes
	 * a relative diagnostic resolvable. */
	header_len = snprintf(header, sizeof(header),
	    "Compilation started in %s\n\n$ %s\n\n", directory, label);
	buf_append_special_text(cidx, header, header_len);
	g_feed.committed_len += (size_t)header_len;
	compilation_report_diag_reset(g_feed.compilation_buffer, directory);
	/* A feed arrives from a poll callback, which runs underneath
	 * minibuffer prompts.  The buffer and the next-error store take the
	 * diagnostics either way -- C-x ` works from wherever the user is --
	 * but rearranging windows under an unrelated question is the hazard
	 * the completion-callback machinery above exists to avoid, so the
	 * raise is skipped while a prompt is standing. */
	if (!kg_event_prompt_active()) {
		win_display_buffer_other_window(cidx);
		win_position_at_end(cidx);
	}
	g_feed_open = true;
	return true;
}

void compilation_feed_bytes(const char *bytes, size_t len)
{
	if (g_feed_open) {
		compilation_process_bytes(&g_feed, bytes, len);
	}
}

void compilation_feed_finish(const char *note)
{
	char msg[192];
	int msg_len;
	int slot;

	if (!g_feed_open) {
		return;
	}
	slot = buf_handle_slot(g_feed.compilation_buffer);
	/* The mirrored copy of the pending line goes before it is committed
	 * for real, or the last unterminated line appears twice -- the same
	 * ordering compilation_poll()'s finalize block keeps. */
	buf_truncate_last_row(slot, g_feed.displayed_pending_length);
	g_feed.displayed_pending_length = 0;
	if (g_feed.pending_line_length > 0) {
		compilation_commit_line(&g_feed);
	}
	if (g_feed.truncated && !g_feed.truncation_marker_written) {
		msg_len = snprintf(msg, sizeof(msg),
		    "[kg: compilation output truncated after %zu bytes]\n",
		    g_feed.maximum_output);
		buf_append_special_text(slot, msg, msg_len);
		g_feed.truncation_marker_written = true;
	}
	msg_len = snprintf(msg, sizeof(msg), "\n%s\n", note ? note : "");
	buf_append_special_text(slot, msg, msg_len);
	feed_release();
}

void compilation_shutdown(void)
{
	feed_release();
	if (g_compilation.phase != COMPILATION_IDLE) {
		kg_close_fd(&g_compilation.output_fd);
		if (g_compilation.process_group > 0) {
			struct kg_process_status status;

			kg_process_signal_group(
			    g_compilation.process_group, SIGKILL);
			for (int i = 0; i < 100; i++) {
				if (kg_process_reap(
					g_compilation.pid, &status)) {
					break;
				}
				usleep(1000);
			}
		}
		if (g_compilation.pending_line) {
			free(g_compilation.pending_line);
			g_compilation.pending_line = NULL;
		}
		g_compilation.pending_line_length = 0;
		g_compilation.pending_line_cap = 0;
		g_compilation.phase = COMPILATION_IDLE;
		g_compilation.restart_pending = false;
	}
	/* Outside the branch above: a caller may be owed a completion for a
	 * run that already finished and has not been delivered yet, and that
	 * run left the phase idle. */
	programmatic_abandon();
}
