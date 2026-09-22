#ifndef KG_MINIBUF_H
#define KG_MINIBUF_H

/* The minibuffer line editor: one line of text read in the echo area,
 * with Emacs' in-minibuffer editing (motion, kill and yank, case words,
 * C-u counts, C-q) and an optional M-p/M-n history ring.
 * editor_read_line() and editor_read_line_with_history() are the whole
 * reader.  The path reader and the buffer-name picker in bufmgr.c have
 * their own key loops around the same editing core, which is why
 * minibuf_edit_key() and the two prompt_* helpers are exported below.
 *
 * Self-contained: bufmgr.h is where `enum minibuf_result' lives, and
 * every reader in kg answers with it. */

#include "bufmgr.h"

struct key_event;

/* `buf` is read before it is written: whatever it already holds, as
 * measured by strnlen(buf, bufsize), becomes the prompt's initial text.
 * A caller wanting an empty prompt must set buf[0] = '\0' first --
 * passing an uninitialized array reads uninitialized memory and
 * prefills the minibuffer with whatever the stack held.  The same holds
 * for editor_read_line_path() (def.h). */
enum minibuf_result editor_read_line(
    int fd, const char *prompt, char *buf, int bufsize);

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
void minibuf_delete_backward(char *buf, int *cursor, int *len, int *overflow);

/* The prompt-local yank-pop record: which kill ring entry the previous
 * keystroke's C-y or M-y inserted, where and how long, so M-y can
 * replace that span with the next-older entry.  One per prompt read
 * loop, zero-initialized at prompt entry.  The loop clears `valid` into
 * `eligible` at every keystroke and only a successful yank sets it
 * again, so eligibility means exactly "the directly preceding prompt
 * keystroke was a yank" with no invalidation bookkeeping anywhere else.
 * The buffer-side yank_pop_state (yank.c) cannot be reused: its gate is
 * cmd_last_kill_class(), whose transient the prompt entry clears, and
 * its span is a buffer marker where a prompt is a bare char array.
 * `note` is a one-shot minibuffer-message-style " [text]" for the next
 * repaint; prompts without that mechanism (the path picker) drop it
 * silently. */
struct minibuf_yank {
	int valid;
	int eligible;
	int index;
	int start;
	int len;
	const char *note;
};

/* One editing key applied to `buf`: returns 1 when `c` was an editing
 * key (consumed, `buf`/`cursor`/`len`/`overflow` updated), 0 when the
 * caller's own loop should look at it.  `yank` may be NULL, which
 * leaves C-y and M-y inserting nothing. */
int minibuf_edit_key(int fd, struct key_event c, char *buf, int bufsize,
    int *cursor, int *len, int *overflow, struct minibuf_yank *yank);

/* Every minibuffer read's one exit: resets the echo-area cursor, clears
 * the status line on a cancel (`rc` < 0), and balances the read's
 * kg_event_prompt_enter().  Returns `rc`. */
int minibuf_prompt_done(int rc);

/* 1-based echo-area column of a cursor `cursor` bytes into `buf` behind
 * `prompt` (`plen` bytes), in display cells. */
int minibuf_prompt_cursor_col(
    const char *prompt, int plen, const char *buf, int cursor);

#endif /* KG_MINIBUF_H */
