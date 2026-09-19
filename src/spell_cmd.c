/* The four spell-* commands behind src/spell.h's facade: the mode
 * toggle, the two navigations, and the correction prompt.  Navigation
 * and correction scan directly from point instead of reading the
 * decorations, so they work on rows the background pass has not reached
 * yet.
 *
 * This is the one spell file that prompts, edits and reads the dispatch
 * prefix, so a test binary linking it links the whole command layer
 * with it -- lsp_req.c/lsp_edit.c and dap_commands.c are split off for
 * exactly that reason, and like them this file exists only on its own
 * link (EXTRA_cmd's everything-but-main set).  Compiled in every
 * configuration: the WITH_ENCHANT=0 build answers "built without spell
 * support" and never reaches a dictionary.
 */

#include "cmd.h"
#include "def.h"
#include "edit.h"
#include "event.h"
#include "keyevent.h"
#include "paste.h"
#include "prompt.h"
#include "spell.h"
#include "spell_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* 1 when a check can run now, enabling the mode on the way (jinx'
 * correct-guard rule).  Anything else has already told the user why. */
static int spell_ensure(void)
{
	struct editor_buffer *b = bcur();

	if (!spell_supported()) {
		editor_set_status_message("kg was built without spell support");
		return 0;
	}
	if (!b->spell_mode) {
		b->spell_mode = 1;
	}
	if (!spell_available()) {
		b->spell_mode = 0;
		editor_set_status_message(
		    "no spelling dictionary for '%s'", spell_language());
		return 0;
	}
	return 1;
}

void spell_cmd_mode(int fd)
{
	struct editor_buffer *b = bcur();

	(void)fd;
	if (!spell_supported()) {
		editor_set_status_message("kg was built without spell support");
		return;
	}
	b->spell_mode = !b->spell_mode;
	if (b->spell_mode && !spell_available()) {
		b->spell_mode = 0;
		editor_set_status_message(
		    "no spelling dictionary for '%s'", spell_language());
		return;
	}
	if (b->spell_mode) {
		editor_set_status_message(
		    "Spell mode enabled (%s)", spell_language());
	} else {
		editor_set_status_message("Spell mode disabled");
	}
}

/* The first span of `row' at/after `from_col' (forward) or at/before it
 * (backward), or 0 when the row holds none there. */
static int spell_row_next(
    struct editor_buffer *b, int row, int from_col, struct spell_span *span)
{
	struct spell_span spans[SPELL_NAV_SPANS];
	int n, k;

	if (row < 0 || row >= b->numrows) {
		return 0;
	}
	n = spell_scan_row(&b->row[row], &b->display, spell_check_all(b),
	    spell_dict_ok, NULL, spans, SPELL_NAV_SPANS);
	for (k = 0; k < n; k++) {
		if (spans[k].end > from_col) {
			*span = spans[k];
			return 1;
		}
	}
	return 0;
}

static int spell_row_prev(
    struct editor_buffer *b, int row, int from_col, struct spell_span *span)
{
	struct spell_span spans[SPELL_NAV_SPANS];
	int n, k;

	if (row < 0 || row >= b->numrows) {
		return 0;
	}
	n = spell_scan_row(&b->row[row], &b->display, spell_check_all(b),
	    spell_dict_ok, NULL, spans, SPELL_NAV_SPANS);
	for (k = n - 1; k >= 0; k--) {
		if (spans[k].start < from_col) {
			*span = spans[k];
			return 1;
		}
	}
	return 0;
}

static int spell_find_next(
    int from_row, int from_col, int *row, struct spell_span *span)
{
	struct editor_buffer *b = bcur();
	int r, limit;

	if (from_row < 0) {
		from_row = 0;
		from_col = -1;
	}
	if (from_row < b->numrows
	    && spell_row_next(b, from_row, from_col, span)) {
		*row = from_row;
		return 1;
	}
	limit = from_row + SPELL_NAV_ROW_MAX;
	if (limit > b->numrows) {
		limit = b->numrows;
	}
	for (r = from_row + 1; r < limit; r++) {
		if (spell_row_next(b, r, -1, span)) {
			*row = r;
			return 1;
		}
	}
	return 0;
}

static int spell_find_prev(
    int from_row, int from_col, int *row, struct spell_span *span)
{
	struct editor_buffer *b = bcur();
	int r, floor;

	if (from_row >= b->numrows) {
		from_row = b->numrows - 1;
		from_col = INT_MAX;
	}
	if (from_row >= 0 && spell_row_prev(b, from_row, from_col, span)) {
		*row = from_row;
		return 1;
	}
	floor = from_row - SPELL_NAV_ROW_MAX;
	if (floor < 0) {
		floor = 0;
	}
	for (r = from_row - 1; r >= floor; r--) {
		if (spell_row_prev(b, r, INT_MAX, span)) {
			*row = r;
			return 1;
		}
	}
	return 0;
}

void spell_cmd_next(int fd)
{
	int row;
	struct spell_span span;

	(void)fd;
	if (!spell_ensure()) {
		return;
	}
	if (!spell_find_next(editor_current_filerow(), editor_current_filecol(),
		&row, &span)) {
		editor_set_status_message("No further misspellings");
		return;
	}
	editor_reveal_position_centered(row, span.end);
}

void spell_cmd_previous(int fd)
{
	int row;
	struct spell_span span;

	(void)fd;
	if (!spell_ensure()) {
		return;
	}
	if (!spell_find_prev(editor_current_filerow(), editor_current_filecol(),
		&row, &span)) {
		editor_set_status_message("No previous misspellings");
		return;
	}
	editor_reveal_position_centered(row, span.end);
}

/* Back up from mid-word to the word's start, so correcting with point
 * inside a misspelling takes the whole word rather than its tail. */
static int spell_word_start(const char *chars, int size, int col)
{
	if (col > size) {
		col = size;
	}
	while (col > 0 && spell_word_byte((unsigned char)chars[col - 1])) {
		col--;
	}
	return col;
}

static void spell_correct_accept(
    struct editor_buffer *b, int row, const struct spell_span *span)
{
	char word[SPELL_SCAN_WORD_MAX + 1];
	size_t len = (size_t)(span->end - span->start);

	memcpy(word, b->row[row].chars + span->start, len);
	word[len] = '\0';
	if (spell_session_accept(word, len) == 0) {
		editor_set_status_message(
		    "Accepted '%s' for this session", word);
	} else {
		editor_set_status_message("Cannot accept '%s'", word);
	}
	spell_drop(b);
}

static void spell_correct_replace(struct editor_buffer *b, int row,
    const struct spell_span *span, const char *text)
{
	size_t begin = buffer_row_col_to_position(b, row, span->start);
	size_t end = buffer_row_col_to_position(b, row, span->end);
	struct kg_edit e = kg_edit_user(b, begin, end, text, strlen(text));

	if (kg_buffer_replace(&e, NULL)) {
		editor_set_status_message("Corrected to '%s'", text);
	} else {
		editor_set_status_message("Cannot correct here");
	}
}

static const struct key_event spell_erase_keys[]
    = { { KEY_BASE_DELETE, 0 }, { 'h', KEY_MOD_CTRL }, { KEY_BASE_DEL, 0 } };
static const struct key_event spell_cancel_keys[]
    = { { KEY_BASE_ESC, 0 }, { 'g', KEY_MOD_CTRL } };
static const struct key_event spell_picker_next_keys[] = { { KEY_BASE_DOWN, 0 },
	{ 'n', KEY_MOD_CTRL }, { KEY_BASE_RIGHT, 0 }, { 'f', KEY_MOD_CTRL } };
static const struct key_event spell_picker_prev_keys[] = { { KEY_BASE_UP, 0 },
	{ 'p', KEY_MOD_CTRL }, { KEY_BASE_LEFT, 0 }, { 'b', KEY_MOD_CTRL } };

static int spell_filter_choices(const char *const *choices, int nchoices,
    const char *query, int *match_indices, int max_matches)
{
	int matches = 0;

	for (int rank = 0; rank <= 1; rank++) {
		for (int i = 0; i < nchoices; i++) {
			if (editor_picker_match_rank(choices[i], query)
			    != rank) {
				continue;
			}
			if (matches < max_matches) {
				match_indices[matches] = i;
			}
			matches++;
		}
		if (rank == 0 && query[0] == '\0') {
			break;
		}
	}
	return matches;
}

static void spell_prompt_redraw(const char *prompt, const char *query, int qlen,
    const char *const *choices, const int *match_indices, int matches, int sel,
    char display_names[][128], const char *popup_ptrs[], int max_display)
{
	char msg[512];
	int off = 0;
	int shown = matches > PICKER_POPUP_MAX ? PICKER_POPUP_MAX : matches;
	int total = matches > max_display ? max_display : matches;

	for (int i = 0; i < total; i++) {
		snprintf(display_names[i], 128, "%d: %s", i,
		    choices[match_indices[i]]);
		popup_ptrs[i] = display_names[i];
	}
	if (total > 0) {
		editor_picker_popup_show(popup_ptrs, NULL, total, matches, sel);
	} else {
		editor_picker_popup_clear();
	}
	editor_msg_appendf(msg, sizeof(msg), &off, "%s%s", prompt, query);
	editor_picker_popup_suffix(msg, sizeof(msg), &off, matches, shown);
	editor_set_status_message("%s", msg);
	editor.echo_cursor_col = (int)strlen(prompt) + qlen + 1;
	editor_refresh_screen();
}

static int spell_deliver_choice(const char *text, char *out, int outsize)
{
	editor_picker_popup_clear();
	editor.echo_cursor_col = 0;
	editor_set_status_message("");
	kg_event_prompt_leave();
	if (!text || (int)strlen(text) >= outsize) {
		return MINIBUF_OVERFLOW;
	}
	strcpy(out, text);
	return MINIBUF_ACCEPTED;
}

static int spell_prompt_action_key(struct key_event c, char *query, int *qlen,
    int *sel, int matches, const char *const *choices, const int *match_indices)
{
	if (KEY_IN_LIST(spell_cancel_keys, c)) {
		editor_picker_popup_clear();
		editor.echo_cursor_col = 0;
		editor_set_status_message("");
		kg_event_prompt_leave();
		return MINIBUF_CANCELLED;
	}
	if (KEY_IN_LIST(spell_erase_keys, c)) {
		if (*qlen > 0) {
			*qlen = utf8_glyph_start_before(query, *qlen, *qlen);
			query[*qlen] = '\0';
			*sel = 0;
		}
		return -2;
	}
	if (KEY_IN_LIST(spell_picker_next_keys, c)) {
		if (matches > 0) {
			*sel = (*sel + 1) % matches;
		}
		return -2;
	}
	if (KEY_IN_LIST(spell_picker_prev_keys, c)) {
		if (matches > 0) {
			*sel = (*sel - 1 + matches) % matches;
		}
		return -2;
	}
	if (KEY_IS(c, KEY_BASE_TAB, 0)) {
		if (matches > 0 && *sel >= 0 && *sel < matches) {
			const char *chosen = choices[match_indices[*sel]];
			snprintf(query, PROMPT_CHOICE_QUERY_MAX, "%s", chosen);
			*qlen = (int)strlen(query);
			*sel = 0;
		}
		return -2;
	}
	return -1;
}

static int spell_prompt_insert_key(
    int fd, struct key_event c, char *query, int *qlen, int *sel)
{
	if (c.base == KEY_BASE_PASTE) {
		size_t plen = 0;
		const char *pdata = kg_bracketed_paste_data(&plen);
		if (pdata && plen > 0
		    && *qlen + (int)plen < PROMPT_CHOICE_QUERY_MAX) {
			memcpy(query + *qlen, pdata, plen);
			*qlen += (int)plen;
			query[*qlen] = '\0';
			*sel = 0;
		}
		kg_bracketed_paste_clear();
		return -2;
	}
	if (c.mods == 0 && ascii_is_print(c.base)) {
		if (*qlen + 1 < PROMPT_CHOICE_QUERY_MAX) {
			query[(*qlen)++] = (char)c.base;
			query[*qlen] = '\0';
			*sel = 0;
		}
		return -2;
	}
	if (c.mods == 0 && c.base >= 0x80 && c.base <= 0xFF) {
		char seq[4];
		int n = editor_read_utf8_seq(fd, (int)c.base, seq);
		if (n > 0 && *qlen + n < PROMPT_CHOICE_QUERY_MAX) {
			memcpy(query + *qlen, seq, (size_t)n);
			*qlen += n;
			query[*qlen] = '\0';
			*sel = 0;
		}
	}
	return -2;
}

static int spell_prompt_handle_key(int fd, struct key_event c, char *query,
    int *qlen, int *sel, int matches, const char *const *choices,
    const int *match_indices, const char *fallback, char *out, int outsize)
{
	int res = spell_prompt_action_key(
	    c, query, qlen, sel, matches, choices, match_indices);
	if (res != -1) {
		return res;
	}
	if (c.mods == 0 && c.base >= '0' && c.base <= '9' && *qlen == 0) {
		int digit = (int)(c.base - '0');
		if (digit < matches) {
			return spell_deliver_choice(
			    choices[match_indices[digit]], out, outsize);
		}
	}
	if (KEY_IS(c, KEY_BASE_RET, 0)) {
		const char *chosen
		    = (matches > 0 && *sel >= 0 && *sel < matches)
		    ? choices[match_indices[*sel]]
		    : (*qlen > 0 ? query : fallback);
		return spell_deliver_choice(chosen, out, outsize);
	}
	return spell_prompt_insert_key(fd, c, query, qlen, sel);
}

static enum minibuf_result spell_prompt_choice(int fd, const char *prompt,
    const char *const *choices, int nchoices, const char *fallback, char *out,
    int outsize)
{
	char query[PROMPT_CHOICE_QUERY_MAX] = { 0 };
	int qlen = 0;
	int sel = 0;
	int match_indices[SPELL_SUGGEST_MAX];
	char display_names[SPELL_SUGGEST_MAX][128];
	const char *popup_ptrs[SPELL_SUGGEST_MAX];

	if (out == NULL || outsize < 2) {
		return MINIBUF_OVERFLOW;
	}
	kg_event_prompt_enter();

	for (;;) {
		int matches = spell_filter_choices(
		    choices, nchoices, query, match_indices, SPELL_SUGGEST_MAX);
		struct key_event c;
		int res;

		if (sel >= matches) {
			sel = matches > 0 ? matches - 1 : 0;
		}
		spell_prompt_redraw(prompt, query, qlen, choices, match_indices,
		    matches, sel, display_names, popup_ptrs, SPELL_SUGGEST_MAX);

		c = editor_read_key(fd);
		res = spell_prompt_handle_key(fd, c, query, &qlen, &sel,
		    matches, choices, match_indices, fallback, out, outsize);
		if (res != -2) {
			return (enum minibuf_result)res;
		}
	}
}

void spell_cmd_correct(int fd)
{
	struct editor_buffer *b = bcur();
	const struct command_prefix *prefix = cmd_active_prefix();
	int filerow, filecol, row;
	struct spell_span span;
	char word[SPELL_SCAN_WORD_MAX + 1];
	char prompt[SPELL_SCAN_WORD_MAX + 32];
	char answer[PROMPT_CHOICE_QUERY_MAX];
	const char *choices[SPELL_SUGGEST_MAX];
	char **raw = NULL;
	size_t len, nsugg = 0, i;

	if (!spell_ensure()) {
		return;
	}
	filerow = editor_current_filerow();
	filecol = editor_current_filecol();
	if (filerow < b->numrows) {
		filecol = spell_word_start(
		    b->row[filerow].chars, b->row[filerow].size, filecol);
	}
	if (!spell_find_next(filerow, filecol, &row, &span)) {
		editor_set_status_message("No further misspellings");
		return;
	}
	if (prefix && prefix->supplied) {
		spell_correct_accept(b, row, &span);
		return;
	}
	len = (size_t)(span.end - span.start);
	memcpy(word, b->row[row].chars + span.start, len);
	word[len] = '\0';
	editor_reveal_position_centered(row, span.end);
	nsugg = spell_suggest(word, len, &raw);
	for (i = 0; i < nsugg; i++) {
		choices[i] = raw[i];
	}
	snprintf(prompt, sizeof(prompt), "Correct '%s': ", word);
	if (spell_prompt_choice(
		fd, prompt, choices, (int)nsugg, word, answer, sizeof(answer))
	    != MINIBUF_ACCEPTED) {
		spell_free_suggestions(raw, nsugg);
		return;
	}
	spell_free_suggestions(raw, nsugg);
	if (strcmp(answer, word) == 0) {
		return;
	}
	spell_correct_replace(b, row, &span, answer);
}
