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

#include "perf.h"

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
	ALT_ENTER, /* M-RET path prompt: accept the text literally */
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
struct editor_buffer;
struct editor_syntax {
	char *name; /* Display name shown in mode line, e.g. "C", "Python" */
	char **filematch;
	char **keywords;
	char singleline_comment_start[5];
	char multiline_comment_start[5];
	char multiline_comment_end[5];
	int flags;
	/* NULL => generic keyword scanner.  The buffer is passed because a
	 * multi-line construct's state comes from the row above, which is
	 * only reachable through the buffer that owns it. */
	void (*highlight)(struct editor_buffer *b, struct erow *row);
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
	int render_capacity; /* Bytes `render` holds; >= rsize + 1. */
	int hl_capacity; /* Bytes `hl` holds; >= rsize. */
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

/* Every interactive command is invoked through one descriptor, so the
 * question "may this run here, and who may ask for it?" has exactly one
 * answer.  Before this, the read-only verdict was spelled three times
 * (cmd.c's flags, lisp.c's allow-list, kbd.c's per-keycode blocklist);
 * the first two are now this table, and kbd.c's list is what plan 11
 * phase 3 retires when built-in keys resolve to command names. */
typedef void (*cmdfn)(int fd);

enum command_flags {
	CMD_NONE = 0,
	/* Refused outright in a read-only buffer. */
	CMD_EDITS_BUFFER = 1 << 0,
	/* May be reached from Lisp's (command-execute ...). */
	CMD_LISP_CALLABLE = 1 << 1,
};

struct named_cmd {
	const char *name;
	cmdfn fn; /* NULL for a command that lives in Lisp */
	unsigned flags;
	const char *summary; /* one line, <= 60 columns, no trailing period */
};

/* Who asked for the command.  Only the Lisp origin is policed differently:
 * it is the one caller that must clear CMD_LISP_CALLABLE, and the one that
 * reports a refusal as a Lisp error rather than an echo-area message. */
enum command_origin {
	CMD_ORIGIN_KEY,
	CMD_ORIGIN_MX,
	CMD_ORIGIN_LISP,
};

struct command_context {
	int fd;
	struct command_prefix prefix;
	enum command_origin origin;
};

/* cmd_invoke() verdicts. */
enum command_result {
	CMD_RAN = 0,
	CMD_UNKNOWN = 1, /* no command of that name */
	CMD_NOT_CALLABLE = 2, /* exists, but not from this origin */
	CMD_READ_ONLY = 3, /* exists, but the buffer refuses edits */
};

/* The on-disk identity of a file, as one stat() saw it.  The fields are
 * widened to fixed-width types and ordered so the struct carries no padding
 * on any supported target, which is what lets two samples be compared as
 * bytes instead of field by field. */
struct file_identity {
	uint64_t present; /* 1 when the path named an object at all. */
	uint64_t device;
	uint64_t inode;
	uint64_t size;
	int64_t mtime_sec;
	int64_t ctime_sec;
	uint32_t mtime_nsec;
	uint32_t ctime_nsec;
};

/* What a buffer accepted the last time it read or wrote its file.  `valid`
 * is false when the state could not be sampled at all, which is a different
 * thing from "the file is not there" — stat() reports absence perfectly
 * well. */
struct file_snapshot {
	struct file_identity id;
	bool valid;
};

/* Three answers, because "unable to check" must never be spelled the same
 * way as "unchanged". */
enum file_change_state {
	FILE_SAME,
	FILE_DIFFERENT,
	FILE_UNKNOWN,
};

/* Editor configuration state */
struct editor_config {
	int rawmode; /* Is terminal raw mode enabled? */
	char statusmsg[512];
	time_t statusmsg_time;
	/* Half-open byte range of statusmsg[] the echo area draws
	 * emphasised; empty when equal.  Styling is addressed by position
	 * rather than spelled into the text, so nothing a caller
	 * interpolates into a message can ask the terminal for it. */
	int statusmsg_emph_start;
	int statusmsg_emph_end;
	int echo_cursor_col; /* 0 = normal; >0 = 1-based column on the bottom
			      * row where the cursor should rest (for minibuffer
			      * prompts). */
	int cx_prefix; /* Set to 1 when C-x was pressed, waiting for next key.
			*/
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
	int rect_prefix; /* 1 after C-x r, waiting for the rectangle op key. */
	int last_key; /* Last key processed, for command repetition logic. */
	int recenter_state; /* Cycle state for C-l: 0=center, 1=top, 2=bottom.
			     */
	int window_line_state; /* Cycle state for M-r: 0=top, 1=middle,
				  2=bottom. */
};

/* Append buffer for efficient screen rendering */
struct abuf {
	char *b;
	int len;
	int capacity; /* Bytes `b` holds; see ab_append(). */
	int oom;
};
#define ABUF_INIT { NULL, 0, 0, 0 }

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
	UNDO_RECT_OVERWRITE, /* Rectangle kill/delete/clear/yank: restore rows
			      */
	/* One replacement of a byte range, addressed by flat position:
	 * what an edit through kg_buffer_replace() records, and what the
	 * eleven opcodes above are being migrated to.  It needs no variant
	 * per kind of edit because every edit is one of these. */
	UNDO_CHANGE
};

/* Single undo operation */
struct undo_op {
	enum undo_type type;
	size_t position; /* UNDO_CHANGE: flat byte position of the change */
	int row; /* Row where operation occurred */
	int col; /* Column where operation occurred */
	int c; /* Character (for char operations); UNDO_CHANGE: new length */
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

/* Where a view sat in a buffer.  A window that shows a buffer it has not
 * shown before starts here rather than at the top, which is what makes a
 * buffer switch feel like returning to where you were. */
struct kg_point {
	int cx, cy;
	int rowoff, coloff;
	int rowoff_visual;
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
	int desired_visual_col; /* goal column across vertical motion; -1 =
				   unset. */
};

/* Per-buffer state saved when switching away from a buffer. */
#define MAX_BUFFERS 20
struct editor_buffer {
	/* Identity of whatever buffer currently occupies this slot.  `id` is
	 * drawn from a counter that never repeats, so it names one buffer for
	 * the life of the process; `generation` counts the times this slot has
	 * changed hands.  Both are needed to tell "the buffer I meant" from
	 * "another buffer that happens to sit where it did".  A slot that has
	 * never been used has id 0. */
	uint32_t id;
	uint32_t generation;
	struct kg_point last_point; /* Where a view last left off here. */
	int numrows;
	erow *row;
	int row_capacity;
	int dirty;
	/* Bumped once per content change and never reset, so a command can
	 * sample it before and after and know whether it edited anything.
	 * See buffer_note_change(). */
	uint64_t content_generation;
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
	struct file_snapshot disk;
	int disk_changed;
	int auto_revert;
	int visual_line_mode;
	int overwrite_mode;
};

/* A reference to a buffer that outlives the command that took it.
 *
 * A bare slot index does not: buf_kill() frees the slot and the next open
 * hands it to an unrelated buffer, so an index kept across commands can
 * quietly start naming somebody else's text.  The identity pair survives
 * that, because buf_resolve() refuses to answer unless the slot still holds
 * the same (id, generation) the handle was taken from.  A zeroed handle
 * names nothing. */
struct kg_buffer_handle {
	int slot;
	uint32_t id;
	uint32_t generation;
};

/* Global editor state */
extern struct editor_config editor;
extern int running;
extern int kg_exit_status; /* Process exit status returned by main(). */
extern int suppress_undo;
extern struct kill_ring killring;
extern struct editor_buffer buflist[MAX_BUFFERS];
extern int buf_current; /* index into buflist[] of the active buffer */
extern int buf_count; /* number of active buffers */
extern int global_auto_revert; /* Default auto-revert flag for all buffers. */

/* The buffer the session is currently in.  Buffer-scoped state belongs to a
 * slot in buflist[]; this is how code that only ever means "the one the user
 * is editing" reaches it without carrying an index around.  Never NULL:
 * buf_current always names a slot, and a slot nobody holds reads as an empty
 * buffer.  Code that may mean a *different* buffer must take a
 * struct editor_buffer * instead — that is the whole point of the split. */
static inline struct editor_buffer *bcur(void) { return &buflist[buf_current]; }
extern int electric_pairs; /* 1 when typing an opener inserts its closer. */

extern struct editor_window winlist[MAX_WINDOWS];
extern int win_current; /* index into winlist[] of the active window */
extern int win_count; /* number of active windows */
extern int win_total_rows; /* terminal rows (set by update_window_size) */
extern int win_total_cols; /* terminal cols (set by update_window_size) */

/* The window the user is typing in.  Point, scroll and goal column belong
 * to a window, not to the session and not to the buffer: two windows on one
 * buffer scroll independently.  Never NULL, for the same reason bcur() is
 * not -- win_current always names a slot.  Code that may mean a different
 * window takes a struct editor_window * instead. */
static inline struct editor_window *wcur(void) { return &winlist[win_current]; }

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
void buf_open_special(const char *name, struct editor_syntax *syn,
    void (*populate)(void), const char *status);

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
int editor_picker_render(char *msg, int msg_size, int *off,
    const char *const *names, int n, int n_total, int sel);
void editor_picker_emphasise(
    int sel_off, const char *const *names, int n, int sel);
void editor_msg_appendf(char *msg, int size, int *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
int autorevert_poll(void);
void buf_reload_from_disk(void);
void buf_select(int slot);
void buf_remember_view(const struct editor_window *w);
void buf_attach_view(struct editor_window *w, int slot);
struct kg_buffer_handle buf_handle(int slot);
struct editor_buffer *buf_resolve(struct kg_buffer_handle handle);
int buf_handle_slot(struct kg_buffer_handle handle);
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
/* Walks a picker's candidates: `index` counts from 0, `data` is whatever
 * the caller handed editor_picker_filter(), and NULL ends the walk. */
typedef const char *(*picker_name_fn)(int index, void *data);
[[nodiscard]] int editor_picker_filter(picker_name_fn name_at, void *data,
    const char *query, const char **names, int *order, int max,
    int *prefix_matches);
void buf_load_args(int nfiles, char **filenames, int readonly);
void buf_select_interactive(int fd);
void buf_open_file(int fd);
void buf_open_file_read_only(int fd);
void buf_open_path(const char *path, int readonly);
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
void buffer_note_change(struct editor_buffer *b);
void editor_update_row(struct editor_buffer *b, erow *row);
int editor_rows_reserve(erow **rows, int *capacity, int need);
int editor_row_grown_capacity(int need);
void editor_insert_row(
    struct editor_buffer *b, int at, const char *s, size_t len);
void editor_free_row(erow *row);
void editor_free_all_rows(struct editor_buffer *b);
void editor_del_row(struct editor_buffer *b, int at);
char *editor_rows_to_string(erow *rows, int numrows, int *buflen);
void editor_row_insert_char(erow *row, int at, int c);
void editor_row_insert_string(erow *row, int at, const char *s, int len);
void editor_row_insert_spaces(erow *row, int at, int len);
void editor_row_append_string(erow *row, char *s, size_t len);
void editor_row_del_char(erow *row, int at);
/* What an edit through editor_row_replace_range() should also do.  Zero
 * is the ordinary edit -- record undo, count the buffer dirty -- and is
 * what every command that is one user operation passes.  This is the
 * local precursor to the edit transaction plan 10 describes; it is a
 * flag word rather than a struct so that a later option needs no churn
 * at the call sites that do not want it. */
enum edit_option {
	/* The caller records its own undo step covering the whole
	 * operation, so this one must not push a second, finer record. */
	KG_EDIT_NO_UNDO = 1 << 0,
	/* This edit is undo putting a record back.  It records nothing --
	 * for the same reason KG_EDIT_NO_UNDO does not -- and it is a
	 * distinct option because the two must not be confused when the
	 * one flag they both replace, `suppress_undo`, is retired: a
	 * caller writing its own coarse record and undo replaying one are
	 * different situations that happen to want the same silence. */
	KG_EDIT_REPLAY = 1 << 1,
};

/* One replacement of a byte range by other bytes: the whole of what a
 * command may do to a buffer's text.  `begin_byte` and `end_byte` are
 * flat byte positions (see buffer_byte_length()); `end_byte` equal to
 * `begin_byte` is an insertion, an empty replacement is a deletion, and
 * `replacement` must be non-NULL even when its length is zero. */
struct kg_edit {
	struct editor_buffer *buffer;
	size_t begin_byte;
	size_t end_byte;
	const char *replacement;
	size_t replacement_len;
	unsigned options; /* bitwise OR of enum edit_option */
};

/* What the edit did, for a caller that has to describe it afterwards.
 * The generations bracket the commit: they differ by exactly one when
 * the edit changed bytes, and are equal when it was refused. */
struct kg_edit_result {
	size_t old_length;
	size_t new_length;
	uint64_t before_generation;
	uint64_t after_generation;
};

int kg_buffer_replace(const struct kg_edit *e, struct kg_edit_result *out);

int editor_row_replace_range(int filerow, int at, int delete_len,
    const char *insert, int insert_len, unsigned options);
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

/* A position in a buffer is a byte offset into its text read as one
 * string: every row's bytes, one '\n' between neighbours, none after the
 * last.  This is the only position dialect the editor uses.
 *
 * The three `editor_*_char_*` conversions below are a second dialect --
 * 0-based codepoint offsets, in `long` -- and belong to `src/lisp.c`,
 * which is where Emacs-shaped point arithmetic is spoken.  Nothing else
 * may grow a use for them: two dialects that interleave is how a
 * coordinate bug gets written, and one of them is already the adapter at
 * the edge. */
size_t buffer_byte_length(const struct editor_buffer *b);
size_t buffer_row_col_to_position(
    const struct editor_buffer *b, int row, int col);
int buffer_position_to_row_col(
    const struct editor_buffer *b, size_t pos, int *row, int *col);

/* Lisp adapter only. */
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

/* ASCII classification, for the grammars that are ASCII by definition:
 * syntax scanning, local-variable parsing, key codes.  Prefer these to
 * <ctype.h> there.  They take an int so a key code above 0xFF or a
 * negative char can be passed safely -- <ctype.h> is undefined for any
 * negative value but EOF -- and they answer only about ASCII, where
 * <ctype.h> in the "C" locale kg always runs in says nothing useful
 * about a byte >= 0x80 and would call an ordinary UTF-8 lead byte
 * unprintable. */
static inline int ascii_is_print(int c) { return c >= 0x20 && c <= 0x7e; }
static inline int ascii_is_digit(int c) { return c >= '0' && c <= '9'; }
static inline int ascii_is_space(int c)
{
	return c == ' ' || (c >= '\t' && c <= '\r');
}

/* Tab stops sit every KG_TAB_WIDTH display columns, matching Emacs'
 * default tab-width, cat(1) and the terminal itself. */
#define KG_TAB_WIDTH 8

/* Coordinate-space checks, doc/coordinates.md's table turned into
 * something a build can fail on: KG_ASSERT_CHARS_OFF takes a byte offset
 * into row->chars, KG_ASSERT_RENDER_OFF one into row->render (which is
 * also how row->hl is indexed).  Off unless the build asks for them, the
 * same shape as KG_PERF_COUNTERS -- the shipped editor carries none of
 * it, and .ci/ci-04 turns them on for the sanitizer lane, which drives
 * the whole PTY suite.  Build by hand with
 * `make CFLAGS="-Wall -g -DKG_DEBUG_COORDS=1"`. */
#ifndef KG_DEBUG_COORDS
#define KG_DEBUG_COORDS 0
#endif
#if KG_DEBUG_COORDS
#include <assert.h>
#define KG_ASSERT_CHARS_OFF(row, off) assert((off) >= 0 && (off) <= (row)->size)
#define KG_ASSERT_RENDER_OFF(row, off)                                         \
	assert((off) >= 0 && (off) <= (row)->rsize)
#else
#define KG_ASSERT_CHARS_OFF(row, off) ((void)0)
#define KG_ASSERT_RENDER_OFF(row, off) ((void)0)
#endif

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

/* True when `path` names a directory.  A path that cannot be stat()ed --
 * missing, or behind an unreadable parent -- is not one. */
static inline int path_is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Truncate `path` in place to the directory containing it.  Returns 0, or
 * -1 when `path` holds no '/' and so names no parent.  The parent of "/"
 * is "/", so an absolute path stays absolute. */
static inline int path_parent_dir(char *path)
{
	char *slash = strrchr(path, '/');

	if (!slash) {
		return -1;
	}
	slash[slash == path ? 1 : 0] = '\0';
	return 0;
}

/* help.c */
extern const char *kg_help_lines[]; /* NULL-terminated, loaded into *help* */

/* display.c */
int ab_append(struct abuf *ab, const char *s, int len);
void ab_free(struct abuf *ab);
void editor_refresh_screen(void);
void editor_set_status_message(const char *fmt, ...);
void editor_set_status_emphasis(int start, int len);

/* fileio.c */
struct temp_load_result {
	char *filename;
	erow *row;
	int numrows;
	int row_capacity;
	struct file_snapshot disk;
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
extern ssize_t (*editor_read_fn)(int fd, void *buf, size_t count);
int file_read_all(int fd, char **out, size_t *out_len);
extern void (*editor_pre_rename_hook_fn)(const char *path);
int editor_write_rows_to_file(const char *filename, erow *rows, int numrows,
    int *out_len, const struct file_snapshot *accepted);
void editor_write_file(int fd);
void editor_insert_file(int fd);
void editor_snapshot_disk(void);
int file_snapshot_path(const char *path, struct file_snapshot *out);
int confirm_save_over_accepted(
    int fd, const char *path, struct file_snapshot *accepted);
int file_snapshot_fd(int fd, struct file_snapshot *out);
enum file_change_state file_snapshot_compare(
    const struct file_snapshot *accepted, const struct file_snapshot *now);
enum file_change_state file_snapshot_compare_path(
    const char *path, const struct file_snapshot *accepted);

/* kbd.c */
void editor_process_keypress(int fd);
[[nodiscard]] int editor_confirm_yn(int fd, const char *fmt, ...);

/* cmd.c */
void editor_named_command(int fd);
[[nodiscard]] int cmd_execute_named(const char *name, int fd);
[[nodiscard]] int cmd_invoke(
    const char *name, const struct command_context *ctx);
[[nodiscard]] const struct named_cmd *cmd_lookup(const char *name);
[[nodiscard]] const struct named_cmd *cmd_descriptor_at(int index);
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

bool shell_output_fits_echo(
    const char *out, int out_len, int available_columns, int reserved_columns);

/* syntax.c */
int is_separator(int c);
int editor_row_has_open_comment(erow *row);
void editor_update_syntax(struct editor_buffer *b, erow *row);
void editor_rehighlight_from(struct editor_buffer *b, int start_idx);
struct editor_syntax *syntax_find_by_name(const char *name);
void editor_set_syntax(struct editor_buffer *b, struct editor_syntax *syntax);
void editor_rehighlight_all(struct editor_buffer *b);
int editor_syntax_to_color(int hl);
void editor_select_syntax_highlight(struct editor_buffer *b, char *filename);
[[nodiscard]] int syntax_is_git_commit(void);
[[nodiscard]] int syntax_git_commit_subject(void);
[[nodiscard]] int syntax_is_git_rebase(void);
int syntax_git_rebase_pick_span(
    const char *line, int len, int *start, int *wlen);
int syntax_git_rebase_flags_end(const char *line, int len, int from);

/* dired.c — syntax_is_dired() lives here rather than in syntax.c because
 * the Dired syntax record is identified by its address, not by the
 * highlighter pointer the other syntax_is_* helpers compare. */
[[nodiscard]] int syntax_is_dired(void);
int dired_open(const char *dir);
int dired_fill_current(const char *dir);
int dired_dir_of(const char *bufname, char *out, int size);
int dired_row_name(const char *line, int len, char *out, int size);

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

/* What a flagged listing row named, and what it was, at the moment the user
 * was asked about it.  Compared as bytes, so it is laid out without padding
 * the way struct file_identity is. */
struct dired_identity {
	uint64_t device;
	uint64_t inode;
	uint64_t type; /* st_mode & S_IFMT */
};

struct dired_target {
	char name[NAME_MAX + 1];
	struct dired_identity id;
};

int dired_collect_flagged(int dirfd, struct dired_target *out, int max);
int dired_delete_verified(int dirfd, const struct dired_target *target);
void dired_find_file(void);
void dired_up_directory(void);
void dired_revert(void);
void dired_set_mark(char mark);
void dired_do_flagged_delete(int fd);

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
/* Smallest terminal kg lays out for: one text row, one mode line and one
 * echo row, and columns enough for the mode line and the tilde/banner
 * filler to land somewhere.  Largest: the frame buffer's arithmetic is
 * int (display.c), so rows * cols stays far from INT_MAX, and no
 * terminal is this big -- a probe that says so is lying. */
#define KG_MIN_ROWS 3
#define KG_MIN_COLS 8
#define KG_MAX_ROWS 10000
#define KG_MAX_COLS 10000

int tty_write(const void *buf, size_t n);
int kg_parse_cursor_report(const char *buf, int *rows, int *cols);
int kg_normalize_window_size(int rows, int cols, int *out_rows, int *out_cols);
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

/* Longest visible spelling display_glyph_at() produces ("\xnn"), with
 * one byte to spare so callers may NUL-terminate it. */
#define KG_DISPLAY_ESCAPE_MAX 5

/* One glyph of text as the renderer draws it.  `bytes`/`len` is what
 * goes to the terminal, `span` how many source bytes it stood for, and
 * `width` how many cells it occupies. */
struct display_glyph {
	const char *bytes;
	int len;
	int span;
	int width;
	char esc[KG_DISPLAY_ESCAPE_MAX];
};

void display_glyph_at(
    const char *buf, int len, int start, struct display_glyph *g);

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
void undo_stack_init(struct undo_stack *st);
void undo_stack_free(struct undo_stack *st);
void undo_init(void);
void undo_free(void);
int undo_push(struct editor_buffer *b, enum undo_type type, int row, int col,
    int c, char *text, int len);
int undo_push_change(struct editor_buffer *b, size_t position, char *old_text,
    int old_len, int new_len);
void editor_undo(void);
void undo_mark_clean(void);

/* mode.c */
int get_visual_row(erow *rows, int numrows, int win_w, int cy, int cx);
int visual_line_cursor_col(erow *row, int chars_col, int win_w);
void find_visual_row(erow *rows, int numrows, int win_w, int rowoff_visual,
    int target_y, int *logical_row, int *render_offset);
int visual_col_to_chars(erow *row, int target_rcol, int win_w);
int visual_line_width(erow *row, int win_w);
void goto_visual_row_col(int target_vrow, int target_rcol_in_segment);
int chars_to_render_col(erow *row, int chars_col);
int get_total_visual_rows(erow *rows, int numrows, int win_w);

/* main.c */
void init_editor(void);

#endif /* DEF_H */
