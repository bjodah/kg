/* Kilo -- A very simple editor in less than 1-kilo lines of code (as counted
 *         by "cloc"). Does not depend on libcurses, directly emits VT100
 *         escapes on the terminal.
 *
 * -----------------------------------------------------------------------
 *
 * Copyright (C) 2016 Salvatore Sanfilippo <antirez at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "compile.h"
#include "def.h"
#include "lisp.h"

struct editor_config editor;
int running = 1;
int kg_exit_status = 0;
int suppress_undo = 0;
int global_auto_revert = 0;
int electric_pairs = 0;

void init_editor(void)
{
	editor.cx = 0;
	editor.cy = 0;
	editor.rowoff = 0;
	editor.coloff = 0;
	editor.numrows = 0;
	editor.row = NULL;
	editor.row_capacity = 0;
	editor.dirty = 0;
	editor.filename = NULL;
	editor.syntax = NULL;
	editor.cx_prefix = 0;
	editor.cc_prefix = 0;
	editor.paste_mode = 0;
	editor.mark_set = 0;
	editor.mark_row = 0;
	editor.mark_col = 0;
	editor.mark_highlight = 0;
	editor.shift_select = 0;
	editor.rect_mode = 0;
	editor.rect_prefix = 0;
	editor.desired_visual_col = -1;
	editor.readonly = 0;
	editor.readonly_local = 0;
	editor.readonly_override = -1;
	editor.compile_command[0] = '\0';
	editor.compile_command_user_override = 0;
	editor.echo_cursor_col = 0;
	memset(&editor.disk, 0, sizeof(editor.disk));
	editor.disk_changed = 0;
	editor.auto_revert = 0;
	editor.visual_line_mode = 0;
	editor.rowoff_visual = 0;
	editor.prefix_pending = 0;
	editor.prefix_arg = 0;
	editor.prefix_no_digits = 0;
	editor.window_line_state = 0;
	gettimeofday(&editor.last_char_time, NULL);
	kill_ring_init();
	undo_init();
	update_window_size();
	win_init();
	atexit(editor_cleanup);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_sig_winch;
	if (sigemptyset(&sa.sa_mask) == -1
	    || sigaction(SIGWINCH, &sa, NULL) == -1) {
		perror("sigaction SIGWINCH");
		exit(1);
	}
}

static int usage(FILE *fp, int rc)
{
	fprintf(fp,
	    "Usage: kg [-QRVh] [file ...]\n"
	    "\n"
	    "Options:\n"
	    "  -Q  Do not load the Lisp init file\n"
	    "  -R  Open file(s) read-only\n"
	    "  -V  Print version and exit\n"
	    "  -h  Print this help and exit\n");
	return rc;
}

int main(int argc, char **argv)
{
	int opt, readonly = 0, no_init = 0;

	while ((opt = getopt(argc, argv, "QRVh")) != -1) {
		switch (opt) {
		case 'Q':
			no_init = 1;
			break;
		case 'R':
			readonly = 1;
			break;
		case 'V':
			printf("kg %s %s\n", KG_VERSION,
			    kg_lisp_active() ? "+lisp" : "-lisp");
			return 0;
		case 'h':
			return usage(stdout, 0);
		default:
			return usage(stderr, 1);
		}
	}

	init_editor();
	if (kg_lisp_active() && kg_lisp_init() != 0) {
		fprintf(stderr, "kg: cannot initialize Lisp: %s\n",
		    kg_lisp_last_error());
		return 1;
	}
	kg_lisp_set_interrupt_check(editor_check_quit_pending);
	buf_load_args(argc - optind, argv + optind, readonly);
	enable_raw_mode(STDIN_FILENO);
	/* The greeting is set before init-file loading so a load error is
	 * not overwritten by it. */
	editor_set_status_message("Press Ctrl-h for help");
	if (!no_init && kg_lisp_load_init() != 0) {
		editor_set_status_message(
		    "Init file error: %s", kg_lisp_last_error());
	}
	while (running) {
		editor_process_pending_signals();
		compilation_poll();
		compilation_start_pending_restart();
		autorevert_poll();
		/* Skip the redraw while a paste floods stdin, so it costs
		 * a handful of refreshes instead of one per pasted byte. */
		if (!editor_input_flood(STDIN_FILENO)) {
			editor_refresh_screen();
		}
		editor_process_keypress(STDIN_FILENO);
	}
	/* Nothing in a default build: KG_PERF_COUNTERS is off and this is a
	 * no-op macro.  A counting build writes its counters to
	 * $KG_PERF_OUT here, which is the only place that knows the session
	 * is over. */
	kg_perf_dump();
	return kg_exit_status;
}
