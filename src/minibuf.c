/* ============================ Minibuffer line editor ======================
 *
 * See minibuf.h: the echo-area line reader, its editing keys, and the
 * M-p/M-n history ring.  Moved out of bufmgr.c, which keeps the path
 * reader and the buffer-name picker built on top of it. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmdstate.h"
#include "def.h"
#include "event.h"
#include "keyevent.h"
#include "minibuf.h"
#include "paste.h"
#include "prefixarg.h"
#include "word.h"
#include "yank.h"

/* Key sets the reader asks about, spelled as bufmgr.c and prompt.c
 * spell theirs; see key_in_list(). */
static const struct key_event erase_keys[]
    = { { KEY_BASE_DELETE, 0 }, { 'h', KEY_MOD_CTRL }, { KEY_BASE_DEL, 0 } };
static const struct key_event history_keys[] = { { 'p', KEY_MOD_META },
	{ 'n', KEY_MOD_META }, { KEY_BASE_UP, 0 }, { KEY_BASE_DOWN, 0 },
	{ 'p', KEY_MOD_CTRL }, { 'n', KEY_MOD_CTRL } };
static const struct key_event history_back_keys[]
    = { { 'p', KEY_MOD_META }, { KEY_BASE_UP, 0 }, { 'p', KEY_MOD_CTRL } };
static const struct key_event cancel_keys[]
    = { { KEY_BASE_ESC, 0 }, { 'g', KEY_MOD_CTRL } };

/* Reset the echo-area cursor state and clear the status line.  Centralises
 * the "leaving the minibuffer" handshake so every exit path agrees --
 * including, now, kg_event_prompt_leave(): every read that entered through
 * kg_event_prompt_enter() (editor_read_line_with_history(),
 * editor_read_line_path()) returns through exactly this one function, so
 * this is the one place that balances it. */
int minibuf_prompt_done(int rc)
{
	editor.echo_cursor_col = 0;
	if (rc < 0) {
		editor_set_status_message("");
	}
	/* The prompt's own kills must not coalesce with whatever the buffer
	 * does next -- in Emacs the minibuffer's exit ran commands in
	 * between.  Clearing the running class makes the next keystroke's
	 * cmd_last_kill_class() read NONE; a command that kills after its
	 * prompt returns still registers its own class normally. */
	cmd_set_kill_class(KILL_COALESCE_NONE);
	kg_event_prompt_leave();
	return rc;
}

/* 1-based echo-area column for a cursor sitting `cursor` bytes into
 * `buf` behind `prompt`.  Measured in display cells, so a multi-byte
 * character moves the cursor one column, not one column per byte. */
int minibuf_prompt_cursor_col(
    const char *prompt, int plen, const char *buf, int cursor)
{
	return utf8_display_width(prompt, plen)
	    + utf8_display_width(buf, cursor) + 1;
}

/* Park the cursor at the typed position on the echo area and refresh. */
static void prompt_refresh(
    const char *prompt, int plen, const char *buf, int cursor)
{
	editor_set_status_message("%s%s", prompt, buf);
	editor.echo_cursor_col
	    = minibuf_prompt_cursor_col(prompt, plen, buf, cursor);
	editor_refresh_screen();
}

/* Splice `n` bytes at the cursor, counting an overflow when the prompt
 * buffer is full.  Insertion is all-or-nothing so a multi-byte sequence
 * is never cut in half by the buffer limit. */
static void minibuf_insert(char *buf, int bufsize, int *cursor, int *len,
    int *overflow, const char *bytes, int n)
{
	if (*len + n >= bufsize) {
		(*overflow)++;
		return;
	}
	memmove(buf + *cursor + n, buf + *cursor, *len - *cursor + 1);
	memcpy(buf + *cursor, bytes, (size_t)n);
	*cursor += n;
	*len += n;
}

/* Bytes of `text` a prompt yank inserts: the prompt is a NUL-terminated
 * single line, so the span stops at the ring entry's first embedded NUL.
 * An Emacs minibuffer, being a real buffer, would take the whole entry;
 * this comment pins the divergence.  Newlines are NOT special: they
 * enter the prompt as bytes, exactly as C-q C-j types one. */
static int minibuf_yank_span(const char *text, size_t len)
{
	const char *nul = memchr(text, '\0', len);

	return (int)(nul != NULL ? (size_t)(nul - text) : len);
}

/* A prompt kill: remove [from, to) from buf and record it in the real
 * kill ring -- forward kills append to the directly preceding prompt
 * kill's entry, backward kills prepend, exactly the buffer rules
 * (yank.h).  cmd_state_prompt_keystroke() in the prompt read loops is
 * what scopes "directly preceding" to the previous prompt keystroke,
 * and minibuf_prompt_done() is what keeps the last prompt kill from coalescing
 * with the buffer kill that runs after the prompt returns. */
static void minibuf_kill_span(
    char *buf, int *cursor, int *len, int from, int to, int backward)
{
	int n = to - from;

	if (n <= 0) {
		return;
	}
	if (backward) {
		kill_ring_kill_backward(buf + from, (size_t)n);
	} else {
		kill_ring_kill_forward(buf + from, (size_t)n);
	}
	memmove(buf + from, buf + to, (size_t)(*len - to + 1));
	*len -= n;
	*cursor = from;
}

/* C-y: insert the kill ring's newest entry at the cursor and record the
 * span for M-y.  Whole-or-nothing on capacity, like the private-store
 * C-y this replaces: a span that does not fit is refused without
 * touching the overflow count, since nothing entered the prompt. */
static void minibuf_yank(char *buf, int bufsize, int *cursor, int *len,
    int *overflow, struct minibuf_yank *yank)
{
	const char *text = kill_ring_get();
	int n, at;

	if (text == NULL) {
		if (yank != NULL) {
			yank->note = "Kill ring is empty";
		}
		return;
	}
	n = minibuf_yank_span(text, kill_ring_get_len());
	if (n == 0 || *len + n >= bufsize) {
		return;
	}
	at = *cursor;
	minibuf_insert(buf, bufsize, cursor, len, overflow, text, n);
	if (yank != NULL) {
		yank->valid = 1;
		yank->index = 0;
		yank->start = at;
		yank->len = n;
	}
}

/* M-y: replace the span the directly preceding C-y or M-y inserted with
 * the next-older ring entry, wrapping past the oldest, cursor after it,
 * as Emacs' yank-pop does in a minibuffer.  Without a preceding yank,
 * Emacs 28+ browses the ring interactively (yank-from-kill-ring); kg
 * has no completion framework and answers the classic eligibility
 * message instead.  The span bound check is a belt against a caller
 * that edited buf without the per-keystroke eligibility reset. */
static void minibuf_yank_pop(char *buf, int bufsize, int *cursor, int *len,
    int *overflow, struct minibuf_yank *yank)
{
	char *text;
	size_t elen = 0;
	int n, next;

	if (yank == NULL) {
		return;
	}
	if (!yank->eligible || killring.count == 0
	    || yank->start + yank->len > *len) {
		yank->note = "Previous command was not a yank";
		return;
	}
	next = (yank->index + 1) % killring.count;
	text = kill_ring_entry_repeated(next, 1, &elen);
	if (text == NULL) {
		return;
	}
	n = minibuf_yank_span(text, elen);
	if (*len - yank->len + n < bufsize) {
		memmove(buf + yank->start, buf + yank->start + yank->len,
		    (size_t)(*len - yank->start - yank->len + 1));
		*len -= yank->len;
		*cursor = yank->start;
		minibuf_insert(buf, bufsize, cursor, len, overflow, text, n);
		yank->valid = 1;
		yank->index = next;
		yank->len = n;
	}
	free(text);
}

/* One repaint of the prompt line, minibuffer-message style: when the
 * previous keystroke left a note (an empty kill ring, a M-y with no
 * yank before it), it is shown once as " [note]" after the text, the
 * way Emacs' minibuffer-message appends its complaint, and cleared so
 * the next repaint is ordinary. */
static void minibuf_prompt_paint(const char *prompt, int plen, const char *buf,
    int cursor, struct minibuf_yank *yank)
{
	if (yank->note != NULL) {
		editor_set_status_message("%s%s [%s]", prompt, buf, yank->note);
		editor.echo_cursor_col
		    = minibuf_prompt_cursor_col(prompt, plen, buf, cursor);
		editor_refresh_screen();
		yank->note = NULL;
		return;
	}
	prompt_refresh(prompt, plen, buf, cursor);
}

/* Offset just past the non-word run and the word that follow `pos`,
 * i.e. where M-f lands. */
static int minibuf_word_end(const char *buf, int len, int pos)
{
	while (pos < len && !kg_is_word_char((unsigned char)buf[pos])) {
		pos++;
	}
	while (pos < len && kg_is_word_char((unsigned char)buf[pos])) {
		pos++;
	}
	return pos;
}

/* Offset at the start of the word preceding `pos`, i.e. where M-b
 * lands. */
static int minibuf_word_start(const char *buf, int pos)
{
	while (pos > 0 && !kg_is_word_char((unsigned char)buf[pos - 1])) {
		pos--;
	}
	while (pos > 0 && kg_is_word_char((unsigned char)buf[pos - 1])) {
		pos--;
	}
	return pos;
}

/* M-u / M-l / M-c, or 0 when `c` is none of them. */
static int minibuf_case_mode_for(struct key_event c)
{
	if (KEY_IS(c, 'u', KEY_MOD_META)) {
		return 'u';
	}
	if (KEY_IS(c, 'l', KEY_MOD_META)) {
		return 'l';
	}
	if (KEY_IS(c, 'c', KEY_MOD_META)) {
		return 'c';
	}
	return 0;
}

/* Insert `repeat` copies of the `unit_len` bytes at `unit`, all at once
 * so a repeat that does not fit refuses whole rather than half. */
static void minibuf_insert_repeated(char *buf, int bufsize, int *cursor,
    int *len, int *overflow, const char *unit, int unit_len, int repeat)
{
	int total, i;

	if (repeat <= 0 || unit_len <= 0) {
		return;
	}
	if (repeat > PREFIX_ARG_MAX) {
		repeat = PREFIX_ARG_MAX;
	}
	if (unit_len > 0 && repeat > INT_MAX / unit_len) {
		(*overflow)++;
		return;
	}
	total = unit_len * repeat;
	if (*len + total >= bufsize) {
		(*overflow)++;
		return;
	}
	memmove(
	    buf + *cursor + total, buf + *cursor, (size_t)(*len - *cursor + 1));
	for (i = 0; i < repeat; i++) {
		memcpy(buf + *cursor + i * unit_len, unit, (size_t)unit_len);
	}
	*cursor += total;
	*len += total;
}

/* Upcase, downcase or capitalize the word forward from the cursor and
 * leave the cursor past it, as Emacs' M-u/M-l/M-c do in the minibuffer.
 * The span is minibuf_word_end()'s, the one M-f and M-d already use, so
 * no two of the prompt's word commands can disagree about where a word
 * ends; the byte rule is word.h's, shared with the buffer's M-u.  The
 * text only changes case, so it can never outgrow the prompt's buffer
 * and there is no overflow to account for. */
static void minibuf_case_word(char *buf, int len, int *cursor, int mode)
{
	int end = minibuf_word_end(buf, len, *cursor);
	int start = *cursor;
	int i;

	while (start < end && !kg_is_word_char((unsigned char)buf[start])) {
		start++;
	}
	for (i = start; i < end; i++) {
		buf[i] = (char)kg_word_case_byte(
		    mode, (unsigned char)buf[i], i == start);
	}
	*cursor = end;
}

/* Delete the whole character before the cursor, not just its last byte:
 * a multi-byte glyph would otherwise leave a stray continuation byte
 * behind and corrupt everything typed after it. */
void minibuf_delete_backward(char *buf, int *cursor, int *len, int *overflow)
{
	int start;

	if (*overflow > 0) {
		(*overflow)--;
		return;
	}
	if (*cursor <= 0) {
		return;
	}
	start = utf8_glyph_start_before(buf, *len, *cursor);
	memmove(buf + start, buf + *cursor, *len - *cursor + 1);
	*len -= *cursor - start;
	*cursor = start;
}

/* The four cursor motions minibuf_edit_key() answers as plain keys, each
 * reachable by a Ctrl letter or the matching arrow/Home/End -- a table
 * instead of four KEY_IS() || KEY_IS() pairs, in the shape
 * isearch_handoff_direction() (search.c) already uses for the same
 * problem. */
enum minibuf_motion {
	MINIBUF_MOTION_NONE,
	MINIBUF_MOTION_LEFT,
	MINIBUF_MOTION_RIGHT,
	MINIBUF_MOTION_HOME,
	MINIBUF_MOTION_END,
};

static const struct {
	struct key_event key;
	enum minibuf_motion motion;
} minibuf_motions[] = {
	{ { 'f', KEY_MOD_CTRL }, MINIBUF_MOTION_RIGHT },
	{ { KEY_BASE_RIGHT, 0 }, MINIBUF_MOTION_RIGHT },
	{ { 'b', KEY_MOD_CTRL }, MINIBUF_MOTION_LEFT },
	{ { KEY_BASE_LEFT, 0 }, MINIBUF_MOTION_LEFT },
	{ { 'a', KEY_MOD_CTRL }, MINIBUF_MOTION_HOME },
	{ { KEY_BASE_HOME, 0 }, MINIBUF_MOTION_HOME },
	{ { 'e', KEY_MOD_CTRL }, MINIBUF_MOTION_END },
	{ { KEY_BASE_END, 0 }, MINIBUF_MOTION_END },
};

static enum minibuf_motion minibuf_motion_for(struct key_event c)
{
	size_t i;

	for (i = 0; i < sizeof(minibuf_motions) / sizeof(*minibuf_motions);
	    i++) {
		if (key_event_equal(minibuf_motions[i].key, c)) {
			return minibuf_motions[i].motion;
		}
	}
	return MINIBUF_MOTION_NONE;
}

int minibuf_edit_key(int fd, struct key_event c, char *buf, int bufsize,
    int *cursor, int *len, int *overflow, struct minibuf_yank *yank)
{
	char seq[4];
	int n, raw, mode;

	if (KEY_IN_LIST(erase_keys, c)) {
		minibuf_delete_backward(buf, cursor, len, overflow);
		return 1;
	}
	if (KEY_IS(c, 'd', KEY_MOD_CTRL)) {
		if (*cursor < *len) {
			int span = utf8_glyph_span_at(buf, *len, *cursor);
			memmove(buf + *cursor, buf + *cursor + span,
			    *len - *cursor - span + 1);
			*len -= span;
		}
		return 1;
	}
	switch (minibuf_motion_for(c)) {
	case MINIBUF_MOTION_RIGHT:
		if (*cursor < *len) {
			*cursor += utf8_glyph_span_at(buf, *len, *cursor);
		}
		return 1;
	case MINIBUF_MOTION_LEFT:
		if (*cursor > 0) {
			*cursor = utf8_glyph_start_before(buf, *len, *cursor);
		}
		return 1;
	case MINIBUF_MOTION_HOME:
		*cursor = 0;
		return 1;
	case MINIBUF_MOTION_END:
		*cursor = *len;
		return 1;
	case MINIBUF_MOTION_NONE:
		break;
	}
	if (c.base == KEY_BASE_PASTE) {
		size_t plen = 0;
		const char *pdata = kg_bracketed_paste_data(&plen);
		if (pdata && plen > 0) {
			minibuf_insert(buf, bufsize, cursor, len, overflow,
			    pdata, (int)plen);
		}
		kg_bracketed_paste_clear();
		return 1;
	}
	if (KEY_IS(c, 'k', KEY_MOD_CTRL)) {
		minibuf_kill_span(buf, cursor, len, *cursor, *len, 0);
		return 1;
	}
	if (KEY_IS(c, 'y', KEY_MOD_CTRL)) {
		minibuf_yank(buf, bufsize, cursor, len, overflow, yank);
		return 1;
	}
	if (KEY_IS(c, 'y', KEY_MOD_META)) {
		minibuf_yank_pop(buf, bufsize, cursor, len, overflow, yank);
		return 1;
	}
	if (KEY_IS(c, 'f', KEY_MOD_META)) {
		*cursor = minibuf_word_end(buf, *len, *cursor);
		return 1;
	}
	if (KEY_IS(c, 'b', KEY_MOD_META)) {
		*cursor = minibuf_word_start(buf, *cursor);
		return 1;
	}
	if (KEY_IS(c, 'd', KEY_MOD_META)) {
		minibuf_kill_span(buf, cursor, len, *cursor,
		    minibuf_word_end(buf, *len, *cursor), 0);
		return 1;
	}
	if (KEY_IS(c, KEY_BASE_DEL, KEY_MOD_META)) {
		minibuf_kill_span(buf, cursor, len,
		    minibuf_word_start(buf, *cursor), *cursor, 1);
		return 1;
	}
	mode = minibuf_case_mode_for(c);
	if (mode != 0) {
		minibuf_case_word(buf, *len, cursor, mode);
		return 1;
	}
	if (KEY_IS(c, 'q', KEY_MOD_CTRL)) {
		raw = editor_read_raw_byte(fd);
		if (!running) {
			return 1;
		}
		seq[0] = (char)raw;
		minibuf_insert(buf, bufsize, cursor, len, overflow, seq, 1);
		return 1;
	}
	/* Every plain-character path below is for an unmodified base: Ctrl-x
	 * is not the letter x, and nothing here should treat it as one. */
	if (c.mods != 0) {
		return 0;
	}
	if (ascii_is_print(c.base)) {
		seq[0] = (char)c.base;
		minibuf_insert(buf, bufsize, cursor, len, overflow, seq, 1);
		return 1;
	}
	/* A byte above ASCII is the lead of a multi-byte character the
	 * terminal is sending one byte at a time; pull in the rest so the
	 * whole glyph enters the prompt together. */
	n = editor_read_utf8_seq(fd, (int)c.base, seq);
	if (n > 0) {
		minibuf_insert(buf, bufsize, cursor, len, overflow, seq, n);
		return 1;
	}
	if (c.base >= 0x80 && c.base <= 0xFF) {
		return 1; /* malformed sequence: swallow the stray byte */
	}
	return 0;
}

/* Prompt the user for a line of text in the status bar.  Returns the shared
 * minibuffer result and always leaves buf NUL-terminated. */
enum minibuf_result editor_read_line(
    int fd, const char *prompt, char *buf, int bufsize)
{
	return editor_read_line_with_history(fd, prompt, buf, bufsize, NULL);
}

/* Explicit init for callers that don't get zero-initialization for free
 * (e.g. a stack-allocated struct minibuf_history).  Equivalent to the
 * all-zero state static/global storage already starts in. */
void minibuf_history_init(struct minibuf_history *hist)
{
	if (!hist) {
		return;
	}
	memset(hist, 0, sizeof(*hist));
}

/* Record `text` as the newest history entry, deduplicating an immediate
 * repeat of the last entry.  Text too long for a slot is dropped rather
 * than stored truncated: a recalled prefix of a compile command or a Lisp
 * expression is not what the user ran.  Oldest entry is overwritten once
 * the ring fills. */
void minibuf_history_add(struct minibuf_history *hist, const char *text)
{
	if (!hist || !text || !text[0]
	    || strlen(text) >= MINIBUF_HISTORY_ENTRY_MAX) {
		return;
	}
	if (hist->count > 0 && strcmp(hist->entries[hist->head], text) == 0) {
		return;
	}
	hist->head
	    = hist->count == 0 ? 0 : (hist->head + 1) % MINIBUF_HISTORY_MAX;
	snprintf(
	    hist->entries[hist->head], MINIBUF_HISTORY_ENTRY_MAX, "%s", text);
	if (hist->count < MINIBUF_HISTORY_MAX) {
		hist->count++;
	}
}

/* index 0 is the newest entry; returns NULL when out of range. */
const char *minibuf_history_get(const struct minibuf_history *hist, int index)
{
	int phys;

	if (!hist || index < 0 || index >= hist->count) {
		return NULL;
	}
	phys = hist->head - index;
	if (phys < 0) {
		phys += MINIBUF_HISTORY_MAX;
	}
	return hist->entries[phys];
}

/* Step `*index` (-1 while the caller's own draft is being edited, 0 at the
 * newest entry) one place in `dir`: +1 walks toward older entries (M-p),
 * -1 back toward newer ones (M-n).  Returns the text to show — `draft`
 * once the walk comes back past the newest entry — or NULL when the walk
 * runs off an end, in which case `*index` is left alone. */
const char *minibuf_history_walk(
    const struct minibuf_history *hist, int dir, int *index, const char *draft)
{
	if (!hist) {
		return NULL;
	}
	if (dir > 0) {
		return *index + 1 < hist->count
		    ? minibuf_history_get(hist, ++*index)
		    : NULL;
	}
	if (*index > 0) {
		return minibuf_history_get(hist, --*index);
	}
	if (*index == 0) {
		*index = -1;
		return draft;
	}
	return NULL;
}

/* Feed `c` to the prompt's own count.  Returns 1 when the count took
 * the key (C-u, a digit, or ESC/C-g cancelling it).  Otherwise, when a
 * count has just ended at `c`, sets `*have_repeat` and `*repeat` for
 * the key it applies to. */
static int minibuf_count_key(struct prefix_accum *count, struct key_event c,
    bool *have_repeat, int *repeat)
{
	if (count->pending && KEY_IN_LIST(cancel_keys, c)) {
		prefix_accum_clear(count);
		return 1;
	}
	switch (prefix_accum_feed(count, c)) {
	case PREFIX_STEP_TAKEN:
		return 1;
	case PREFIX_STEP_ENDS:
		*repeat = count->arg;
		*have_repeat = true;
		prefix_accum_clear(count);
		break;
	case PREFIX_STEP_NONE:
		break;
	}
	return 0;
}

/* A counted self-insert: `repeat` copies of what `c` types (a printing
 * character, a UTF-8 character, or C-q's next byte).  Returns 0 when `c`
 * types nothing, and then the caller drops the count. */
static int minibuf_insert_counted(int fd, struct key_event c, char *buf,
    int bufsize, int *cursor, int *len, int *overflow, int repeat)
{
	char seq[4];
	int seqlen = 0;

	if (KEY_IS(c, 'q', KEY_MOD_CTRL)) {
		int raw = editor_read_raw_byte(fd);

		if (!running) {
			return 1;
		}
		seq[0] = (char)raw;
		seqlen = 1;
	} else if (c.mods == 0 && ascii_is_print(c.base)) {
		seq[0] = (char)c.base;
		seqlen = 1;
	} else if (c.mods == 0 && c.base >= 0x80 && c.base <= 0xFF) {
		seqlen = editor_read_utf8_seq(fd, (int)c.base, seq);
	} else {
		return 0;
	}
	if (seqlen > 0) {
		minibuf_insert_repeated(
		    buf, bufsize, cursor, len, overflow, seq, seqlen, repeat);
	}
	return 1;
}

/* What M-p saves before it replaces the typed text with history, and
 * M-n puts back once it walks past the newest entry.  `index` is -1
 * while the draft itself is being edited. */
struct minibuf_draft {
	char *text;
	int cursor;
	int overflow;
	int index;
};

/* One M-p (`dir` 1) or M-n (`dir` -1) step through `hist`. */
static void minibuf_history_step(struct minibuf_history *hist, int dir,
    struct minibuf_draft *d, char *buf, int bufsize, int *cursor, int *len,
    int *overflow)
{
	const char *entry;

	if (dir > 0 && d->index < 0) {
		snprintf(d->text, bufsize, "%s", buf);
		d->cursor = *cursor;
		d->overflow = *overflow;
	}
	entry = minibuf_history_walk(hist, dir, &d->index, d->text);
	if (!entry) {
		return;
	}
	*len = (int)strnlen(entry, bufsize - 1);
	memmove(buf, entry, *len);
	buf[*len] = '\0';
	if (d->index < 0) {
		*cursor = d->cursor;
		*overflow = d->overflow;
	} else {
		*cursor = *len;
		*overflow = 0;
	}
}

/* Like editor_read_line(), but M-p/M-n (also Up/Down and C-p/C-n) walk
 * `hist` (newest first).  The in-progress typed text, cursor position, and
 * overflow count are preserved as a "draft" and restored once navigation
 * walks back past the newest entry.  hist may be NULL to disable history
 * navigation (equivalent to editor_read_line()).  Accepted input (Enter) is
 * pushed onto hist as the newest entry. */
enum minibuf_result editor_read_line_with_history(int fd, const char *prompt,
    char *buf, int bufsize, struct minibuf_history *hist)
{
	int plen = (int)strlen(prompt);
	int len = (int)strnlen(buf, bufsize - 1);
	int cursor = len;
	int overflow = 0;
	struct key_event c;
	char draft_text[bufsize];
	struct minibuf_draft draft = { draft_text, cursor, 0, -1 };
	struct minibuf_yank yank = { 0 };
	/* C-u 4 SPC inserts four spaces at a prompt such as
	 * string-rectangle's.  The count is local: editor.uarg belongs to
	 * the top-level dispatcher, and the command that asked has already
	 * taken its own. */
	struct prefix_accum count = { 0 };

	buf[len] = '\0';
	/* kg_event_drain_safe() must defer for the whole of this read, not
	 * just its final keystroke: the prompt is on screen and mid-edit
	 * from its first character, so a callback repainting the echo area
	 * (or killing the buffer the read is scoped to) could otherwise run
	 * underneath it.  Balanced by minibuf_prompt_done(), this function's
	 * one exit path. */
	kg_event_prompt_enter();
	/* Keystrokes read here are the prompt's, not commands: nothing that
	 * happens inside it may look like a repeat of what ran before it. */
	cmd_clear_transient();
	while (1) {
		int repeat = 1;
		bool have_repeat = false;

		minibuf_prompt_paint(prompt, plen, buf, cursor, &yank);
		c = editor_read_key(fd);
		/* Every prompt keystroke is its own kill-class boundary, and
		 * yank-pop eligibility is exactly "the key before this one
		 * was a yank": clear it here, and only a successful C-y/M-y
		 * sets it again. */
		cmd_state_prompt_keystroke();
		yank.eligible = yank.valid;
		yank.valid = 0;
		if (minibuf_count_key(&count, c, &have_repeat, &repeat)) {
			continue;
		}
		if (have_repeat
		    && minibuf_insert_counted(fd, c, buf, bufsize, &cursor,
			&len, &overflow, repeat)) {
			continue;
		}
		/* Any other key discards the pending count. */
		if (hist && KEY_IN_LIST(history_keys, c)) {
			minibuf_history_step(hist,
			    KEY_IN_LIST(history_back_keys, c) ? 1 : -1, &draft,
			    buf, bufsize, &cursor, &len, &overflow);
			continue;
		}
		if (minibuf_edit_key(
			fd, c, buf, bufsize, &cursor, &len, &overflow, &yank)) {
			continue;
		}
		if (KEY_IN_LIST(cancel_keys, c)) {
			return minibuf_prompt_done(-1);
		} else if (KEY_IS(c, KEY_BASE_RET, 0)) {
			if (overflow == 0) {
				minibuf_history_add(hist, buf);
			}
			return minibuf_prompt_done(overflow > 0);
		}
	}
}