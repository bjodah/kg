/* ============================ Minibuffer questions =========================
 *
 * See prompt.h: the completing read over a caller-supplied candidate list
 * and the cancellable y/n question.  Both are echo-area reads with their
 * own key loop, so both bracket themselves with the event-drain deferral
 * every other prompt takes (kg_event_prompt_enter/leave) -- a callback
 * repainting the status line under a question that is still waiting for
 * its answer is exactly what that pair exists to prevent. */

#include <stdio.h>
#include <string.h>

#include "def.h"
#include "event.h"
#include "keyevent.h"
#include "prompt.h"

/* Key sets this picker asks about, spelled as bufmgr.c and cmd.c spell
 * theirs; see key_in_list(). */
static const struct key_event erase_keys[]
    = { { KEY_BASE_DELETE, 0 }, { 'h', KEY_MOD_CTRL }, { KEY_BASE_DEL, 0 } };
static const struct key_event cancel_keys[]
    = { { KEY_BASE_ESC, 0 }, { 'g', KEY_MOD_CTRL } };
static const struct key_event picker_next_keys[]
    = { { KEY_BASE_RIGHT, 0 }, { 'f', KEY_MOD_CTRL } };
static const struct key_event picker_prev_keys[]
    = { { KEY_BASE_LEFT, 0 }, { 'b', KEY_MOD_CTRL } };

/* The candidate array, shaped so editor_picker_filter() can walk it. */
struct prompt_choices {
	const char *const *names;
	int n;
};

static const char *prompt_choice_at(int index, void *data)
{
	const struct prompt_choices *choices = data;

	return index < choices->n ? choices->names[index] : NULL;
}

/* Everything one keystroke may change.  `explicit_selection` is M-x's own
 * flag: Enter on an empty query answers with the caller's default until
 * the user has moved the highlight themselves, and typing, erasing or
 * completing puts the picker back in that state. */
struct prompt_choice_state {
	char query[PROMPT_CHOICE_QUERY_MAX];
	int qlen;
	int sel;
	int overflow;
	bool explicit_selection;
};

/* One redraw; returns how many candidates the query matches. */
static int prompt_choice_redraw(const char *prompt,
    struct prompt_choice_state *st, const struct prompt_choices *choices,
    const char **names)
{
	char msg[512];
	int off = 0, sel_off, matches, nprefix;

	matches = editor_picker_filter(prompt_choice_at, (void *)choices,
	    st->query, names, NULL, PROMPT_CHOICE_MAX, &nprefix);
	if (st->sel >= matches) {
		st->sel = matches > 0 ? matches - 1 : 0;
	}
	editor_msg_appendf(msg, sizeof(msg), &off, "%s%s ", prompt, st->query);
	sel_off = editor_picker_render(
	    msg, sizeof(msg), &off, names, matches, matches, st->sel);
	editor_set_status_message("%s", msg);
	editor_picker_emphasise(sel_off, names, matches, st->sel);
	editor.echo_cursor_col = (int)strlen(prompt) + st->qlen + 1;
	editor_refresh_screen();
	return matches;
}

/* Deliver `text` unless it does not fit: a truncated answer is never
 * handed back as if it were the answer. */
static int prompt_choice_deliver(const char *text, char *out, int outsize)
{
	if ((int)strlen(text) >= outsize) {
		return MINIBUF_OVERFLOW;
	}
	strcpy(out, text);
	return MINIBUF_ACCEPTED;
}

/* Which match is exactly what was typed, or -1.  An exact match wins over
 * the highlight, which is Emacs' completing-read rule and not kg's M-x
 * rule: M-x runs the first *prefix* match in table order, so a built-in
 * `show-paren-mode' shadows a user's `show-p' (doc/TODO.md records that
 * as open).  A user cycling the highlight themselves outranks both. */
static int prompt_choice_exact(
    const struct prompt_choice_state *st, int matches, const char **names)
{
	int i;

	for (i = 0; i < matches; i++) {
		if (strcmp(names[i], st->query) == 0) {
			return i;
		}
	}
	return -1;
}

/* Enter.  Answers MINIBUF_*, or -2 to keep reading (the require-match
 * miss, which re-prompts exactly as the `b' interactive code does). */
static int prompt_choice_accept(struct prompt_choice_state *st, int matches,
    const char **names, bool require_match, char *out, int outsize)
{
	int exact;

	if (st->overflow > 0) {
		return MINIBUF_OVERFLOW;
	}
	if (st->qlen == 0 && !st->explicit_selection) {
		return prompt_choice_deliver("", out, outsize);
	}
	exact = st->explicit_selection
	    ? -1
	    : prompt_choice_exact(st, matches, names);
	if (exact >= 0) {
		return prompt_choice_deliver(names[exact], out, outsize);
	}
	if (matches > 0) {
		return prompt_choice_deliver(names[st->sel], out, outsize);
	}
	if (require_match) {
		return -2;
	}
	return prompt_choice_deliver(st->query, out, outsize);
}

/* Tab: complete the typed text to the highlighted candidate.  kg's other
 * pickers extend to the longest common prefix of the prefix-matched
 * group; here the whole candidate is available and already highlighted,
 * so one keystroke finishes the job rather than half of it. */
static void prompt_choice_complete(
    struct prompt_choice_state *st, int matches, const char **names)
{
	int len;

	if (matches <= 0) {
		return;
	}
	len = (int)strlen(names[st->sel]);
	if (len < PROMPT_CHOICE_QUERY_MAX) {
		memcpy(st->query, names[st->sel], (size_t)len + 1);
		st->qlen = len;
		st->overflow = 0;
	}
	st->sel = 0;
	st->explicit_selection = false;
}

static void prompt_choice_erase(struct prompt_choice_state *st)
{
	/* Retire a refused keystroke before deleting a real one, as
	 * minibuf_delete_backward() does: the count means "your answer did
	 * not fit", and backing up over what did not fit has to clear it. */
	if (st->overflow > 0) {
		st->overflow--;
	} else if (st->qlen > 0) {
		st->qlen
		    = utf8_glyph_start_before(st->query, st->qlen, st->qlen);
		st->query[st->qlen] = '\0';
	}
	st->sel = 0;
	st->explicit_selection = false;
}

static void prompt_choice_insert(
    struct prompt_choice_state *st, const char *bytes, int n)
{
	if (st->qlen + n < PROMPT_CHOICE_QUERY_MAX) {
		memcpy(st->query + st->qlen, bytes, (size_t)n);
		st->qlen += n;
		st->query[st->qlen] = '\0';
		st->sel = 0;
		st->explicit_selection = false;
	} else {
		st->overflow++;
	}
}

/* A printable key, or the lead byte of a character the terminal is
 * sending one byte at a time.  Anything else is ignored. */
static void prompt_choice_type(
    int fd, struct key_event c, struct prompt_choice_state *st)
{
	char seq[4];
	int n;

	if (c.mods != 0) {
		return;
	}
	if (ascii_is_print(c.base)) {
		seq[0] = (char)c.base;
		prompt_choice_insert(st, seq, 1);
		return;
	}
	if (c.base >= 0x80 && c.base <= 0xFF) {
		n = editor_read_utf8_seq(fd, (int)c.base, seq);
		if (n > 0) {
			prompt_choice_insert(st, seq, n);
		}
	}
}

/* One keystroke: MINIBUF_* to finish, -2 to keep reading. */
static int prompt_choice_key(int fd, struct key_event c,
    struct prompt_choice_state *st, int matches, const char **names,
    bool require_match, char *out, int outsize)
{
	if (KEY_IN_LIST(erase_keys, c)) {
		prompt_choice_erase(st);
		return -2;
	}
	if (KEY_IN_LIST(cancel_keys, c)) {
		return MINIBUF_CANCELLED;
	}
	if (KEY_IS(c, KEY_BASE_RET, 0)) {
		return prompt_choice_accept(
		    st, matches, names, require_match, out, outsize);
	}
	if (KEY_IS(c, KEY_BASE_TAB, 0)) {
		prompt_choice_complete(st, matches, names);
		return -2;
	}
	if (KEY_IN_LIST(picker_next_keys, c)) {
		editor_picker_cycle(&st->sel, matches, 1);
		st->explicit_selection |= matches > 0;
		return -2;
	}
	if (KEY_IN_LIST(picker_prev_keys, c)) {
		editor_picker_cycle(&st->sel, matches, -1);
		st->explicit_selection |= matches > 0;
		return -2;
	}
	prompt_choice_type(fd, c, st);
	return -2;
}

enum minibuf_result prompt_read_choice(int fd, const char *prompt,
    const char *const *choices, int nchoices, const char *initial,
    bool require_match, char *out, int outsize)
{
	struct prompt_choices candidates = { choices, nchoices };
	struct prompt_choice_state st = { { 0 }, 0, 0, 0, false };
	const char *names[PROMPT_CHOICE_MAX] = { 0 };

	if (out == NULL || outsize < 2 || nchoices > PROMPT_CHOICE_MAX) {
		return MINIBUF_OVERFLOW;
	}
	if (initial != NULL && strlen(initial) < PROMPT_CHOICE_QUERY_MAX) {
		(void)snprintf(st.query, sizeof(st.query), "%s", initial);
		st.qlen = (int)strlen(st.query);
	}
	kg_event_prompt_enter();
	for (;;) {
		int matches
		    = prompt_choice_redraw(prompt, &st, &candidates, names);
		struct key_event c = editor_read_key(fd);
		int result = prompt_choice_key(
		    fd, c, &st, matches, names, require_match, out, outsize);

		if (result != -2) {
			editor.echo_cursor_col = 0;
			editor_set_status_message("");
			kg_event_prompt_leave();
			return (enum minibuf_result)result;
		}
	}
}

enum prompt_yn prompt_ask_yn(int fd, const char *question)
{
	struct key_event answer;
	enum prompt_yn result;

	editor_set_status_message("%s", question);
	/* The echo area is committed to the question for exactly one key,
	 * which is the window kg_event_drain_safe() must defer through. */
	kg_event_prompt_enter();
	editor_refresh_screen();
	answer = editor_read_key(fd);
	if (KEY_IN_LIST(cancel_keys, answer)) {
		result = PROMPT_YN_CANCELLED;
	} else if (KEY_IS(answer, 'y', 0) || KEY_IS(answer, 'Y', 0)) {
		result = PROMPT_YN_YES;
	} else {
		result = PROMPT_YN_NO;
	}
	kg_event_prompt_leave();
	return result;
}
