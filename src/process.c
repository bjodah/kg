/* ======================= Child process primitives ========================
 *
 * One spawn, one reap, one kill for every command kg runs: see process.h
 * for the contract.  The two clients are compile.c (asynchronous, drains a
 * non-blocking pipe from the main loop) and shell.c (synchronous, pumps
 * stdin and stdout with poll()); both get a child in its own process group,
 * so cancelling one takes the command's children with it. */

#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>

pid_t (*kg_process_waitpid_fn)(pid_t pid, int *status, int options) = waitpid;

void kg_close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

int kg_pipe_cloexec(int fds[2])
{
	if (pipe(fds) < 0) {
		return -1;
	}
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	return 0;
}

/* Exec `req` directly (argv) or through /bin/sh -c (command) -- the two
 * ways a caller may ask for a child, chosen the same way
 * kg_process_spawn() itself chose which to refuse.  Returns only on
 * failure, the same as execvp()/execl() do. */
static void exec_spawn_request(const struct kg_spawn_request *req)
{
	if (req->argv) {
		execvp(req->argv[0], (char *const *)req->argv);
	} else {
		execl("/bin/sh", "sh", "-c", req->command, (char *)NULL);
	}
}

/* The forked child, between fork() and exec.  Nothing here may allocate or
 * touch editor state; `p` is the output pipe, whose write end this end of
 * the fork owns. */
[[noreturn]] static void spawn_child(
    const struct kg_spawn_request *req, const int p[2])
{
	/* Both sides of the fork call setpgid() so neither has to win the
	 * race: the child is a group leader before it can exec, and before
	 * the parent can signal the group. */
	setpgid(0, 0);

	if (req->directory && chdir(req->directory) < 0) {
		_exit(127);
	}

	int null_fd = open("/dev/null",
	    O_RDWR
#ifdef O_CLOEXEC
		| O_CLOEXEC
#endif
	);

	if (req->stdin_fd >= 0) {
		dup2(req->stdin_fd, STDIN_FILENO);
	} else if (null_fd >= 0) {
		dup2(null_fd, STDIN_FILENO);
	}
	dup2(p[1], STDOUT_FILENO);
	if (req->stderr_to_output) {
		dup2(p[1], STDERR_FILENO);
	} else if (null_fd >= 0) {
		dup2(null_fd, STDERR_FILENO);
	}
	if (null_fd >= 0) {
		close(null_fd);
	}
	close(p[0]);
	close(p[1]);

	exec_spawn_request(req);
	_exit(127);
}

/* The pipe/fork/parent-side bookkeeping, unchanged by the argv-or-command
 * decision -- kg_process_spawn() is the one place that decision is made, so
 * pulling this out keeps its own complexity where it already was. */
static int spawn_process(
    const struct kg_spawn_request *req, pid_t *pid_out, int *output_fd_out)
{
	int p[2];
	pid_t pid;

	if (kg_pipe_cloexec(p) < 0) {
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		int saved_errno = errno;
		close(p[0]);
		close(p[1]);
		errno = saved_errno;
		return -1;
	}

	if (pid == 0) {
		spawn_child(req, p);
	}

	close(p[1]);
	if (req->nonblocking_output) {
		fcntl(p[0], F_SETFL, O_NONBLOCK);
	}
	setpgid(pid, pid);

	*pid_out = pid;
	*output_fd_out = p[0];
	return 0;
}

/* Whether `req` names exactly one runnable thing.  Exactly one of
 * argv/command is honoured, argv first; both set is a caller bug, refused
 * before the pipe or the fork rather than guessed at.  An argv with no
 * argv[0] is refused for a blunter reason: execvp(NULL, ...) dereferences
 * the name to search PATH for it and dies on the spot, so an empty vector
 * would fork a child whose only act is to segfault, and the caller would
 * read that back as a program killed by SIGSEGV rather than as the bad
 * request it is. */
static bool spawn_request_is_runnable(const struct kg_spawn_request *req)
{
	if (req->argv) {
		return req->command == NULL && req->argv[0] != NULL;
	}
	return req->command != NULL;
}

int kg_process_spawn(
    const struct kg_spawn_request *req, pid_t *pid_out, int *output_fd_out)
{
	if (!spawn_request_is_runnable(req)) {
		errno = EINVAL;
		return -1;
	}
	return spawn_process(req, pid_out, output_fd_out);
}

/* The one place a raw wait status is taken apart: everything above this
 * module sees the decoded record. */
static void decode_status(int wait_status, struct kg_process_status *out)
{
	*out = (struct kg_process_status) { 0 };
	if (WIFEXITED(wait_status)) {
		out->exited = true;
		out->exit_code = WEXITSTATUS(wait_status);
	} else if (WIFSIGNALED(wait_status)) {
		out->signal_number = WTERMSIG(wait_status);
	}
}

int kg_process_wait(pid_t pid, struct kg_process_status *status)
{
	int wait_status = 0;
	pid_t result;

	do {
		result = kg_process_waitpid_fn(pid, &wait_status, 0);
	} while (result < 0 && errno == EINTR);

	if (result != pid) {
		return -1;
	}
	decode_status(wait_status, status);
	return 0;
}

int kg_process_reap(pid_t pid, struct kg_process_status *status)
{
	int wait_status = 0;
	pid_t result = kg_process_waitpid_fn(pid, &wait_status, WNOHANG);

	if (result == pid) {
		decode_status(wait_status, status);
		return 1;
	}
	if (result < 0 && errno == ECHILD) {
		*status = (struct kg_process_status) { 0 };
		return 1;
	}
	return 0;
}

void kg_process_signal_group(pid_t group, int sig)
{
	if (group > 0) {
		kill(-group, sig);
	}
}
