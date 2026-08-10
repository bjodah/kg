/* ======================= Child process primitives ========================
 *
 * One spawn, one reap, one kill for every command kg runs: see process.h
 * for the contract.  The two clients are compile.c (asynchronous, drains a
 * non-blocking pipe from the main loop) and shell.c (synchronous, pumps
 * stdin and stdout with poll()); both get a child in its own process group,
 * so cancelling one takes the command's children with it. */

#include "process.h"

#ifdef _WIN32

#include <errno.h>
#include <stdio.h>
#include <string.h>

static HANDLE process_handle(pid_t pid) { return (HANDLE)(intptr_t)pid; }

static int make_command_line(
    const struct kg_spawn_request *req, char **out, size_t *out_size)
{
	const char *const *arg;
	char *line;
	size_t size = 64;
	size_t used = 0;

	if (req->argv) {
		for (arg = req->argv; *arg; arg++) {
			size += strlen(*arg) * 2 + 4;
		}
	} else {
		size += strlen(req->command) + 8;
	}
	line = malloc(size);
	if (!line) {
		errno = ENOMEM;
		return -1;
	}
	if (!req->argv) {
		(void)snprintf(line, size, "/d /s /c %s", req->command);
		*out = line;
		*out_size = size;
		return 0;
	}
	for (arg = req->argv; *arg; arg++) {
		const char *p = *arg;
		bool quote = *p == '\0' || strpbrk(p, " \t\"") != NULL;

		if (used > 0) {
			line[used++] = ' ';
		}
		if (quote) {
			line[used++] = '"';
		}
		while (*p) {
			if (*p == '"') {
				line[used++] = '\\';
			}
			line[used++] = *p++;
		}
		if (quote) {
			line[used++] = '"';
		}
	}
	line[used] = '\0';
	*out = line;
	*out_size = size;
	return 0;
}

static int make_inheritable(int fd)
{
	HANDLE handle = (HANDLE)_get_osfhandle(fd);

	if (handle == INVALID_HANDLE_VALUE
	    || !SetHandleInformation(
		handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		errno = EBADF;
		return -1;
	}
	return 0;
}

static int spawn_process(
    const struct kg_spawn_request *req, pid_t *pid_out, int *output_fd_out)
{
	SECURITY_ATTRIBUTES security
	    = { .nLength = sizeof(security), .bInheritHandle = TRUE };
	STARTUPINFOA startup = { .cb = sizeof(startup) };
	PROCESS_INFORMATION process = { 0 };
	HANDLE output_read = NULL, output_write = NULL;
	HANDLE null_handle = INVALID_HANDLE_VALUE;
	char *command_line = NULL;
	int output_fd;
	int result = -1;
	size_t command_line_size;

	if (make_command_line(req, &command_line, &command_line_size) != 0) {
		return -1;
	}
	(void)command_line_size;
	if (!CreatePipe(&output_read, &output_write, &security, 0)
	    || !SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0)) {
		errno = EIO;
		goto done;
	}
	if (req->stdin_fd >= 0) {
		if (make_inheritable(req->stdin_fd) != 0) {
			goto done;
		}
		startup.hStdInput = (HANDLE)_get_osfhandle(req->stdin_fd);
	} else {
		null_handle = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
		    FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
		    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (null_handle == INVALID_HANDLE_VALUE) {
			errno = EIO;
			goto done;
		}
		startup.hStdInput = null_handle;
	}
	if (!req->stderr_to_output && null_handle == INVALID_HANDLE_VALUE) {
		null_handle = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
		    FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
		    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (null_handle == INVALID_HANDLE_VALUE) {
			errno = EIO;
			goto done;
		}
	}
	startup.hStdOutput = output_write;
	startup.hStdError = req->stderr_to_output ? output_write : null_handle;
	startup.dwFlags = STARTF_USESTDHANDLES;
	if (!CreateProcessA(req->argv ? NULL : "C:\\Windows\\System32\\cmd.exe",
		command_line, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, NULL,
		req->directory, &startup, &process)) {
		errno = EIO;
		goto done;
	}
	CloseHandle(process.hThread);
	CloseHandle(output_write);
	output_write = NULL;
	if (null_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(null_handle);
		null_handle = INVALID_HANDLE_VALUE;
	}
	output_fd
	    = _open_osfhandle((intptr_t)output_read, _O_BINARY | _O_RDONLY);
	if (output_fd < 0) {
		CloseHandle(output_read);
		TerminateProcess(process.hProcess, 127);
		CloseHandle(process.hProcess);
		errno = EIO;
		goto done;
	}
	*pid_out = (pid_t)(intptr_t)process.hProcess;
	*output_fd_out = output_fd;
	result = 0;

done:
	if (output_write) {
		CloseHandle(output_write);
	}
	if (output_read && result != 0) {
		CloseHandle(output_read);
	}
	if (null_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(null_handle);
	}
	free(command_line);
	return result;
}

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

/* Not ported: the Windows spawn above builds its child's handles up front
 * with CreatePipe(), and a second, writable pipe is a different piece of
 * plumbing rather than a flag on this one.  The only caller is the LSP
 * transport, which reports the failure as "no server" -- an editor without
 * a language server, not a broken one -- so this stays a refusal until
 * something on Windows wants it. */
int kg_process_spawn_bidi(const struct kg_spawn_request *req, pid_t *pid_out,
    int *stdin_fd_out, int *stdout_fd_out, int *stderr_fd_out)
{
	(void)req;
	(void)pid_out;
	(void)stdin_fd_out;
	(void)stdout_fd_out;
	(void)stderr_fd_out;
	errno = ENOSYS;
	return -1;
}

void kg_close_fd(int *fd)
{
	if (*fd >= 0) {
		_close(*fd);
		*fd = -1;
	}
}

int kg_process_wait(pid_t pid, struct kg_process_status *status)
{
	HANDLE handle = process_handle(pid);
	DWORD code;

	if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0
	    || !GetExitCodeProcess(handle, &code)) {
		return -1;
	}
	*status = (struct kg_process_status) { .exited = true,
		.exit_code = (int)code };
	CloseHandle(handle);
	return 0;
}

int kg_process_reap(pid_t pid, struct kg_process_status *status)
{
	HANDLE handle = process_handle(pid);
	DWORD code;
	DWORD wait = WaitForSingleObject(handle, 0);

	if (wait == WAIT_TIMEOUT) {
		return 0;
	}
	if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(handle, &code)) {
		return 0;
	}
	*status = (struct kg_process_status) { .exited = true,
		.exit_code = (int)code };
	CloseHandle(handle);
	return 1;
}

void kg_process_signal_group(pid_t group, int sig)
{
	(void)sig;
	if (group > 0) {
		(void)TerminateProcess(process_handle(group), 1);
	}
}

#else

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
 * the fork owns, and `stderr_fd` is a descriptor to put on the child's
 * standard error, or -1 for the request's own policy (the output pipe, or
 * /dev/null).  It is a parameter rather than a field of the request
 * because only kg_process_spawn_bidi() makes such a pipe, and a field
 * defaulting to 0 in a designated initializer would silently mean kg's own
 * stdin -- the exact trap `stdin_fd` documents. */
[[noreturn]] static void spawn_child(
    const struct kg_spawn_request *req, const int p[2], int stderr_fd)
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
	} else if (stderr_fd >= 0) {
		dup2(stderr_fd, STDERR_FILENO);
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
 * pulling this out keeps its own complexity where it already was.
 * `stderr_fd` is spawn_child()'s parameter of the same name, passed
 * through: -1 for every caller but the bidirectional one. */
static int spawn_process(const struct kg_spawn_request *req, int stderr_fd,
    pid_t *pid_out, int *output_fd_out)
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
		spawn_child(req, p, stderr_fd);
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

/* The runnable check and the spawn, with spawn_child()'s `stderr_fd`
 * passed through.  Both public entry points are this function with a
 * different third pipe: none, or one the bidirectional caller made. */
static int spawn_checked(const struct kg_spawn_request *req, int stderr_fd,
    pid_t *pid_out, int *output_fd_out)
{
	if (!spawn_request_is_runnable(req)) {
		errno = EINVAL;
		return -1;
	}
	return spawn_process(req, stderr_fd, pid_out, output_fd_out);
}

int kg_process_spawn(
    const struct kg_spawn_request *req, pid_t *pid_out, int *output_fd_out)
{
	return spawn_checked(req, -1, pid_out, output_fd_out);
}

/* The optional third pipe.  Returns the read end, or -1 -- and -1 is not a
 * failure of the spawn: a server whose complaints cannot be captured is
 * still a server, so the child gets /dev/null on its stderr and the caller
 * is told there is nothing to read. */
static int stderr_pipe_open(int fds[2], int *stderr_fd_out)
{
	if (!stderr_fd_out || kg_pipe_cloexec(fds) < 0) {
		return -1;
	}
	return fds[1];
}

/* Composed out of the same spawn as kg_process_spawn() rather than beside
 * it: the extra descriptor a bidirectional child needs is a pipe whose read
 * end goes in the request's own `stdin_fd`, which is precisely what that
 * field is for and what shell.c already does with it.  So the fork, the
 * exec, the process group, the /dev/null fallbacks and the CLOEXEC
 * discipline are reached, not copied -- every end of the new pipes is
 * CLOEXEC, and the ones the child keeps lose the flag on the dup2() onto
 * fd 0 and fd 2. */
int kg_process_spawn_bidi(const struct kg_spawn_request *req, pid_t *pid_out,
    int *stdin_fd_out, int *stdout_fd_out, int *stderr_fd_out)
{
	struct kg_spawn_request child = *req;
	int in[2];
	int err[2] = { -1, -1 };
	int err_write;
	int saved_errno;

	if (req->stdin_fd >= 0) {
		errno = EINVAL;
		return -1;
	}
	if (kg_pipe_cloexec(in) < 0) {
		return -1;
	}
	err_write = stderr_pipe_open(err, stderr_fd_out);
	child.stdin_fd = in[0];
	child.nonblocking_output = true;
	if (spawn_checked(&child, err_write, pid_out, stdout_fd_out) != 0) {
		saved_errno = errno;
		close(in[0]);
		close(in[1]);
		kg_close_fd(&err[0]);
		kg_close_fd(&err[1]);
		errno = saved_errno;
		return -1;
	}
	close(in[0]);
	kg_close_fd(&err[1]);
	fcntl(in[1], F_SETFL, O_NONBLOCK);
	*stdin_fd_out = in[1];
	if (stderr_fd_out) {
		if (err[0] >= 0) {
			fcntl(err[0], F_SETFL, O_NONBLOCK);
		}
		*stderr_fd_out = err[0];
	}
	return 0;
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

#endif
