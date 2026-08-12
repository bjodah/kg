#ifndef KG_COMPILE_H
#define KG_COMPILE_H

#include "bufhandle.h"
#include "localvars.h"
#include "platform.h"
#include "process.h"
#include <limits.h>
#include <stddef.h>

enum compilation_phase {
	COMPILATION_IDLE,
	COMPILATION_RUNNING,
	COMPILATION_TERMINATING,
};

/* Optional hooks compile.c calls as output streams in: once when a run
 * starts (a fresh compilation buffer and its directory context) and once
 * per line committed to it (the line's own bytes and where they start in
 * the buffer).  Nothing in compile.c names a module that implements
 * these -- compile_nav.c installs itself via compile_nav_install(), which
 * the real editor calls once at startup -- so a caller that never installs
 * anything (test/test_compile.c's lightweight streaming tests) still
 * links and runs with plain, untracked output. */
struct compile_diag_hooks {
	void (*reset)(struct kg_buffer_handle compilation_buffer,
	    const char *initial_cwd, size_t initial_cwd_len);
	void (*ingest_line)(
	    const char *line, size_t len, size_t line_start_pos);
};
void compilation_set_diag_hooks(const struct compile_diag_hooks *hooks);

struct compilation_state {
	enum compilation_phase phase;

	bool have_last_command;
	char last_command[KG_COMPILE_COMMAND_MAX];
	char last_directory[PATH_MAX];

	/* Handles, not slot indices: a compilation outlives many commands,
	 * and either buffer can be killed or have its slot reused while the
	 * child is still writing. */
	struct kg_buffer_handle source_buffer;
	struct kg_buffer_handle compilation_buffer;

	pid_t pid;
	pid_t process_group;
	int output_fd;

	bool pipe_eof;
	bool child_reaped;
	struct kg_process_status wait_status;

	/* One budget for everything the child produced and kg kept:
	 * every byte held in pending_line, every byte committed as a
	 * completed line, and every line terminator retained in
	 * *compilation*.  Bytes are charged when accepted and never
	 * refunded, so \b and \r rubbing them out still costs.  Not
	 * charged: the header compilation_start() writes, the truncation
	 * note and the finish line — editor-generated text bounded by the
	 * command and directory lengths, not by the child. */
	size_t stored_output;
	size_t maximum_output;
	bool truncated;
	bool truncation_marker_written;

	/* Total bytes permanently committed to the compilation buffer so
	 * far: the header, and every committed line plus its terminator.
	 * Unlike stored_output, this is not a budget -- it is the buffer's
	 * own byte length as compile.c itself has built it, tracked here
	 * rather than read back with buffer_byte_length() so compile.c never
	 * has to resolve its buffer handle to a real buffer.  It is what
	 * lets the ingest_line hook above say where a line starts without
	 * this module knowing anything about markers. */
	size_t committed_len;

	char *pending_line;
	size_t pending_line_length;
	size_t pending_line_cap;
	size_t displayed_pending_length;
	int ansi_state;
	bool pending_cr;

	bool restart_pending;
	char pending_command[KG_COMPILE_COMMAND_MAX];
	char pending_directory[PATH_MAX];
	struct kg_buffer_handle pending_source_buffer;
};

int compilation_resolve_directory(
    const char *filename, char *directory, size_t directory_size);

void editor_compile(int fd);
void editor_recompile(int fd);

/* Streaming seams.  compilation_process_bytes() touches no global state: it
 * is exposed, along with the reset and budget setters, so the byte parser and
 * its output budget can be driven directly from tests. */
void compilation_set_maximum_output(size_t bytes);
void compilation_stream_reset(struct compilation_state *s, size_t bytes);
void compilation_process_bytes(
    struct compilation_state *s, const char *bytes, size_t len);

int compilation_poll(void);
void compilation_start_pending_restart(void);
int compilation_is_running(void);
void compilation_shutdown(void);
void editor_kill_compilation(int fd);

/* ------------------------- the programmatic seam ---------------------- */

/* A compilation somebody other than the user asked for, and the one thing
 * M-x compile never had to answer: what happened.
 *
 * It exists for the debugger's optional `build` step
 * (doc/plans/dap/01-protocol.md stage 4), which must run `make` and then
 * launch only if it worked -- but it is an EDITOR seam, not a debugger one:
 * it is compiled in every configuration, it names nothing about debugging,
 * and any later caller that needs "run this and tell me" gets the same
 * three properties.
 *
 * What makes it different from the interactive entry points, and why it is
 * a separate function rather than a flag on one of them:
 *
 *   - It never prompts.  A compilation already running answers
 *     COMPILATION_BUSY and nothing else happens; asking a user "kill it?"
 *     on behalf of a caller who is halfway through starting a debug session
 *     is a question with no good answer.
 *   - The completion callback runs EXACTLY ONCE for every accepted call,
 *     and only at a top-level safe point -- compilation_deliver_completion()
 *     below, which the main loop calls where it already launches deferred
 *     restarts.  Never from inside compilation_poll(): that runs underneath
 *     minibuffer prompts, and a callback that starts a debug session there
 *     would change buffers and windows under an unrelated question.
 *   - A caller that has given up says so by generation
 *     (compilation_cancel_programmatic()), which drops the callback without
 *     touching the run: the build keeps going and *compilation* keeps its
 *     output, because the user's compilation is not the caller's to kill.
 *
 * Everything else about the run is ordinary: the same *compilation* buffer,
 * the same header and finish lines, the same diagnostics hooks, the same
 * C-c C-k. */
enum compilation_done_status {
	/* The child exited; `exit_code` says with what.  Zero is the only
	 * "it worked", and it is the caller's test, not this module's. */
	COMPILATION_DONE_EXITED = 0,
	/* A signal ended it, `signal_number` says which -- including the
	 * SIGINT/SIGKILL of C-c C-k, so a build the user killed is reported
	 * rather than silently unfinished. */
	COMPILATION_DONE_SIGNALLED,
	/* Nothing ever ran: the compilation buffer could not be prepared, or
	 * the child could not be spawned.  Reported through the callback
	 * like every other outcome, so a caller has one completion path
	 * rather than two. */
	COMPILATION_DONE_SPAWN_FAILED,
	/* The editor is shutting down and the run is being abandoned.  The
	 * callback runs so a caller can release what it owns; nothing about
	 * the build is claimed. */
	COMPILATION_DONE_CANCELLED,
};

struct compilation_result {
	enum compilation_done_status status;
	int exit_code;
	int signal_number;
	/* Whether output hit the retained-output budget.  A build whose
	 * diagnostics were cut short still succeeded or failed on its exit
	 * code; this is how a caller knows the buffer is incomplete. */
	bool truncated;
};

/* `ctx` is the caller's and is never touched here beyond being handed
 * back.  It must stay valid until the callback runs or until
 * compilation_cancel_programmatic() has been called with this run's
 * generation -- after either, this module holds nothing of the caller's. */
typedef void (*compilation_done_fn)(
    const struct compilation_result *result, void *ctx);

enum compilation_start_result {
	/* The callback will run exactly once, later. */
	COMPILATION_ACCEPTED = 0,
	/* A compilation is running, or an accepted one's completion has not
	 * been delivered yet.  Nothing was started and the callback will
	 * never run. */
	COMPILATION_BUSY,
};

enum compilation_start_result compilation_start_programmatic(
    const char *command, const char *directory, struct kg_buffer_handle source,
    compilation_done_fn done_fn, void *ctx, unsigned *generation_out);

/* Abandon the completion of `generation`: its callback will not run and
 * this module drops its context pointer.  An unknown or already-delivered
 * generation is ignored, which is what makes it safe to call from a
 * caller's own teardown without remembering whether it raced the
 * delivery. */
void compilation_cancel_programmatic(unsigned generation);

/* Run a completed programmatic run's callback, if there is one waiting.
 * Called from the top-level input loops beside
 * compilation_start_pending_restart(), and from nowhere else. */
void compilation_deliver_completion(void);

#endif
