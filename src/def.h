/* kg -- A very simple editor in less than 1-kilo lines of code (as counted
 *       by "cloc"). Does not depend on libcurses, directly emits VT100
 *       escapes on the terminal.
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

#ifndef DEF_H
#define DEF_H

#define KG_VERSION "1.1.0"

/* Compile-time presentation options.
 *
 * KG_SHOW_TILDE: prefix lines past end-of-buffer (and the welcome
 * banner's left margin) with a leading "~", the vim/kilo convention.
 * Off by default for an Emacs-like presentation where such lines are
 * simply blank.  Override at build time, e.g. `make KG_SHOW_TILDE=1`.
 * Defining it via -D on the compile line also works. */
#ifndef KG_SHOW_TILDE
#define KG_SHOW_TILDE 0
#endif

#if defined(__linux__) || defined(__CYGWIN__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Write escape sequences to stdout; silently discard errors (best-effort). */
static inline void tty_write(const void *buf, size_t n)
{
#ifdef KG_FUZZ
	(void)buf;
	(void)n;
#else
	ssize_t r = write(STDOUT_FILENO, buf, n);
	(void)r;
#endif
}

/* Syntax highlight types */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2 /* Single line comment. */
#define HL_MLCOMMENT 3 /* Multi-line comment. */
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8 /* Search match. */
#define HL_WARNING 9 /* Overlong commit subject, etc. */

#define HL_HIGHLIGHT_STRINGS (1 << 0)
#define HL_HIGHLIGHT_NUMBERS (1 << 1)

/* Key action codes */
enum KEY_ACTION {
	KEY_NULL = 0, /* NULL */
	CTRL_A = 1, /* Ctrl-a */
	CTRL_B = 2, /* Ctrl-b */
	CTRL_C = 3, /* Ctrl-c */
	CTRL_D = 4, /* Ctrl-d */
	CTRL_E = 5, /* Ctrl-e */
	CTRL_F = 6, /* Ctrl-f */
	CTRL_G = 7, /* Ctrl-g */
	CTRL_H = 8, /* Ctrl-h */
	TAB = 9, /* Tab */
	CTRL_J = 10, /* Ctrl-j */
	CTRL_K = 11, /* Ctrl-k */
	CTRL_L = 12, /* Ctrl+l */
	ENTER = 13, /* Enter */
	CTRL_N = 14, /* Ctrl-n */
	CTRL_O = 15, /* Ctrl-o */
	CTRL_P = 16, /* Ctrl-p */
	CTRL_Q = 17, /* Ctrl-q */
	CTRL_R = 18, /* Ctrl-r */
	CTRL_S = 19, /* Ctrl-s */
	CTRL_T = 20, /* Ctrl-t */
	CTRL_U = 21, /* Ctrl-u */
	CTRL_V = 22, /* Ctrl-v */
	CTRL_W = 23, /* Ctrl-w */
	CTRL_X = 24, /* Ctrl-x */
	CTRL_Y = 25, /* Ctrl-y */
	CTRL_Z = 26, /* Ctrl-z */
	ESC = 27, /* Escape */
	CTRL_UNDERSCORE = 31, /* Ctrl-_ or Ctrl-/ (undo) */
	BACKSPACE = 127, /* Backspace */
	/* The following are just soft codes, not really reported by the
	 * terminal directly. */
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN,
	CTRL_ARROW_LEFT,
	CTRL_ARROW_RIGHT,
	CTRL_ARROW_UP,
	CTRL_ARROW_DOWN,
	CTRL_HOME,
	CTRL_END,
	SHIFT_ARROW_LEFT,
	SHIFT_ARROW_RIGHT,
	SHIFT_ARROW_UP,
	SHIFT_ARROW_DOWN,
	SHIFT_HOME,
	SHIFT_END,
	INSERT_KEY,
	SHIFT_INSERT, /* CUA paste */
	SHIFT_DELETE, /* CUA cut */
	CTRL_INSERT, /* CUA copy */
	ALT_F,
	ALT_B,
	ALT_D,
	ALT_G,
	ALT_H,
	ALT_V,
	ALT_W,
	ALT_Q,
	ALT_BACKSPACE,
	ALT_PCT, /* M-% query-replace */
	ALT_AT, /* M-@ mark-word */
	ALT_SEMICOLON, /* M-; comment-dwim */
	ALT_COLON, /* M-: eval-expression */
	ALT_X, /* M-x named command */
	ALT_CARET, /* M-^ join-line */
	ALT_U, /* M-u upcase-word */
	ALT_L, /* M-l downcase-word */
	ALT_C, /* M-c capitalize-word */
	ALT_BANG, /* M-! shell-command */
	ALT_PIPE, /* M-| shell-command-on-region */
	ALT_LT, /* M-< beginning-of-buffer */
	ALT_GT, /* M-> end-of-buffer */
	ALT_LBRACE, /* M-{ backward paragraph */
	ALT_RBRACE, /* M-} forward paragraph */
	ALT_M, /* M-m back-to-indentation */
	ALT_A, /* M-a backward sentence */
	ALT_E, /* M-e forward sentence */
	ALT_R, /* M-r move-to-window-line */
	ALT_BACKSLASH, /* M-\ delete-horizontal-space */
	ALT_SPACE, /* M-SPC just-one-space */
	ALT_Z, /* M-z zap-to-char */
	ALT_P, /* M-p minibuffer history: previous */
	ALT_N, /* M-n minibuffer history: next */
	ALT_0, /* M-0 numeric argument */
	ALT_1,
	ALT_2,
	ALT_3,
	ALT_4,
	ALT_5,
	ALT_6,
	ALT_7,
	ALT_8,
	ALT_9,
	ALT_CTRL_S, /* ESC C-s */
	ALT_CTRL_R, /* ESC C-r */
	KEY_F3, /* F3: start keyboard macro */
	KEY_F4 /* F4: stop or replay keyboard macro */
};

/* Syntax highlight definition */
struct erow;
struct editor_syntax {
	char *name; /* Display name shown in mode line, e.g. "C", "Python" */
	char **filematch;
	char **keywords;
	char singleline_comment_start[5];
	char multiline_comment_start[5];
	char multiline_comment_end[5];
	int flags;
	void (*highlight)(
	    struct erow *row); /* NULL => generic keyword scanner */
};

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
	int idx; /* Row index in the file, zero-based. */
	int size; /* Size of the row, excluding the null term. */
	int rsize; /* Size of the rendered row. */
	char *chars; /* Row content. */
	char *render; /* Row content "rendered" for screen (for TABs). */
	unsigned char
	    *hl; /* Syntax highlight type for each character in render.*/
	int hl_oc; /* Row had open comment at end in last syntax highlight
		      check. */
} erow;

#include "localvars.h"

/* Highlight color */
typedef struct hl_color {
	int r, g, b;
} hl_color;

/* Former marks remembered per buffer, popped with C-u C-SPC. */
#define MARK_RING_MAX 16

/* Prefix argument context for commands */
struct command_prefix {
	int supplied;
	int value;
};

/* Editor configuration state */
struct editor_config {
	int cx, cy; /* Cursor x and y position in characters */
	int rowoff; /* Offset of row displayed. */
	int coloff; /* Offset of column displayed. */
	int screenrows; /* Number of rows that we can show */
	int screencols; /* Number of cols that we can show */
	int numrows; /* Number of rows */
	int rawmode; /* Is terminal raw mode enabled? */
	erow *row; /* Rows */
	int dirty; /* File modified but not saved. */
	char *filename; /* Currently open filename */
	char statusmsg[512];
	time_t statusmsg_time;
	int echo_cursor_col; /* 0 = normal; >0 = 1-based column on the bottom
			      * row where the cursor should rest (for minibuffer
			      * prompts). */
	struct editor_syntax *syntax; /* Current syntax highlight, or NULL. */
	int cx_prefix; /* Set to 1 when C-x was pressed, waiting for next key.
			*/
	int cx_prefix_arg; /* Prefix arg carried into the C-x sub-prefix. */
	int cc_prefix; /* Set to 1 when C-c was pressed, waiting for the
			  user-bound key. */
	int prefix_pending; /* Set while accumulating a C-u numeric argument. */
	int prefix_arg; /* The numeric argument under construction. */
	int prefix_supplied; /* 1 if a numeric argument sequence was typed. */
	int prefix_no_digits; /* 1 between C-u and the first digit, so a digit
				 replaces 4. */
	struct command_prefix
	    current_prefix; /* Prefix arg of the active command. */
	int paste_mode; /* If 1, we're in paste mode - disable autocomplete */
	struct timeval
	    last_char_time; /* Time of last character for paste detection */
	int mark_set; /* Is mark set for region selection? */
	int mark_row; /* Mark row position */
	int mark_col; /* Mark column position */
	int mark_highlight; /* 1 when the region should render with reverse
			       video. */
	int mark_ring_row[MARK_RING_MAX]; /* Previous marks, newest first. */
	int mark_ring_col[MARK_RING_MAX];
	int mark_ring_len; /* Number of live entries in the mark ring. */
	int shift_select; /* 1 when the active region was started by
			     shift+motion. */
	int rect_mode; /* 1 when the region should render as a rectangle. */
	int rect_prefix; /* 1 after C-x r, waiting for the rectangle op key. */
	int desired_visual_col; /* goal column across vertical motion; -1 =
				   unset. */
	int readonly; /* If 1, buffer is read-only (editing is blocked). */
	int readonly_local; /* 0/1, set by local variables */
	int readonly_override; /* -1 none, 0 explicit writable, 1 explicit
				  read-only */
	char compile_command[KG_COMPILE_COMMAND_MAX];
	int compile_command_user_override; /* 1 once the user edited
					      compile-command */
	int last_key; /* Last key processed, for command repetition logic. */
	int recenter_state; /* Cycle state for C-l: 0=center, 1=top, 2=bottom.
			     */
	int window_line_state; /* Cycle state for M-r: 0=top, 1=middle,
				  2=bottom. */
	time_t disk_mtime; /* mtime of `filename` when we last read/wrote it. */
	off_t disk_size; /* size of `filename` when we last read/wrote it. */
	int disk_changed; /* Set by the auto-revert poll when disk differs. */
	int auto_revert; /* Per-buffer auto-revert toggle. */
	int visual_line_mode; /* 1 if visual-line-mode is enabled */
	int rowoff_visual; /* Visual row offset for visual-line-mode */
	int overwrite_mode; /* 1 if overwrite-mode is enabled */
};

/* Append buffer for efficient screen rendering */
struct abuf {
	char *b;
	int len;
	int oom;
};
#define ABUF_INIT { NULL, 0, 0 }

/* Kill ring (yank buffer) for copy/paste operations */
struct kill_ring {
	char *text; /* Killed/copied text */
	int len; /* Length of text */
};

/* Undo operation types */
enum undo_type {
	UNDO_INSERT_CHAR,
	UNDO_DELETE_CHAR,
	UNDO_INSERT_LINE,
	UNDO_DELETE_LINE,
	UNDO_SPLIT_LINE,
	UNDO_JOIN_LINE,
	UNDO_KILL_TEXT, /* Kill line or region */
	UNDO_YANK_TEXT, /* Yank (paste) */
	UNDO_REPLACE_TEXT, /* Replace span at row/col; c = replacement length */
	UNDO_REFLOW_PARA, /* M-q paragraph reflow */
	UNDO_RECT_OVERWRITE /* Rectangle kill/delete/clear/yank: restore rows */
};

/* Single undo operation */
struct undo_op {
	enum undo_type type;
	int row; /* Row where operation occurred */
	int col; /* Column where operation occurred */
	int c; /* Character (for char operations) */
	char *text; /* Line text (for line operations) */
	int len; /* Length of text */
	struct undo_op *next;
};

#define MAX_UNDO_SIZE 1000

/* Undo stack */
struct undo_stack {
	struct undo_op *head;
	int size;
	int max_size;
	int clean_size; /* Stack size at last save (-1 if never saved clean) */
};

/* Per-window viewport state. */
#define MAX_WINDOWS 8
struct editor_window {
	int bufidx; /* Which buffer this window shows */
	int cx, cy; /* Cursor position within window */
	int rowoff, coloff; /* Scroll offsets */
	int y, x; /* Top-left corner on terminal (1-based) */
	int h, w; /* Height (text rows) and width (cols) of this window */
	int active; /* 1 if this slot is in use */
	int col_group; /* Column group: windows with same value stack
			  vertically; different values sit side-by-side */
	int rowoff_visual; /* Visual row offset for visual-line-mode */
};

/* Per-buffer state saved when switching away from a buffer. */
#define MAX_BUFFERS 20
struct editor_buffer {
	int cx, cy;
	int rowoff, coloff;
	int numrows;
	erow *row;
	int dirty;
	char *filename;
	struct editor_syntax *syntax;
	int mark_set;
	int mark_row, mark_col;
	int mark_highlight;
	int mark_ring_row[MARK_RING_MAX];
	int mark_ring_col[MARK_RING_MAX];
	int mark_ring_len;
	int shift_select;
	int rect_mode;
	struct undo_stack undostack; /* per-buffer undo chain */
	int active; /* 1 if this slot is in use */
	int readonly; /* 1 if buffer is read-only */
	int readonly_local; /* 0/1, set by local variables */
	int readonly_override; /* -1 none, 0 explicit writable, 1 explicit
				  read-only */
	char compile_command[KG_COMPILE_COMMAND_MAX];
	int compile_command_user_override; /* 1 once the user edited
					      compile-command */
	time_t disk_mtime;
	off_t disk_size;
	int disk_changed;
	int auto_revert;
	int visual_line_mode;
	int rowoff_visual;
	int overwrite_mode;
};

/* Global editor state */
extern struct editor_config editor;
extern int running;
extern int kg_exit_status; /* Process exit status returned by main(). */
extern int suppress_undo;
extern struct kill_ring killring;
extern struct undo_stack undostack;
extern struct editor_buffer buflist[MAX_BUFFERS];
extern int buf_current; /* index into buflist[] of the active buffer */
extern int buf_count; /* number of active buffers */
extern int global_auto_revert; /* Default auto-revert flag for all buffers. */
extern int electric_pairs; /* 1 when typing an opener inserts its closer. */

extern struct editor_window winlist[MAX_WINDOWS];
extern int win_current; /* index into winlist[] of the active window */
extern int win_count; /* number of active windows */
extern int win_total_rows; /* terminal rows (set by update_window_size) */
extern int win_total_cols; /* terminal cols (set by update_window_size) */

/* bufmgr.c */
extern struct editor_syntax lisp_interaction_syntax;
extern struct editor_syntax compilation_syntax;
extern struct editor_syntax text_syntax;
int buf_replace_special_text(const char *name, struct editor_syntax *syntax,
    const char *text, size_t text_length, int readonly);
int buf_prepare_special_text(
    const char *name, struct editor_syntax *syntax, int readonly);
int buf_append_special_text(
    int buffer_index, const char *text, size_t text_length);
void buf_clear_special_text(int buffer_index);
void buf_truncate_last_row(int buffer_index, size_t len_to_remove);
void buf_save_current_state(void);

/* Result of a minibuffer prompt.  ACCEPTED is the only outcome that should
 * ever be acted on by callers that execute the typed text (e.g. as a shell
 * command): OVERFLOW means the accepted buffer is a silently truncated
 * prefix of what the user typed, which is unsafe to run verbatim. */
enum minibuf_result {
	MINIBUF_CANCELLED = -1,
	MINIBUF_ACCEPTED = 0,
	MINIBUF_OVERFLOW = 1,
};

enum minibuf_result editor_read_line(
    int fd, const char *prompt, char *buf, int bufsize);
int editor_read_line_path(int fd, const char *prompt, char *buf, int bufsize);

#define MINIBUF_HISTORY_MAX 32
#define MINIBUF_HISTORY_ENTRY_MAX 256

/* A circular ring of past minibuffer entries (e.g. shell commands),
 * navigable with M-p / M-n.  `head` is the physical slot of the newest
 * entry; meaningless when count==0.  Zero-initialization (static/global
 * storage) is equivalent to minibuf_history_init(): the first insertion
 * always lands at slot 0 regardless of head's initial value. */
struct minibuf_history {
	char entries[MINIBUF_HISTORY_MAX][MINIBUF_HISTORY_ENTRY_MAX];
	int head;
	int count;
};

void minibuf_history_init(struct minibuf_history *hist);
void minibuf_history_add(struct minibuf_history *hist, const char *text);
const char *minibuf_history_get(const struct minibuf_history *hist, int index);
const char *minibuf_history_walk(
    const struct minibuf_history *hist, int dir, int *index, const char *draft);
enum minibuf_result editor_read_line_with_history(int fd, const char *prompt,
    char *buf, int bufsize, struct minibuf_history *hist);
void editor_prompt_prefill_dir(char *buf, int bufsize);
void editor_path_expand_tilde(char *buf, int bufsize);
#define PICKER_MAX_ENTRIES 64
void editor_picker_render(char *msg, int msg_size, int *off,
    const char *const *names, int n, int n_total, int sel);
void editor_msg_appendf(char *msg, int size, int *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
int autorevert_poll(void);
void buf_reload_from_disk(void);
void buf_restore_from_slot(int idx);
void buf_visit_file(const char *filename, int explicit_readonly);
void minibuf_delete_backward(char *buf, int *cursor, int *len, int *overflow);

/* path.c */
#define PATH_ENTRY_NAME_MAX 256 /* fits POSIX NAME_MAX (255) + NUL */
struct path_entry {
	char name[PATH_ENTRY_NAME_MAX];
	int is_dir;
};
void editor_path_split(
    const char *path, char *dir, int dsize, char *file, int fsize);
int editor_path_complete_entries(const char *dir, const char *prefix,
    struct path_entry *entries, int max, char *lcp, int lcp_size);
int editor_picker_match_rank(const char *haystack, const char *needle);
void buf_load_args(int nfiles, char **filenames, int readonly);
void buf_select_interactive(int fd);
void buf_open_file(int fd);
void buf_open_file_read_only(int fd);
void buf_kill(int fd);
void buf_save_all(int fd);
void buf_open_list(void);
void buf_open_help(void);
void buf_ibuffer_select(void);
void buf_display_name(int idx, char *out, size_t outsize);
void editor_cleanup(void);

/* winmgr.c */
void win_init(void);
void win_reflow(void);
void win_save_active_view(void);
void win_restore_active_view(void);
void win_split_horizontal(void);
void win_split_vertical(void);
void win_cycle_next(void);
void win_delete_current(void);
void win_delete_others(void);
void win_display_buffer_other_window(int buffer_index);
int win_can_display_buffer_other_window(int buffer_index);
void win_position_at_end(int buffer_index);

/* autocomplete.c */
int editor_find_close_char(int open_char);
void editor_insert_char_auto_complete(int c);

/* basic.c */
void editor_move_cursor(int key);
void editor_move_to_beginning(void);
void editor_move_to_end(void);
void editor_move_to_indentation(void);
void editor_move_to_window_line(void);
void editor_goto_line_direct(int line, int col);
void editor_goto_line(int fd);
void editor_cursor_goto(int row, int col);
void editor_reveal_position_centered(int row, int col);
int editor_current_filerow_or_eof(void);
int editor_current_filerow(void);
int editor_current_filecol(void);
int editor_current_filecol_in_row(erow *row);
void editor_snap_cx_to_row(void);
int editor_visual_col(erow *row, int chars_col);
int editor_display_col(erow *rows, int numrows, int filerow, int filecol);
int editor_chars_col_at_visual(erow *row, int target_vcol);

/* buffer.c */
void editor_update_row(erow *row);
void editor_insert_row(int at, const char *s, size_t len);
void editor_free_row(erow *row);
void editor_free_all_rows(void);
void editor_del_row(int at);
char *editor_rows_to_string(erow *rows, int numrows, int *buflen);
void editor_row_insert_char(erow *row, int at, int c);
void editor_row_insert_string(erow *row, int at, const char *s, int len);
void editor_row_insert_spaces(erow *row, int at, int len);
void editor_row_append_string(erow *row, char *s, size_t len);
void editor_row_del_char(erow *row, int at);
void editor_insert_char(int c);
void editor_insert_newline_raw(void);
void editor_insert_text_raw(const char *text, int len);
void editor_insert_newline(void);
void editor_open_line(void);
void editor_del_char(void);
void editor_del_forward_char(void);
void editor_transpose_chars(void);
void editor_kill_line(void);
void editor_toggle_read_only_mode(void);
void editor_toggle_overwrite_mode(void);
void editor_overwrite_char(int c);
void editor_self_insert_char(int c);
void editor_self_insert_glyph(const char *seq, int len);
void editor_refresh_readonly_state(void);
void editor_set_local_readonly(int enabled);
void editor_set_readonly_override(int enabled);
int editor_row_byte_to_char(erow *row, int byte_index);
int editor_row_char_to_byte(erow *row, int char_index);
long editor_char_offset(int row, int col);
void editor_offset_to_rowcol(long offset, int *row, int *col);
long editor_buffer_char_length(void);

static inline int checked_add_size_t(size_t *result, size_t a, size_t b)
{
	if (a > SIZE_MAX - b) {
		return 0;
	}
	*result = a + b;
	return 1;
}

static inline int checked_mul_size_t(size_t *result, size_t a, size_t b)
{
	if (a != 0 && b > SIZE_MAX / a) {
		return 0;
	}
	*result = a * b;
	return 1;
}

static inline int checked_add_int_size(int *result, int a, size_t b)
{
	if (a < 0) {
		return 0;
	}
	if (b > (size_t)INT_MAX) {
		return 0;
	}
	if ((size_t)a > (size_t)INT_MAX - b) {
		return 0;
	}
	*result = a + (int)b;
	return 1;
}

static inline int checked_size_to_int(int *result, size_t val)
{
	if (val > (size_t)INT_MAX) {
		return 0;
	}
	*result = (int)val;
	return 1;
}

/* Returns 1 if filename belongs to a special/system buffer (NULL or starts with
 * '*'). */
static inline int is_special_buffer(const char *filename)
{
	return !filename || filename[0] == '*';
}

/* True for UTF-8 continuation bytes (0x80–0xBF).  Useful when iterating
 * a raw byte stream and needing to skip past or land on glyph boundaries. */
static inline int utf8_is_cont(unsigned char b) { return (b & 0xC0) == 0x80; }

/* Tab stops sit every KG_TAB_WIDTH display columns, matching Emacs'
 * default tab-width, cat(1) and the terminal itself. */
#define KG_TAB_WIDTH 8

/* Columns a TAB occupies when it starts at visual column `vcol`: enough
 * to reach the next tab stop, i.e. 1..KG_TAB_WIDTH columns.  Every
 * module that models tab geometry must go through this. */
static inline int tab_stop_advance(int vcol)
{
	return KG_TAB_WIDTH - (vcol % KG_TAB_WIDTH);
}

/* Continuation bytes that must follow the UTF-8 lead byte `lead`, or 0
 * when `lead` is ASCII, a continuation byte, or one of the values no
 * well-formed sequence starts with (0xC0/0xC1 and 0xF5-0xFF). */
static inline int utf8_lead_extra(unsigned char lead)
{
	if (lead >= 0xC2 && lead <= 0xDF) {
		return 1;
	}
	if (lead >= 0xE0 && lead <= 0xEF) {
		return 2;
	}
	if (lead >= 0xF0 && lead <= 0xF4) {
		return 3;
	}
	return 0;
}

/* Byte length of the glyph starting at buf[start], validating the lead byte
 * and every required continuation byte against buf[0..len).  Returns 1 for
 * ASCII, malformed lead bytes, or a start position on a continuation byte,
 * so callers never treat a corrupt sequence as one oversized glyph. */
static inline int utf8_glyph_span_at(const char *buf, int len, int start)
{
	unsigned char lead;
	int need, i;

	if (start < 0 || start >= len) {
		return 1;
	}
	lead = (unsigned char)buf[start];
	if (lead < 0x80 || utf8_is_cont(lead)) {
		return 1;
	}
	need = utf8_lead_extra(lead);
	if (need == 0) {
		return 1;
	}
	for (i = 1; i <= need; i++) {
		if (start + i >= len
		    || !utf8_is_cont((unsigned char)buf[start + i])) {
			return 1;
		}
	}
	return need + 1;
}

/* Byte offset where the glyph ending at `pos` begins, i.e. how far back a
 * backspace at `pos` has to reach to remove one whole character.  Falls
 * back to `pos - 1` when the bytes before `pos` are not a well-formed
 * sequence, so a corrupt byte can always be deleted. */
static inline int utf8_glyph_start_before(const char *buf, int len, int pos)
{
	int start;

	if (pos <= 0) {
		return 0;
	}
	start = pos - 1;
	while (start > 0 && utf8_is_cont((unsigned char)buf[start])) {
		start--;
	}
	if (utf8_glyph_span_at(buf, len, start) != pos - start) {
		return pos - 1;
	}
	return start;
}

/* Return the basename of a filename (part after last '/'), or the whole
 * string if no '/' is present.  Falls back to "[new]" for NULL. */
static inline const char *buf_basename(const char *filename)
{
	const char *base;
	if (!filename) {
		return "[new]";
	}
	base = strrchr(filename, '/');
	return base ? base + 1 : filename;
}

/* help.c */
extern const char *kg_help_lines[]; /* NULL-terminated, loaded into *help* */

/* display.c */
int ab_append(struct abuf *ab, const char *s, int len);
void ab_free(struct abuf *ab);
void editor_refresh_screen(void);
void editor_set_status_message(const char *fmt, ...);

/* fileio.c */
struct temp_load_result {
	char *filename;
	erow *row;
	int numrows;
	time_t disk_mtime;
	off_t disk_size;
};

int load_file_transactional(const char *filename, struct temp_load_result *res);
void commit_load_result(struct temp_load_result *res);
void free_load_result(struct temp_load_result *res);

int editor_open(char *filename);
int editor_save(int fd);
ssize_t write_all(int fd, const char *buf, size_t len);
extern ssize_t (*editor_write_fn)(int fd, const void *buf, size_t count);
extern int (*editor_fsync_fn)(int fd);
extern int (*editor_close_fn)(int fd);
extern int (*editor_rename_fn)(const char *oldpath, const char *newpath);
int editor_write_rows_to_file(
    const char *filename, erow *rows, int numrows, int *out_len);
void editor_write_file(int fd);
void editor_insert_file(int fd);
void editor_snapshot_disk(void);
int file_state_differs(const char *path, time_t mtime, off_t size);

/* kbd.c */
void editor_process_keypress(int fd);

/* cmd.c */
void editor_named_command(int fd);
[[nodiscard]] int cmd_execute_named(const char *name, int fd);
[[nodiscard]] int cmd_execute_named_with_prefix(
    const char *name, int fd, struct command_prefix prefix);
[[nodiscard]] int cmd_static_exists(const char *name);
void cmd_eval_print_last_sexp(void);

/* keybind.c */
[[nodiscard]] int keybind_parse(const char *sequence, int *key);
[[nodiscard]] int keybind_bind(const char *sequence, const char *command);
[[nodiscard]] int keybind_unbind(const char *sequence);
[[nodiscard]] const char *keybind_lookup(int key);

/* macro.c */
int macro_is_recording(void);
void macro_on_key(int key);
int macro_next_key(void);
void macro_reset(void);
void macro_start(void);
void macro_stop(int trim);
void macro_replay(int fd, int count);

/* search.c */
enum search_kind { SEARCH_LITERAL, SEARCH_REGEXP };
void editor_find(int fd, int direction);
void editor_find_regexp(int fd, int direction);
void editor_query_replace(int fd);
void editor_query_replace_regexp(int fd);

/* shell.c */
void editor_shell_command(int fd, int insert_output);
void editor_shell_command_on_region(int fd, int insert_output);

struct shell_run_status {
	bool known; /* false if the child could not be reaped (waitpid
		       failure): exited/exit_code/signal_number are then
		       meaningless and must not be reported as success. */
	bool exited;
	int exit_code;
	int signal_number;
};

/* status may be NULL when the caller doesn't need exit-status detail. */
char *shell_run(const char *cmd, const char *in, int inlen, int *out_len,
    struct shell_run_status *status);

/* Overridable by tests; defaults to waitpid(). */
extern pid_t (*shell_waitpid_fn)(pid_t pid, int *status, int options);

struct shell_capture_result {
	char *output;
	size_t output_length;
	bool exited;
	int exit_code;
	int signal_number;
	bool truncated;
};

int shell_run_capture(const char *command, const char *directory,
    size_t maximum_output, struct shell_capture_result *result);
bool shell_output_fits_echo(
    const char *out, int out_len, int available_columns, int reserved_columns);

/* syntax.c */
int is_separator(int c);
int editor_row_has_open_comment(erow *row);
void editor_update_syntax(erow *row);
void editor_rehighlight_from(int start_idx);
struct editor_syntax *syntax_find_by_name(const char *name);
void editor_set_syntax(struct editor_syntax *syntax);
void editor_rehighlight_all(void);
int editor_syntax_to_color(int hl);
void editor_select_syntax_highlight(char *filename);
[[nodiscard]] int syntax_is_git_commit(void);
[[nodiscard]] int syntax_git_commit_subject(void);
[[nodiscard]] int syntax_is_git_rebase(void);
int syntax_git_rebase_pick_span(
    const char *line, int len, int *start, int *wlen);
int syntax_git_rebase_flags_end(const char *line, int len, int from);

/* tty.c */
void disable_raw_mode(int fd);
void editor_at_exit(void);
int enable_raw_mode(int fd);
void editor_suspend(void);
int editor_read_key(int fd);
int editor_read_key_idle(int fd);
int editor_input_flood(int fd);
int editor_read_raw_byte(int fd);
int editor_read_utf8_seq(int fd, int lead, char *seq);
int editor_check_quit_pending(void);
int get_cursor_position(int ifd, int ofd, int *rows, int *cols);
int get_window_size(int ifd, int ofd, int *rows, int *cols);
void update_window_size(void);
void probe_window_size(void);
void handle_sig_winch(int unused);
int editor_process_pending_signals(void);

/* width.c */
int kg_codepoint_width(uint32_t cp);
uint32_t utf8_codepoint_at(const char *buf, int len, int start, int *span);
int utf8_width_at(const char *buf, int len, int start);
int utf8_display_width(const char *buf, int len);

/* word.c */
void editor_move_word_forward(void);
void editor_move_word_backward(void);
void editor_move_paragraph_forward(void);
void editor_move_paragraph_backward(void);
void editor_mark_paragraph(void);
void editor_mark_word(int count);
void editor_move_sentence_forward(void);
void editor_move_sentence_backward(void);
void editor_kill_word_forward(void);
void editor_kill_word_backward(void);
void editor_join_line(void);
void editor_delete_horizontal_space(void);
void editor_just_one_space(void);
void editor_zap_to_char(int fd, int count);
void editor_rebase_set_action(const char *action);
void editor_rebase_move_line(int dir);
void editor_upcase_word(void);
void editor_downcase_word(void);
void editor_capitalize_word(void);
void editor_reflow_paragraph(void);
void editor_comment_dwim(void);

/* yank.c */
void kill_ring_init(void);
void kill_ring_free(void);
void kill_ring_set(char *text, int len);
void kill_ring_append(char *text, int len);
char *kill_ring_get(void);
void editor_set_mark(void);
void editor_set_mark_silent(void);
void editor_push_mark(void);
void editor_pop_to_mark(void);
void editor_exchange_point_and_mark(void);

/* Generic region delete-without-save (Delete key).  Falls through to
 * editor_del_forward_char when no region is active. */
void editor_delete_region_or_char(void);
int editor_delete_text_range_raw(int start_row, int start_col, int byte_len);

/* Rectangle operations (src/rect.c) */
void editor_rect_mode_toggle(void);
void editor_kill_rect(void);
void editor_delete_rect(void);
void editor_clear_rect(void);
void editor_yank_rect(void);
void editor_string_rect(int fd);
void rect_kill_ring_free(void);
void editor_kill_region(void);
void editor_copy_region(void);
char *editor_get_region_text(int *out_len);
int editor_region_bounds(
    int *start_row, int *start_col, int *end_row, int *end_col);
void editor_yank(void);
void editor_sort_lines(void);

/* undo.c */
void undo_init(void);
void undo_free(void);
int undo_push(
    enum undo_type type, int row, int col, int c, char *text, int len);
void editor_undo(void);
void undo_mark_clean(void);

/* mode.c */
int get_visual_row(erow *rows, int numrows, int win_w, int cy, int cx);
int visual_line_cursor_col(erow *row, int chars_col, int win_w);
void find_visual_row(erow *rows, int numrows, int win_w, int rowoff_visual,
    int target_y, int *logical_row, int *char_offset);
int render_col_to_chars(erow *row, int target_rcol, int win_w);
int visual_line_width(erow *row, int win_w);
void goto_visual_row_col(int target_vrow, int target_rcol_in_segment);
int chars_to_render_col(erow *row, int chars_col);
int get_total_visual_rows(erow *rows, int numrows, int win_w);

/* main.c */
void init_editor(void);

#endif /* DEF_H */
