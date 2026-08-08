#ifndef KG_COMPILE_H
#define KG_COMPILE_H

#include "bufhandle.h"
#include "localvars.h"
#include "process.h"
#include "platform.h"
#include <limits.h>
#include <stddef.h>
#ifndef _WIN32
#include <sys/types.h>
#endif

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

#endif
