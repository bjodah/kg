#ifndef KG_COMPILE_H
#define KG_COMPILE_H

#include "localvars.h"
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

struct shell_capture_result;

enum compilation_phase {
	COMPILATION_IDLE,
	COMPILATION_RUNNING,
	COMPILATION_TERMINATING,
};

struct compilation_state {
	enum compilation_phase phase;

	bool have_last_command;
	char last_command[KG_COMPILE_COMMAND_MAX];
	char last_directory[PATH_MAX];

	int source_buffer;
	int compilation_buffer;

	pid_t pid;
	pid_t process_group;
	int output_fd;

	bool pipe_eof;
	bool child_reaped;
	int wait_status;

	size_t stored_output;
	size_t maximum_output;
	bool truncated;
	bool truncation_marker_written;

	char *pending_line;
	size_t pending_line_length;
	size_t pending_line_cap;
	size_t displayed_pending_length;
	int ansi_state;
	bool pending_cr;

	bool restart_pending;
	char pending_command[KG_COMPILE_COMMAND_MAX];
	char pending_directory[PATH_MAX];
	int pending_source_buffer;
};

int compilation_resolve_directory(
    const char *filename, char *directory, size_t directory_size);

char *compilation_format_transcript(const char *command, const char *directory,
    size_t maximum_output, const struct shell_capture_result *cap);

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
