#ifndef KG_COMPILE_H
#define KG_COMPILE_H

#include "localvars.h"
#include <limits.h>
#include <stddef.h>
struct shell_capture_result;

struct compilation_state {
	bool have_last_command;
	char last_command[KG_COMPILE_COMMAND_MAX];
	char last_directory[PATH_MAX];
	int source_buffer; /* buflist index of the buffer that launched it */
};

int compilation_resolve_directory(
    const char *filename, char *directory, size_t directory_size);

char *compilation_format_transcript(const char *command, const char *directory,
    size_t maximum_output, const struct shell_capture_result *cap);

void editor_compile(int fd);
void editor_recompile(int fd);

#endif
