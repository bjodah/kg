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

	execl("/bin/sh", "sh", "-c", req->command, (char *)NULL);
	_exit(127);
}

int kg_process_spawn(
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

int kg_process_wait(pid_t pid, int *status)
{
	pid_t result;

	do {
		result = kg_process_waitpid_fn(pid, status, 0);
	} while (result < 0 && errno == EINTR);

	return result == pid ? 0 : -1;
}

int kg_process_reap(pid_t pid, int *status)
{
	pid_t result = kg_process_waitpid_fn(pid, status, WNOHANG);

	if (result == pid) {
		return 1;
	}
	if (result < 0 && errno == ECHILD) {
		*status = 0;
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
