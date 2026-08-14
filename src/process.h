#ifndef KG_PROCESS_H
#define KG_PROCESS_H

#include "platform.h"

/* Child processes kg starts on the user's behalf.  Most are a `/bin/sh -c`
 * command whose output kg reads from a pipe: M-x compile streams it into
 * *compilation*, M-! and M-| collect it.  A caller that already has an
 * argv (a Lisp `start-process`, not yet `start-shell-command`) may exec it
 * directly instead, with no shell in between.  This module owns the parts
 * that are the same either way -- the CLOEXEC pipe, the fork, the process
 * group, the redirections, and reaping -- and nothing about how the output
 * is read, which is what actually differs between the callers. */

struct kg_spawn_request {
	/* Passed to /bin/sh -c, so the user's pipes and redirects work.
	 * Ignored when `argv` is set. */
	const char *command;
	/* Explicit argv, NULL-terminated, or NULL to use `command`.  When
	 * this is set the child is exec'd directly (via execvp(), so PATH
	 * is searched but nothing else is): no shell, so no word splitting,
	 * globbing, redirection or substitution can happen to an argument a
	 * caller passed as one string.  A request with both `argv` and
	 * `command` set is a caller bug and is refused by kg_process_spawn()
	 * rather than guessed at. */
	const char *const *argv;
	/* chdir() here before exec, or NULL to inherit kg's directory. */
	const char *directory;
	/* The child's stdin: a descriptor to dup, or -1 for /dev/null. */
	int stdin_fd;
	/* Where the child's stderr goes: the output pipe, or /dev/null. */
	bool stderr_to_output;
	/* O_NONBLOCK on the read end the caller gets back. */
	bool nonblocking_output;
	/* The child gives up kg's controlling terminal, and so does
	 * everything it starts -- a controlling terminal is inherited, and
	 * this is the one thing a process group does NOT isolate.  A debug
	 * adapter is the caller that needs it: its debuggee is spawned into
	 * whatever kg handed the adapter, and debugpy's launcher hands the
	 * terminal's foreground process group to that debuggee (measured).
	 * kg is then a background process group on its own terminal, its
	 * read() returns EIO the moment the program finishes, and the editor
	 * treats that as end of input and exits -- at the end of every
	 * completed debug run.
	 *
	 * Off by default: a compilation or a shell command shares the
	 * editor's terminal deliberately.  On for spawned debug adapters
	 * (src/dap_transport.c). */
	bool detach_terminal;
	/* The child leads its own process group, so a signal reaches the
	 * command's own children too.  Already the behaviour; named here
	 * because delete-process depends on it. */
};

/* Start `req->command`, or `req->argv` when it is set (see its comment).
 * On success returns 0, stores the child's pid and the read end of its
 * output pipe, and the child is a process group leader whose id is that pid
 * -- so kg_process_signal_group() reaches the command's own children too.
 * Returns -1 with errno set: from pipe() or fork(), or EINVAL when `req`
 * sets both `argv` and `command`, checked before either fork or pipe.
 *
 * Every other descriptor kg holds must already be CLOEXEC: the child is
 * exec'd immediately and closes only the fds named here. */
int kg_process_spawn(
    const struct kg_spawn_request *req, pid_t *pid_out, int *output_fd_out);

/* Like kg_process_spawn(), but the caller also gets a pipe to the child's
 * standard input, so it can keep talking to a child that keeps answering --
 * the shape a language server needs, and the one thing kg_process_spawn()
 * cannot express (its `stdin_fd` is a descriptor the caller already has,
 * written once and closed).  Every parent-side descriptor comes back
 * O_NONBLOCK and CLOEXEC, because the only caller drives them from a poll
 * loop and must never block the editor on any of them.
 *
 * `stderr_fd_out` is optional and is the third pipe: pass NULL and the
 * child's stderr goes to /dev/null as it always did, pass a pointer and it
 * comes back as a readable descriptor.  It is a separate pipe rather than
 * `stderr_to_output` for the reason that field's own comment gives -- a log
 * line spliced into a protocol stream desynchronises the framing for good
 * -- and it is what lets a server's complaint be read by somebody instead
 * of thrown away.
 *
 * `req->stdin_fd` must be -1: this function creates the child's stdin
 * itself, so a descriptor there is a caller bug and is refused with EINVAL
 * rather than silently ignored -- the field defaults to 0 (kg's own stdin,
 * the terminal) in a designated initializer that forgets it, which is
 * exactly the mistake worth failing loudly on.  `req->nonblocking_output`
 * is likewise ignored: the read end is always non-blocking here.
 *
 * `req->stderr_to_output` is honoured, and for a protocol stream it must
 * be false: a server that writes a log line to stderr would otherwise
 * splice it into the middle of a message and desynchronise the framing.
 * lsp_transport_start_wire() forces it false for that reason.
 *
 * Returns 0, or -1 with errno set (EINVAL, or whatever pipe()/fork()
 * failed with).  Same process group as kg_process_spawn(): the child leads
 * its own, so signalling it reaches the server's own children too. */
int kg_process_spawn_bidi(const struct kg_spawn_request *req, pid_t *pid_out,
    int *stdin_fd_out, int *stdout_fd_out, int *stderr_fd_out);

/* What became of a child, decoded from the wait status the callers would
 * otherwise each pick apart with WIFEXITED and friends.  A collected child
 * either exited, with a code, or died from a signal; the zeroed value
 * claims neither, which is what an unreaped child is worth. */
struct kg_process_status {
	bool exited;
	int exit_code;
	int signal_number;
};

/* Blocking reap, retried across EINTR.  Returns 0 with *status filled in,
 * -1 (status untouched) if the child could not be reaped at all. */
int kg_process_wait(pid_t pid, struct kg_process_status *status);

/* Non-blocking reap.  Returns 1 when the child has been collected -- or was
 * already gone (ECHILD), in which case *status is zeroed -- and 0 while it
 * is still running, leaving *status untouched. */
int kg_process_reap(pid_t pid, struct kg_process_status *status);

/* Send `sig` to every process in group `group`.  A group id of 0 or less is
 * ignored rather than passed to kill(), where it would mean kg's own group:
 * a finalized run leaves the field at 0 exactly so it cannot be signalled
 * twice, and that must not turn into suicide. */
void kg_process_signal_group(pid_t group, int sig);

/* A pipe whose two ends are both CLOEXEC.  Returns 0, or -1 with errno. */
int kg_pipe_cloexec(int fds[2]);

/* close() an owned descriptor once, and mark it closed. */
void kg_close_fd(int *fd);

#ifndef _WIN32
/* Overridable by tests to inject EINTR sequences and permanent failures
 * without needing to race a real child process.  Defaults to waitpid(). */
extern pid_t (*kg_process_waitpid_fn)(pid_t pid, int *status, int options);
#endif

#endif /* KG_PROCESS_H */
