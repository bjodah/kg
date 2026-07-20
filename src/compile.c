#include "compile.h"
#include "def.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define COMPILATION_READ_CHUNK 4096
#define COMPILATION_TICK_BUDGET (64 * 1024)

static struct compilation_state g_compilation;

int compilation_resolve_directory(
    const char *filename, char *directory, size_t directory_size)
{
	const char *slash;
	size_t dirlen;

	if (!filename || is_special_buffer(filename)) {
		if (!getcwd(directory, directory_size)) {
			return -1;
		}
		return 0;
	}

	slash = strrchr(filename, '/');
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

char *compilation_format_transcript(const char *command, const char *directory,
    size_t maximum_output, const struct shell_capture_result *cap)
{
	const char *output = cap->output ? cap->output : "";
	size_t outlen = cap->output ? cap->output_length : 0;
	int need_newline = (outlen > 0 && output[outlen - 1] != '\n');
	size_t sz;
	char *buf, *pos;

	sz = 0;
	sz += (size_t)snprintf(NULL, 0, "Compilation started in %s\n\n$ %s\n\n",
	    directory, command);
	sz += outlen;
	if (need_newline) {
		sz += 1;
	}
	if (cap->truncated) {
		sz += (size_t)snprintf(NULL, 0,
		    "[kg: compilation output truncated after %zu bytes]\n",
		    maximum_output);
	}
	sz += 1; /* blank line before finish */
	if (cap->exited) {
		sz += (size_t)snprintf(NULL, 0,
		    "Compilation finished with exit code %d\n", cap->exit_code);
	} else {
		sz += (size_t)snprintf(NULL, 0,
		    "Compilation terminated by signal %d\n",
		    cap->signal_number);
	}

	buf = malloc(sz + 1);
	if (!buf) {
		return NULL;
	}

	pos = buf;
	pos += snprintf(pos, sz + 1 - (size_t)(pos - buf),
	    "Compilation started in %s\n\n$ %s\n\n", directory, command);

	memcpy(pos, output, outlen);
	pos += outlen;

	if (need_newline) {
		*pos++ = '\n';
	}

	if (cap->truncated) {
		pos += snprintf(pos, sz + 1 - (size_t)(pos - buf),
		    "[kg: compilation output truncated after %zu bytes]\n",
		    maximum_output);
	}

	*pos++ = '\n';

	if (cap->exited) {
		pos += snprintf(pos, sz + 1 - (size_t)(pos - buf),
		    "Compilation finished with exit code %d\n", cap->exit_code);
	} else {
		pos += snprintf(pos, sz + 1 - (size_t)(pos - buf),
		    "Compilation terminated by signal %d\n",
		    cap->signal_number);
	}

	return buf;
}

static int compilation_spawn(const char *command, const char *directory,
    pid_t *pid_out, int *output_fd_out)
{
	int p[2];
	if (pipe(p) < 0) {
		return -1;
	}

	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	fcntl(p[1], F_SETFD, FD_CLOEXEC);

	pid_t pid = fork();
	if (pid < 0) {
		int saved_errno = errno;
		close(p[0]);
		close(p[1]);
		errno = saved_errno;
		return -1;
	}

	if (pid == 0) {
		setpgid(0, 0);

		if (directory && chdir(directory) < 0) {
			_exit(127);
		}

		int dn = open("/dev/null", O_RDONLY);
		if (dn >= 0) {
			dup2(dn, STDIN_FILENO);
			close(dn);
		}

		dup2(p[1], STDOUT_FILENO);
		dup2(p[1], STDERR_FILENO);

		close(p[0]);
		close(p[1]);

		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}

	close(p[1]);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	setpgid(pid, pid);

	*pid_out = pid;
	*output_fd_out = p[0];
	return 0;
}

/* Start a compilation of COMMAND in DIRECTORY. compilation_start is the
 * single place that records the command/directory that actually started, so
 * an ordinary start and a deferred restart both leave last_command and
 * last_directory consistent for a later recompile. When from_user is true the
 * caller has just switched into source_buffer, so focus is returned there;
 * for a deferred restart (launched from the top-level loop) we keep whatever
 * buffer the user currently occupies and only show output in another window. */
static void compilation_start(const char *command, const char *directory,
    int source_buffer, bool from_user)
{
	int cidx
	    = buf_prepare_special_text("*compilation*", &compilation_syntax, 1);
	if (cidx < 0) {
		editor_set_status_message(
		    "Failed to prepare compilation buffer");
		return;
	}

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
	g_compilation.compilation_buffer = cidx;
	g_compilation.stored_output = 0;
	g_compilation.maximum_output = 8 * 1024 * 1024;
	g_compilation.truncated = false;
	g_compilation.truncation_marker_written = false;
	g_compilation.pipe_eof = false;
	g_compilation.child_reaped = false;
	g_compilation.wait_status = 0;
	g_compilation.ansi_state = 0;
	g_compilation.pending_cr = false;

	if (g_compilation.pending_line) {
		free(g_compilation.pending_line);
	}
	g_compilation.pending_line = NULL;
	g_compilation.pending_line_length = 0;
	g_compilation.pending_line_cap = 0;
	g_compilation.displayed_pending_length = 0;

	pid_t pid;
	int out_fd;
	if (compilation_spawn(command, directory, &pid, &out_fd) != 0) {
		editor_set_status_message(
		    "Cannot start compilation: %s", strerror(errno));
		g_compilation.phase = COMPILATION_IDLE;
		return;
	}

	g_compilation.pid = pid;
	g_compilation.process_group = pid;
	g_compilation.output_fd = out_fd;

	char header[PATH_MAX + KG_COMPILE_COMMAND_MAX + 128];
	int header_len = snprintf(header, sizeof(header),
	    "Compilation started in %s\n\n$ %s\n\n", directory, command);
	buf_append_special_text(cidx, header, header_len);

	if (from_user) {
		buf_save_current_state();
		buf_restore_from_slot(source_buffer);
	}
	win_display_buffer_other_window(cidx);
	win_position_at_end(cidx);

	editor_set_status_message("Compilation started: %s", command);
}

void editor_compile(int fd)
{
	char prompt[KG_COMPILE_COMMAND_MAX];
	char dir[PATH_MAX];
	int rc;
	int source_slot;

	strncpy(prompt, editor.compile_command, sizeof(prompt));
	prompt[sizeof(prompt) - 1] = '\0';

	rc = editor_read_line(fd, "Compile command: ", prompt, sizeof(prompt));
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

	strncpy(editor.compile_command, prompt, sizeof(editor.compile_command));
	editor.compile_command[sizeof(editor.compile_command) - 1] = '\0';
	editor.compile_command_user_override = 1;
	buf_save_current_state();
	source_slot = buf_current;

	if (compilation_resolve_directory(editor.filename, dir, sizeof(dir))
	    != 0) {
		if (!getcwd(dir, sizeof(dir))) {
			strcpy(dir, ".");
		}
	}

	if (compilation_is_running()) {
		char check_prompt[256];
		snprintf(check_prompt, sizeof(check_prompt),
		    "A compilation process is running; kill it? (y/n) ");
		editor_set_status_message("%s", check_prompt);
		editor_refresh_screen();
		int ans = editor_read_key(fd);
		if (ans != 'y' && ans != 'Y') {
			editor_set_status_message("Compilation not started");
			return;
		}

		/* editor_read_key() polls the compilation while it waits for
		 * the answer, so it may have finished and gone idle meanwhile.
		 * Only signal/defer if it is genuinely still running; otherwise
		 * start the new command directly to avoid signalling a stale
		 * (possibly reused) process group. */
		if (compilation_is_running()) {
			if (g_compilation.process_group > 0) {
				kill(-g_compilation.process_group, SIGINT);
			}
			g_compilation.phase = COMPILATION_TERMINATING;
			g_compilation.restart_pending = true;
			strncpy(g_compilation.pending_command, prompt,
			    sizeof(g_compilation.pending_command));
			g_compilation.pending_command[sizeof(
						g_compilation.pending_command)
			    - 1]
			    = '\0';
			strncpy(g_compilation.pending_directory, dir,
			    sizeof(g_compilation.pending_directory));
			g_compilation.pending_directory[sizeof(
						g_compilation
						    .pending_directory)
			    - 1]
			    = '\0';
			g_compilation.pending_source_buffer = source_slot;

			editor_set_status_message("Sent SIGINT to active "
						  "compilation, restart "
						  "pending...");
			return;
		}
	}

	compilation_start(prompt, dir, source_slot, true);
}

void editor_recompile(int fd)
{
	char dir[PATH_MAX];
	char cmd_buf[KG_COMPILE_COMMAND_MAX];
	const char *command;
	int source_slot;

	if (editor.filename && strcmp(editor.filename, "*compilation*") == 0) {
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
		command = editor.compile_command;
		if (command[0] == '\0') {
			editor_set_status_message("No compile command");
			return;
		}
		if (compilation_resolve_directory(
			editor.filename, dir, sizeof(dir))
		    != 0) {
			if (!getcwd(dir, sizeof(dir))) {
				strcpy(dir, ".");
			}
		}
	}

	buf_save_current_state();
	source_slot = buf_current;

	if (compilation_is_running()) {
		char check_prompt[256];
		snprintf(check_prompt, sizeof(check_prompt),
		    "A compilation process is running; kill it? (y/n) ");
		editor_set_status_message("%s", check_prompt);
		editor_refresh_screen();
		int ans = editor_read_key(fd);
		if (ans != 'y' && ans != 'Y') {
			editor_set_status_message("Compilation not started");
			return;
		}

		/* See editor_compile(): the compilation may have finished while
		 * editor_read_key() polled it, so re-check before signalling. */
		if (compilation_is_running()) {
			if (g_compilation.process_group > 0) {
				kill(-g_compilation.process_group, SIGINT);
			}
			g_compilation.phase = COMPILATION_TERMINATING;
			g_compilation.restart_pending = true;
			strncpy(g_compilation.pending_command, command,
			    sizeof(g_compilation.pending_command));
			g_compilation.pending_command[sizeof(
						g_compilation.pending_command)
			    - 1]
			    = '\0';
			strncpy(g_compilation.pending_directory, dir,
			    sizeof(g_compilation.pending_directory));
			g_compilation.pending_directory[sizeof(
						g_compilation
						    .pending_directory)
			    - 1]
			    = '\0';
			g_compilation.pending_source_buffer = source_slot;

			editor_set_status_message("Sent SIGINT to active "
						  "compilation, restart "
						  "pending...");
			return;
		}
	}

	compilation_start(command, dir, source_slot, true);
}

static void compilation_append_char(struct compilation_state *s, char c)
{
	if (s->pending_line_length + 1 >= s->pending_line_cap) {
		size_t new_cap
		    = s->pending_line_cap == 0 ? 128 : s->pending_line_cap * 2;
		char *new_buf = realloc(s->pending_line, new_cap);
		if (!new_buf) {
			return;
		}
		s->pending_line = new_buf;
		s->pending_line_cap = new_cap;
	}
	s->pending_line[s->pending_line_length++] = c;
	s->pending_line[s->pending_line_length] = '\0';
}

/* Commit the completed pending line (plus its terminating newline) to the
 * buffer as permanent output and reset the pending line. Only committed bytes
 * are charged against the output cap, and each real byte is charged exactly
 * once here, so a line split across several reads is never double-counted.
 * The caller must have already removed any mirrored copy of the pending line
 * from the buffer's last row (see displayed_pending_length). */
static void compilation_commit_line(struct compilation_state *s)
{
	if (s->stored_output < s->maximum_output) {
		size_t rem = s->maximum_output - s->stored_output;
		size_t to_add = s->pending_line_length;
		if (to_add > rem) {
			to_add = rem;
			s->truncated = true;
		}
		if (to_add > 0) {
			buf_append_special_text(
			    s->compilation_buffer, s->pending_line, to_add);
		}
		buf_append_special_text(s->compilation_buffer, "\n", 1);
		s->stored_output += to_add;
	} else if (s->pending_line_length > 0) {
		s->truncated = true;
	}
	s->pending_line_length = 0;
	if (s->pending_line) {
		s->pending_line[0] = '\0';
	}
}

/* Mirror the still-incomplete pending line into the buffer's last row so the
 * user sees partial progress. These bytes are transient: they are removed
 * (via displayed_pending_length) before the next chunk is processed and are
 * not charged against the cap until the line is actually committed. */
static void compilation_mirror_pending(struct compilation_state *s)
{
	size_t to_show = 0;
	if (s->stored_output < s->maximum_output) {
		size_t rem = s->maximum_output - s->stored_output;
		to_show = s->pending_line_length < rem ? s->pending_line_length
						       : rem;
		if (to_show > 0) {
			buf_append_special_text(
			    s->compilation_buffer, s->pending_line, to_show);
		}
	}
	s->displayed_pending_length = to_show;
}

static void compilation_process_bytes(
    struct compilation_state *s, const char *bytes, size_t len)
{
	if (len == 0) {
		return;
	}

	buf_truncate_last_row(
	    s->compilation_buffer, s->displayed_pending_length);
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
			ssize_t n
			    = read(g_compilation.output_fd, buf, sizeof(buf));
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

	if (!g_compilation.child_reaped && g_compilation.pid > 0) {
		int status;
		pid_t wpid = waitpid(g_compilation.pid, &status, WNOHANG);
		if (wpid == g_compilation.pid) {
			g_compilation.child_reaped = true;
			g_compilation.wait_status = status;
		} else if (wpid < 0 && errno == ECHILD) {
			g_compilation.child_reaped = true;
			g_compilation.wait_status = 0;
		}
	}

	if (g_compilation.pipe_eof && g_compilation.child_reaped) {
		/* Remove the mirrored copy of the pending line before
		 * committing it for real, otherwise the final unterminated
		 * line would appear twice. */
		buf_truncate_last_row(g_compilation.compilation_buffer,
		    g_compilation.displayed_pending_length);
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
			buf_append_special_text(
			    g_compilation.compilation_buffer, msg, msg_len);
			g_compilation.truncation_marker_written = true;
		}

		buf_append_special_text(
		    g_compilation.compilation_buffer, "\n", 1);

		if (WIFEXITED(g_compilation.wait_status)) {
			int code = WEXITSTATUS(g_compilation.wait_status);
			msg_len = snprintf(msg, sizeof(msg),
			    "Compilation finished with exit code %d\n", code);
			editor_set_status_message(
			    "Compilation finished with exit code %d", code);
		} else if (WIFSIGNALED(g_compilation.wait_status)) {
			int sig = WTERMSIG(g_compilation.wait_status);
			msg_len = snprintf(msg, sizeof(msg),
			    "Compilation terminated by signal %d\n", sig);
			editor_set_status_message(
			    "Compilation terminated by signal %d", sig);
		} else {
			msg_len = snprintf(
			    msg, sizeof(msg), "Compilation finished\n");
			editor_set_status_message("Compilation finished");
		}
		buf_append_special_text(
		    g_compilation.compilation_buffer, msg, msg_len);

		if (g_compilation.output_fd >= 0) {
			close(g_compilation.output_fd);
			g_compilation.output_fd = -1;
		}

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

		/* A queued restart is only launched from the top-level loop
		 * (see compilation_start_pending_restart), never from here:
		 * compilation_poll runs during minibuffer prompts, and starting
		 * a compilation switches buffers/windows, which must not happen
		 * underneath an unrelated prompt. */
	}

	return state_changed;
}

/* Launch a restart that a previous compile/recompile deferred until the
 * running child exited. Called only from top-level input loops, where
 * changing the displayed buffer/window is safe. */
void compilation_start_pending_restart(void)
{
	if (g_compilation.restart_pending
	    && g_compilation.phase == COMPILATION_IDLE) {
		g_compilation.restart_pending = false;
		compilation_start(g_compilation.pending_command,
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

	if (g_compilation.phase == COMPILATION_TERMINATING) {
		if (g_compilation.process_group > 0) {
			kill(-g_compilation.process_group, SIGKILL);
			editor_set_status_message(
			    "Sent SIGKILL to compilation process group");
		}
		return;
	}

	if (g_compilation.process_group > 0) {
		kill(-g_compilation.process_group, SIGINT);
		g_compilation.phase = COMPILATION_TERMINATING;
		editor_set_status_message("Sent SIGINT to compilation process "
					  "group (repeat to SIGKILL)");
	}
}

void compilation_shutdown(void)
{
	if (g_compilation.phase != COMPILATION_IDLE) {
		if (g_compilation.output_fd >= 0) {
			close(g_compilation.output_fd);
			g_compilation.output_fd = -1;
		}
		if (g_compilation.process_group > 0) {
			kill(-g_compilation.process_group, SIGKILL);
			int status;
			for (int i = 0; i < 100; i++) {
				pid_t wpid = waitpid(
				    g_compilation.pid, &status, WNOHANG);
				if (wpid == g_compilation.pid
				    || (wpid < 0 && errno == ECHILD)) {
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
}
