#include "compile.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

void editor_compile(int fd)
{
	char prompt[KG_COMPILE_COMMAND_MAX];
	char dir[PATH_MAX];
	int rc;
	struct shell_capture_result cap;
	char *transcript;
	int source_slot;
	int exited;
	int exit_code;
	int signal_number;

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

	g_compilation.have_last_command = true;
	strncpy(g_compilation.last_command, prompt,
	    sizeof(g_compilation.last_command));
	g_compilation.last_command[sizeof(g_compilation.last_command) - 1]
	    = '\0';
	strncpy(g_compilation.last_directory, dir,
	    sizeof(g_compilation.last_directory));
	g_compilation.last_directory[sizeof(g_compilation.last_directory) - 1]
	    = '\0';
	g_compilation.source_buffer = source_slot;

	memset(&cap, 0, sizeof(cap));
	if (shell_run_capture(prompt, dir, 0, &cap) != 0) {
		editor_set_status_message(
		    "Cannot start compilation: %s", strerror(errno));
		return;
	}

	exited = (int)cap.exited;
	exit_code = cap.exit_code;
	signal_number = cap.signal_number;

	transcript = compilation_format_transcript(
	    prompt, dir, (size_t)(8 * 1024 * 1024), &cap);
	{
		int cidx = buf_replace_special_text("*compilation*",
		    &compilation_syntax, transcript, strlen(transcript), 1);
		if (cidx >= 0) {
			buf_restore_from_slot(source_slot);
			win_display_buffer_other_window(cidx);
		}
	}
	free(transcript);
	free(cap.output);

	if (exited) {
		editor_set_status_message(
		    "Compilation finished with exit code %d", exit_code);
	} else {
		editor_set_status_message(
		    "Compilation terminated by signal %d", signal_number);
	}
}

void editor_recompile(int fd)
{
	char dir[PATH_MAX];
	const char *command;
	struct shell_capture_result cap;
	char *transcript;
	int source_slot;
	int exited;
	int exit_code;
	int signal_number;

	(void)fd;

	if (editor.filename && strcmp(editor.filename, "*compilation*") == 0) {
		if (!g_compilation.have_last_command) {
			editor_set_status_message("No compile command");
			return;
		}
		command = g_compilation.last_command;
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

	g_compilation.have_last_command = true;
	strncpy(g_compilation.last_command, command,
	    sizeof(g_compilation.last_command));
	g_compilation.last_command[sizeof(g_compilation.last_command) - 1]
	    = '\0';
	strncpy(g_compilation.last_directory, dir,
	    sizeof(g_compilation.last_directory));
	g_compilation.last_directory[sizeof(g_compilation.last_directory) - 1]
	    = '\0';
	g_compilation.source_buffer = source_slot;

	memset(&cap, 0, sizeof(cap));
	if (shell_run_capture(command, dir, 0, &cap) != 0) {
		editor_set_status_message(
		    "Cannot start compilation: %s", strerror(errno));
		return;
	}

	exited = (int)cap.exited;
	exit_code = cap.exit_code;
	signal_number = cap.signal_number;

	transcript = compilation_format_transcript(
	    command, dir, (size_t)(8 * 1024 * 1024), &cap);
	{
		int cidx = buf_replace_special_text("*compilation*",
		    &compilation_syntax, transcript, strlen(transcript), 1);
		if (cidx >= 0) {
			buf_restore_from_slot(source_slot);
			win_display_buffer_other_window(cidx);
		}
	}
	free(transcript);
	free(cap.output);

	if (exited) {
		editor_set_status_message(
		    "Compilation finished with exit code %d", exit_code);
	} else {
		editor_set_status_message(
		    "Compilation terminated by signal %d", signal_number);
	}
}
