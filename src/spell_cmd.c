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
	if (prompt_read_choice(fd, prompt, choices, (int)nsugg, word, false,
		answer, sizeof(answer))
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
