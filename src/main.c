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
#ifdef _WIN32
#include "platform.h"
#else
#include <sys/time.h>
#include <unistd.h>
#endif

#include "bufhandle.h"
#include "compile.h"
#include "compile_nav.h"
#include "def.h"
#include "event.h"
#include "kbd.h"
#include "lisp.h"
#include "lsp.h"
#include "marker.h"
#include "perf.h"
#include "process_table.h"
#include "register.h"
#include "yank.h"

struct editor_config editor;
int running = 1;
int kg_exit_status = 0;
int global_auto_revert = 0;
int electric_pairs = 0;

void init_editor(void)
{
	wcur()->cx = 0;
	wcur()->cy = 0;
	wcur()->rowoff = 0;
	wcur()->coloff = 0;
	/* bcur()->row/numrows/row_capacity need no reset here: buflist[] is a
	 * static array, zero-initialized before main() runs, and
	 * init_editor() itself runs exactly once, before any buffer has held
	 * a row. */
	bcur()->dirty = 0;
	bcur()->filename = NULL;
	bcur()->syntax = NULL;
	editor.paste_mode = 0;
	bcur()->mark = (struct kg_buffer_mark) { .virtual_chars_col = -1 };
	bcur()->mark_highlight = 0;
	bcur()->mark_ring_len = 0;
	bcur()->shift_select = 0;
	bcur()->rect_mode = 0;
	wcur()->desired_visual_col = -1;
	bcur()->readonly = 0;
	bcur()->readonly_local = 0;
	bcur()->readonly_override = -1;
	bcur()->compile_command[0] = '\0';
	bcur()->compile_command_user_override = 0;
	editor.echo_cursor_col = 0;
	memset(&bcur()->disk, 0, sizeof(bcur()->disk));
	bcur()->disk_changed = 0;
	bcur()->auto_revert = 0;
	bcur()->visual_line_mode = 0;
	wcur()->rowoff_visual = 0;
	editor.prefix_pending = 0;
	editor.prefix_arg = 0;
	editor.prefix_no_digits = 0;
	gettimeofday(&editor.last_char_time, NULL);
	kill_ring_init();
	undo_init();
	update_window_size();
	win_init();
	compile_nav_install();
	kg_process_table_init();
	lsp_init();
	atexit(editor_cleanup);
	/* Registered after editor_cleanup, so it runs before it: a position
	 * register's marker is given back while its buffer is still there. */
	atexit(kg_register_clear);
#ifndef _WIN32
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_sig_winch;
	if (sigemptyset(&sa.sa_mask) == -1
	    || sigaction(SIGWINCH, &sa, NULL) == -1) {
		perror("sigaction SIGWINCH");
		exit(1);
	}
#endif
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

/* The feature words `kg -V` prints, one space-separated +word/-word per
 * build axis.  Lisp answers at run time (kg_lisp_active()); the syntax
 * backend is a compile-time source-list choice with no run-time question to
 * ask, so it answers from the macro the Makefile defines.  The spelling
 * matters beyond the eye: utils/pty_accept.py takes every +word here as a
 * feature name, which is what makes `requires_feature: tree-sitter` in a
 * PTY case skip on a build that has none. */
#ifdef KG_USE_TREE_SITTER
#define KG_FEATURE_TREE_SITTER "+tree-sitter"
#else
#define KG_FEATURE_TREE_SITTER "-tree-sitter"
#endif

/* The LSP client answers from its macro too: which servers are reachable is
 * a run-time question, but whether this build has a client at all is not. */
#ifdef KG_USE_LSP
#define KG_FEATURE_LSP "+lsp"
#else
#define KG_FEATURE_LSP "-lsp"
#endif

int main(int argc, char **argv)
{
	int opt, readonly = 0, no_init = 0;

#ifdef _WIN32
#define KG_GETOPT kg_getopt
#define KG_OPTIND kg_optind
#else
#define KG_GETOPT getopt
#define KG_OPTIND optind
#endif
	while ((opt = KG_GETOPT(argc, argv, "QRVh")) != -1) {
		switch (opt) {
		case 'Q':
			no_init = 1;
			break;
		case 'R':
			readonly = 1;
			break;
		case 'V':
			printf("kg %s %s %s %s\n", KG_VERSION,
			    kg_lisp_active() ? "+lisp" : "-lisp",
			    KG_FEATURE_TREE_SITTER, KG_FEATURE_LSP);
			return 0;
		case 'h':
			return usage(stdout, 0);
		default:
			return usage(stderr, 1);
		}
	}

#ifdef _WIN32
	kg_platform_init();
	for (int i = KG_OPTIND; i < argc; i++) {
		kg_normalize_path(argv[i]);
	}
#endif
	init_editor();
	if (kg_lisp_active() && kg_lisp_init() != 0) {
		fprintf(stderr, "kg: cannot initialize Lisp: %s\n",
		    kg_lisp_last_error());
		return 1;
	}
	kg_lisp_set_interrupt_check(editor_check_quit_pending);
	buf_load_args(argc - KG_OPTIND, argv + KG_OPTIND, readonly);
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
		kg_process_table_poll();
		lsp_poll();
		/* Safe point: each of the non-command lifecycle actions
		 * above (signal-driven resize, compilation polling,
		 * autorevert, process-table and LSP polling) has either done
		 * nothing or fully completed -- none of them holds an edit
		 * transaction or the renderer open across this call.
		 * kg_process_table_poll() itself never writes a buffer; its
		 * drain subscriber runs from kg_event_drain_safe() below, not
		 * from here. */
		kg_event_drain_safe();
		/* Windows first, then the frame: nothing is drawn from a
		 * handle that has stopped resolving.  The top of the loop
		 * is the lifecycle commit -- whatever the last keystroke
		 * opened, killed or switched to has finished happening. */
		win_check_handles();
		KG_STATE_CHECK("main loop");
		/* Skip the redraw while a paste floods stdin, so it costs
		 * a handful of refreshes instead of one per pasted byte. */
		if (!editor_input_flood(STDIN_FILENO)) {
			/* Safe point: about to repaint, so a callback that
			 * changed buffer text will show up in this frame
			 * rather than the one after. */
			kg_event_drain_safe();
			editor_refresh_screen();
		}
		editor_process_keypress(STDIN_FILENO);
		/* Safe point: the keystroke just processed, and the one
		 * command (if any) and edit group it ran, have fully
		 * committed -- editor_process_keypress() never returns
		 * mid-command. */
		kg_event_drain_safe();
	}
	/* Nothing in a default build: KG_PERF_COUNTERS is off, and
	 * kg_lisp_perf_snapshot() is a no-op with it off or Lisp inactive.
	 * A counting build with Lisp active copies the Fe arena's final
	 * state into the KG_PERF_LISP_* gauges just before the counters
	 * that includes are written to $KG_PERF_OUT, which is the only
	 * place that knows the session is over. */
	kg_lisp_perf_snapshot();
	kg_perf_dump();
	return kg_exit_status;
}
