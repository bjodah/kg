/* The debugger's panes, layout and row metadata.  src/dap_ui.h has the
 * contract; this is the projection.
 *
 * Read the header first: the two content disciplines, the one notification
 * point and the metadata transaction are stated there, and nothing below
 * re-argues them.
 */

#include "dap_ui.h"

#include "bufhandle.h"
#include "dap_breakpoint.h"
#include "dap_exec.h"
#include "dap_session.h"
#include "def.h"
#include "event.h"
#include "visit.h"
#include "winmgr.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DAP_UI_WHO "dap"

/* One pane, and everything kg believes about it.
 *
 * `generation_begun` and `generation_done` are the transaction: the first
 * moves when a swap starts, the second when it finished, and a row is only
 * ever acted on while they agree.  A torn swap -- text replaced, metadata
 * not -- is therefore a pane that answers "this line is nothing" rather
 * than one that answers with somebody else's line.
 */
struct dap_ui_pane_state {
	struct kg_buffer_handle buffer;
	unsigned generation_begun;
	unsigned generation_done;
	size_t row_count;
	struct dap_ui_row rows[DAP_UI_ROWS_MAX];
	/* Transcript panes only. */
	size_t bytes;
	bool truncated;
	bool pending_cr; /* the last accepted byte was a CR [M-12] */
	bool at_line_start;
};

static const struct {
	const char *name;
	bool transcript;
} pane_table[DAP_UI_PANE_COUNT] = {
	[DAP_UI_PANE_REPL] = { DAP_UI_REPL_BUFFER_NAME, true },
	[DAP_UI_PANE_LOCALS] = { DAP_UI_LOCALS_BUFFER_NAME, false },
	[DAP_UI_PANE_OUTPUT] = { DAP_UI_OUTPUT_BUFFER_NAME, true },
	[DAP_UI_PANE_STACK] = { DAP_UI_STACK_BUFFER_NAME, false },
	[DAP_UI_PANE_BREAKPOINTS] = { DAP_UI_BREAKPOINTS_BUFFER_NAME, false },
	[DAP_UI_PANE_THREADS] = { DAP_UI_THREADS_BUFFER_NAME, false },
};

static struct dap_ui_pane_state panes[DAP_UI_PANE_COUNT];

/* Which of the two panes that share the layout's sixth slot is showing.
 * GDB shows one and reaches the other with a command, and so does kg:
 * dap-info-toggle-breakpoints-threads. */
static bool show_threads;

/* The repaint's own state: one flag, one deadline. */
static bool dirty;
static unsigned long long due_ms;

/* An expanded variable node, remembered by NAME-PATH so that it survives
 * the re-fetch every step performs (dape.el:4616-4627).  The path is not
 * the identity a request is issued under -- that is the model index under
 * one suspension epoch, which is exact -- it is what carries a user's
 * "keep this open" across a stop boundary, where nothing else can.
 *
 * `asked_epoch` is the loop guard: a node whose children are not in the
 * model is re-requested at most once per suspension, so an adapter that
 * answers with nothing cannot make the repaint ask forever. */
static struct {
	char key[DAP_UI_KEY_MAX];
	unsigned asked_epoch;
	bool used;
} expanded[DAP_UI_EXPANDED_MAX];

static struct {
	struct kg_window_configuration saved;
	bool active;
	bool wanted; /* the per-SESSION preference; the snapshot is not it */
	bool restore_pending; /* a restore a prompt is standing on */
} layout;

/* ------------------------------- plumbing ----------------------------- */

static unsigned long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ull
	    + (unsigned long long)(ts.tv_nsec / 1000000);
}

void dap_ui_changed(void)
{
	if (!dirty) {
		due_ms = now_ms() + DAP_UI_DEBOUNCE_MS;
	}
	dirty = true;
}

/* The pane's buffer, created if this is the first time anybody asked.
 * find-or-create and never re-prepare: buf_prepare_special_text() clears
 * what it finds, and clearing a transcript because a pane was displayed
 * again would be a debugger that forgets what the program printed. */
static int pane_slot(enum dap_ui_pane pane)
{
	struct dap_ui_pane_state *p = &panes[pane];
	int slot = buf_find_open(pane_table[pane].name);

	if (slot < 0) {
		slot = buf_prepare_special_text(
		    pane_table[pane].name, &text_syntax, 1);
	}
	/* Only ever on success: a buffer table that was momentarily full is
	 * not news about the pane kg already has, and forgetting its handle
	 * here would make the next append think the user had killed it. */
	if (slot >= 0) {
		p->buffer = buf_handle(slot);
	}
	return slot;
}

/* Whether a window is showing this pane right now.  A pane nobody can see
 * is not repainted: the tables are the truth and the pane is rebuilt from
 * them the moment it is displayed. */
static bool pane_displayed(enum dap_ui_pane pane)
{
	int slot = buf_find_open(pane_table[pane].name);
	int i;

	if (slot < 0) {
		return false;
	}
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (winlist[i].active && winlist[i].buf.slot == slot
		    && buf_resolve(winlist[i].buf)) {
			return true;
		}
	}
	return false;
}

/* Which pane the current buffer is, or DAP_UI_PANE_COUNT. */
static enum dap_ui_pane current_pane(void)
{
	const char *name = bcur()->filename;
	size_t i;

	if (!name) {
		return DAP_UI_PANE_COUNT;
	}
	for (i = 0; i < DAP_UI_PANE_COUNT; i++) {
		if (strcmp(name, pane_table[i].name) == 0
		    && panes[i].buffer.id != 0
		    && buf_handle_slot(panes[i].buffer) == buf_current) {
			return (enum dap_ui_pane)i;
		}
	}
	return DAP_UI_PANE_COUNT;
}

bool dap_ui_current_buffer_is_pane(void)
{
	const char *name = bcur()->filename;
	size_t i;

	if (!name || bcur()->no_file) {
		return false;
	}
	for (i = 0; i < DAP_UI_PANE_COUNT; i++) {
		if (strcmp(name, pane_table[i].name) == 0) {
			return true;
		}
	}
	return false;
}

/* --------------------------- the render buffer ------------------------ */

/* One pane's next contents, text and metadata together, built before
 * anything is replaced.  Static rather than automatic because it is 12 KiB
 * and there is never more than one render in flight: a renderer is called
 * from the repaint and calls nothing that could re-enter it. */
static struct {
	char *text;
	size_t len;
	size_t cap;
	bool failed;
	struct dap_ui_row rows[DAP_UI_ROWS_MAX];
	size_t row_count;
	size_t omitted;
} rb;

static void rb_begin(void)
{
	rb.len = 0;
	rb.failed = false;
	rb.row_count = 0;
	rb.omitted = 0;
}

static bool rb_reserve(size_t extra)
{
	size_t want = rb.len + extra + 1;
	char *grown;

	if (rb.failed) {
		return false;
	}
	if (want <= rb.cap) {
		return true;
	}
	while (rb.cap < want) {
		rb.cap = rb.cap ? rb.cap * 2 : 4096;
	}
	grown = realloc(rb.text, rb.cap);
	if (!grown) {
		rb.failed = true;
		return false;
	}
	rb.text = grown;
	return true;
}

/* Append one LINE and the metadata that describes it.  Every pane line goes
 * through here, which is what keeps the text and the vector in lockstep:
 * one call is one '\n' and one row.  Past the row bound nothing is appended
 * at all, and the caller's "N omitted" line is what a reader sees instead. */
static void rb_line(const struct dap_ui_row *meta, const char *text)
{
	size_t len = strlen(text);

	if (rb.row_count >= DAP_UI_ROWS_MAX) {
		rb.omitted++;
		return;
	}
	if (!rb_reserve(len + 1)) {
		return;
	}
	memcpy(rb.text + rb.len, text, len);
	rb.len += len;
	rb.text[rb.len++] = '\n';
	rb.rows[rb.row_count++] = *meta;
}

static void rb_text_line(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* A line that is not anything: a heading, a notice, a truncation marker. */
static void rb_text_line(const char *fmt, ...)
{
	struct dap_ui_row meta = { .kind = DAP_UI_ROW_NONE, .parent = -1 };
	char line[DAP_UI_VALUE_COLS + DAP_UI_NAME_COLS + 128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	rb_line(&meta, line);
}

/* Where point was in every window showing this pane, and putting it back.
 *
 * A model-derived pane is REPLACED on every repaint, and the append path
 * follows the tail of a growing buffer -- which is right for a transcript
 * and wrong here: a user reading the third frame of the stack when a
 * `thread` event lands would find point on the last line of the pane, and
 * the next RET would act on whatever that turned out to be.  So the view is
 * saved across the swap and clamped to what the new text has. */
struct pane_view {
	bool saved;
	int rowoff;
	int cy;
	int cx;
};

static void views_save(int slot, struct pane_view views[MAX_WINDOWS])
{
	int i;

	for (i = 0; i < MAX_WINDOWS; i++) {
		views[i].saved = slot >= 0 && winlist[i].active
		    && winlist[i].buf.slot == slot;
		if (views[i].saved) {
			views[i].rowoff = winlist[i].rowoff;
			views[i].cy = winlist[i].cy;
			views[i].cx = winlist[i].cx;
		}
	}
}

static void views_restore(
    int slot, const struct pane_view views[MAX_WINDOWS], int numrows)
{
	int i;

	for (i = 0; i < MAX_WINDOWS; i++) {
		struct editor_window *w = &winlist[i];
		int row;

		if (!views[i].saved || !w->active || w->buf.slot != slot) {
			continue;
		}
		row = views[i].rowoff + views[i].cy;
		if (row >= numrows) {
			row = numrows > 0 ? numrows - 1 : 0;
		}
		w->rowoff = views[i].rowoff <= row ? views[i].rowoff : row;
		w->cy = row - w->rowoff;
		w->cx = views[i].cx;
	}
}

/* Swap `pane` to what the render buffer holds, in that order: the text
 * first, the metadata only once the text is really there.  A pane whose
 * build failed is left exactly as it was -- that is the whole reason the
 * build happens into rb rather than into the buffer. */
static bool rb_commit(enum dap_ui_pane pane)
{
	struct dap_ui_pane_state *p = &panes[pane];
	struct pane_view views[MAX_WINDOWS];
	int slot;

	if (rb.failed) {
		return false;
	}
	views_save(buf_find_open(pane_table[pane].name), views);
	slot = buf_prepare_special_text(pane_table[pane].name, &text_syntax, 1);
	if (slot < 0) {
		return false;
	}
	p->buffer = buf_handle(slot);
	p->generation_begun++;
	p->row_count = 0;
	if (rb.len > 0 && buf_append_special_text(slot, rb.text, rb.len) != 0) {
		return false;
	}
	memcpy(p->rows, rb.rows, rb.row_count * sizeof(*rb.rows));
	p->row_count = rb.row_count;
	p->generation_done = p->generation_begun;
	views_restore(slot, views, buflist[slot].numrows);
	return true;
}

/* ---------------------------- capped fields --------------------------- */

/* Append `text` to `line`, at most `cols` bytes of it, with an ellipsis
 * where it cut and with every C0 byte turned into a space.
 *
 * The C0 replacement is STRUCTURAL, not a safety measure: a pane line is
 * one buffer row and one metadata row, and a newline inside an adapter's
 * variable value would make it two.  What makes an adapter's bytes safe to
 * PAINT is display_glyph_at() at the other end, which is where they still
 * go from here.  The cut respects UTF-8 continuation bytes so a truncated
 * name is short rather than mojibake. */
static void append_capped(
    char *line, size_t size, const char *text, size_t cols)
{
	size_t len = strlen(line), i, take;

	if (!text || len >= size - 1) {
		return;
	}
	take = strlen(text);
	if (take > cols) {
		take = cols;
		while (take > 0 && ((unsigned char)text[take] & 0xc0) == 0x80) {
			take--;
		}
	}
	if (take > size - 1 - len) {
		take = size - 1 - len;
	}
	for (i = 0; i < take; i++) {
		unsigned char c = (unsigned char)text[i];

		line[len + i] = (c < 0x20 || c == 0x7f) ? ' ' : text[i];
	}
	line[len + take] = '\0';
	if (strlen(text) > cols) {
		snprintf(line + strlen(line), size - strlen(line), "…");
	}
}

static void append_plain(char *line, size_t size, const char *text)
{
	snprintf(line + strlen(line), size - strlen(line), "%s", text);
}

/* ------------------------------ transcripts --------------------------- */

/* The transcript's buffer, and the notice a user needs when it is not the
 * one they were reading.  Killing `*dap-output*` is allowed -- it is an
 * ordinary buffer with an ordinary `q` -- and the model keeps no second
 * copy, so what comes back is a fresh transcript that says so rather than
 * a silent gap. */
static int transcript_slot(enum dap_ui_pane pane)
{
	struct dap_ui_pane_state *p = &panes[pane];
	bool killed = p->buffer.id != 0 && !buf_resolve(p->buffer);
	int slot;

	if (killed) {
		*p = (struct dap_ui_pane_state) { 0 };
		p->at_line_start = true;
	}
	slot = pane_slot(pane);
	if (slot >= 0 && killed) {
		static const char notice[]
		    = "[transcript restarted; what came before went with the "
		      "buffer]\n";

		(void)buf_append_special_text(slot, notice, sizeof(notice) - 1);
	}
	return slot;
}

/* Charge and append one run of bytes.  Past the cap the marker is printed
 * once and everything after it is dropped -- but only AFTER the client has
 * already read it off the pipe, which is the half that matters: an adapter
 * that blocks writing is a debugger that hangs. */
static void transcript_append(
    enum dap_ui_pane pane, int slot, const char *text, size_t len)
{
	struct dap_ui_pane_state *p = &panes[pane];

	if (len == 0) {
		return;
	}
	if (p->bytes >= DAP_UI_TRANSCRIPT_MAX) {
		return;
	}
	if (len > DAP_UI_TRANSCRIPT_MAX - p->bytes) {
		len = DAP_UI_TRANSCRIPT_MAX - p->bytes;
	}
	p->bytes += len;
	(void)buf_append_special_text(slot, text, len);
	p->at_line_start = text[len - 1] == '\n';
	if (p->bytes >= DAP_UI_TRANSCRIPT_MAX && !p->truncated) {
		static const char marker[] = "\n[output truncated]\n";

		p->truncated = true;
		p->at_line_start = true;
		(void)buf_append_special_text(slot, marker, sizeof(marker) - 1);
	}
}

/* CR/CRLF normalisation as STREAMING STATE.  lldb-dap emits CRLF and an
 * event boundary is not a line boundary, so an event ending in CR followed
 * by one starting with LF is ONE newline -- which is only decidable with a
 * flag that outlives the call.  A lone CR is a newline; a CR that turns out
 * to be half of a CRLF has already produced it, and the LF is dropped. */
static void transcript_write(
    enum dap_ui_pane pane, int slot, const char *text, size_t len)
{
	struct dap_ui_pane_state *p = &panes[pane];
	char out[512];
	size_t n = 0, i;

	for (i = 0; i < len; i++) {
		char c = text[i];

		if (p->pending_cr) {
			p->pending_cr = false;
			if (c == '\n') {
				continue;
			}
		}
		if (c == '\r') {
			p->pending_cr = true;
			c = '\n';
		}
		out[n++] = c;
		if (n == sizeof(out)) {
			transcript_append(pane, slot, out, n);
			n = 0;
		}
	}
	transcript_append(pane, slot, out, n);
}

void dap_ui_output(const char *category, const char *text, size_t len)
{
	bool console = category && strcmp(category, "console") == 0;
	bool err = category && strcmp(category, "stderr") == 0;
	enum dap_ui_pane pane = console ? DAP_UI_PANE_REPL : DAP_UI_PANE_OUTPUT;
	int slot = transcript_slot(pane);

	if (slot < 0 || !text) {
		return;
	}
	/* stderr is marked, and marked with TEXT: kg does not insert ANSI
	 * into a buffer nobody asked to be a terminal, and a special buffer
	 * carries no per-line face.  The mark goes in only at the start of a
	 * line, so a chunk that continues somebody else's line stays in
	 * arrival order rather than being cut in half by a prefix. */
	if (err && panes[pane].at_line_start) {
		transcript_append(pane, slot, "! ", 2);
	}
	transcript_write(pane, slot, text, len);
	dap_ui_changed();
}

void dap_ui_repl_echo(const char *expression)
{
	int slot = transcript_slot(DAP_UI_PANE_REPL);
	char line[DAP_EXEC_TEXT_MAX + 8];

	if (slot < 0) {
		return;
	}
	line[0] = '\0';
	append_plain(line, sizeof(line), "DAP> ");
	append_capped(line, sizeof(line), expression, DAP_UI_VALUE_COLS);
	append_plain(line, sizeof(line), "\n");
	transcript_write(DAP_UI_PANE_REPL, slot, line, strlen(line));
	dap_ui_changed();
}

/* An answer, prefixed with the expression it answers.  Replies finish out
 * of order -- two evaluates in flight, the second one faster -- so a bare
 * value in a transcript would be a value nobody can attribute. */
void dap_ui_repl_answer(const char *expression, bool error, const char *text)
{
	int slot = transcript_slot(DAP_UI_PANE_REPL);
	char line[DAP_EXEC_TEXT_MAX * 2 + 16];

	if (slot < 0) {
		return;
	}
	line[0] = '\0';
	append_capped(line, sizeof(line), expression, DAP_UI_NAME_COLS);
	append_plain(line, sizeof(line), error ? " => error: " : " => ");
	append_capped(line, sizeof(line), text, DAP_UI_VALUE_COLS);
	append_plain(line, sizeof(line), "\n");
	transcript_write(DAP_UI_PANE_REPL, slot, line, strlen(line));
	dap_ui_changed();
}

/* ------------------------------- the stack ---------------------------- */

/* dape's arrow discipline: the selected frame is marked, and the mark is
 * hollow when it is not the top of the stack -- "you are looking at this
 * one" and "this one is where the program is" are different facts.
 *
 * A frame kg cannot open is listed and said so.  kg has no dim face for a
 * special buffer's line, so "dimmed and non-navigable" is spelled out in
 * words and enforced by the metadata: RET refuses it. */
static void render_stack(void)
{
	size_t count = dap_exec_frame_count();
	size_t selected = dap_exec_selected_frame();
	size_t i;

	rb_text_line("Stack — %zu frame%s%s", count, count == 1 ? "" : "s",
	    count == 0 ? " (nothing is stopped)" : "");
	for (i = 0; i < count; i++) {
		struct dap_exec_frame f;
		struct dap_ui_row meta = { .kind = DAP_UI_ROW_FRAME,
			.suspension_epoch = dap_exec_epoch(),
			.parent = -1 };
		char line[DAP_UI_NAME_COLS + PATH_MAX + 64];
		char where[64];

		if (!dap_exec_frame(i, &f)) {
			continue;
		}
		meta.stable_id = f.id;
		meta.navigable = f.navigable;
		line[0] = '\0';
		append_plain(line, sizeof(line),
		    i != selected ? "  " : (selected == 0 ? "> " : "- "));
		snprintf(where, sizeof(where), "#%zu ", i);
		append_plain(line, sizeof(line), where);
		append_capped(line, sizeof(line), f.name, DAP_UI_NAME_COLS);
		if (f.navigable) {
			snprintf(where, sizeof(where), "  %d", f.line);
			append_capped(line, sizeof(line), "  ", 2);
			append_capped(
			    line, sizeof(line), f.path, DAP_UI_VALUE_COLS);
			append_plain(line, sizeof(line), where);
		} else {
			append_plain(line, sizeof(line), "  (no source)");
		}
		rb_line(&meta, line);
	}
	if (rb.omitted > 0) {
		rb_text_line("… %zu frame%s omitted", rb.omitted,
		    rb.omitted == 1 ? "" : "s");
	}
}

/* ------------------------------- the locals --------------------------- */

/* A variable node's NAME-PATH: the scope it lives in, then one component
 * per ancestor spelling that ancestor's ordinal among its own siblings and
 * its name.  The ordinal is in there because duplicate names are real --
 * two scopes may both have `self`, and an adapter may report the same name
 * twice in one scope -- and the name is in there because the ordinal alone
 * would move the moment the adapter reports one variable more.
 *
 * This is what survives a re-fetch, and it is best-effort by construction.
 * The identity a REQUEST is issued under is not this: it is the model index
 * under one suspension epoch, which is exact. */
static size_t sibling_ordinal(size_t index, const struct dap_exec_variable *v)
{
	size_t ordinal = 0, j;

	for (j = 0; j < index; j++) {
		struct dap_exec_variable other;

		if (dap_exec_variable(j, &other) && other.parent == v->parent
		    && other.scope == v->scope) {
			ordinal++;
		}
	}
	return ordinal;
}

static bool variable_key(size_t index, char *out, size_t size)
{
	char parts[DAP_EXEC_MAX_DEPTH][64];
	struct dap_exec_variable v;
	struct dap_exec_scope scope;
	size_t depth = 0, i;

	while (dap_exec_variable(index, &v) && depth < DAP_EXEC_MAX_DEPTH) {
		snprintf(parts[depth], sizeof(parts[depth]), "%zu:%.40s",
		    sibling_ordinal(index, &v), v.name);
		depth++;
		if (v.parent < 0) {
			break;
		}
		index = (size_t)v.parent;
	}
	if (depth == 0) {
		return false;
	}
	out[0] = '\0';
	if (v.scope >= 0 && dap_exec_scope((size_t)v.scope, &scope)) {
		snprintf(out, size, "%.40s", scope.name);
	}
	for (i = depth; i > 0; i--) {
		size_t len = strlen(out);

		snprintf(out + len, size - len, "/%s", parts[i - 1]);
	}
	return true;
}

static int expanded_find(const char *key)
{
	int i;

	for (i = 0; i < DAP_UI_EXPANDED_MAX; i++) {
		if (expanded[i].used && strcmp(expanded[i].key, key) == 0) {
			return i;
		}
	}
	return -1;
}

/* Remember one.  A full table refuses rather than evicting: an expansion
 * silently dropped is a pane that closes a node the user just opened. */
static bool expanded_add(const char *key)
{
	int i;

	if (expanded_find(key) >= 0) {
		return true;
	}
	for (i = 0; i < DAP_UI_EXPANDED_MAX; i++) {
		if (!expanded[i].used) {
			expanded[i].used = true;
			expanded[i].asked_epoch = 0;
			snprintf(expanded[i].key, sizeof(expanded[i].key), "%s",
			    key);
			return true;
		}
	}
	return false;
}

static bool variable_is_expanded(size_t index)
{
	char key[DAP_UI_KEY_MAX];

	return variable_key(index, key, sizeof(key)) && expanded_find(key) >= 0;
}

/* Whether this node's whole ancestry is expanded, which is the only reason
 * a child is on screen at all.  It is also the "still-expanded parent" half
 * of the late-reply rule: children that arrive for a node the user has
 * since collapsed are in the model and simply never rendered. */
static bool variable_visible(const struct dap_exec_variable *v)
{
	struct dap_exec_variable node = *v;
	size_t guard = 0;

	while (node.parent >= 0 && guard++ < DAP_EXEC_MAX_DEPTH) {
		if (!variable_is_expanded((size_t)node.parent)
		    || !dap_exec_variable((size_t)node.parent, &node)) {
			return false;
		}
	}
	return true;
}

/* Re-ask for the children of a node the user left open, once per
 * suspension.  This is what makes an expansion survive a step: the model
 * drops every variable on the stop, the key does not, and the first repaint
 * after the re-fetch asks again. */
static void expansion_refetch(size_t index, const struct dap_exec_variable *v)
{
	char key[DAP_UI_KEY_MAX];
	int slot;

	if (v->variables_reference <= 0
	    || !variable_key(index, key, sizeof(key))) {
		return;
	}
	slot = expanded_find(key);
	if (slot < 0 || expanded[slot].asked_epoch == dap_exec_epoch()) {
		return;
	}
	expanded[slot].asked_epoch = dap_exec_epoch();
	(void)dap_exec_expand_variable(index);
}

static void render_variable(size_t index, const struct dap_exec_variable *v)
{
	struct dap_ui_row meta = { .kind = DAP_UI_ROW_VARIABLE,
		.suspension_epoch = dap_exec_epoch(),
		.stable_id = (int)index,
		.parent = v->parent,
		.depth = v->depth,
		.navigable = v->variables_reference > 0 };
	char line[DAP_UI_NAME_COLS + DAP_UI_VALUE_COLS + DAP_UI_TYPE_COLS + 64];
	int indent = v->depth < 8 ? v->depth : 8;

	line[0] = '\0';
	snprintf(line, sizeof(line), "  %*s", indent * 2, "");
	append_plain(line, sizeof(line),
	    v->variables_reference > 0
		? (variable_is_expanded(index) ? "- " : "+ ")
		: "  ");
	append_capped(line, sizeof(line), v->name, DAP_UI_NAME_COLS);
	append_plain(line, sizeof(line), " = ");
	append_capped(line, sizeof(line), v->value, DAP_UI_VALUE_COLS);
	if (v->type[0]) {
		append_plain(line, sizeof(line), " : ");
		append_capped(line, sizeof(line), v->type, DAP_UI_TYPE_COLS);
	}
	rb_line(&meta, line);
}

/* Scopes and variables of the SELECTED frame and of no other: locals are
 * about where the user is looking, and a pane that mixed two frames' values
 * would be a pane nobody could read. */
static void render_locals(void)
{
	size_t scopes = dap_exec_scope_count();
	size_t count = dap_exec_variable_count();
	struct dap_exec_frame frame;
	size_t i, s;

	if (dap_exec_frame(dap_exec_selected_frame(), &frame)) {
		rb_text_line("Locals — frame #%zu %.*s",
		    dap_exec_selected_frame(), DAP_UI_NAME_COLS, frame.name);
	} else {
		rb_text_line("Locals — nothing is stopped");
	}
	for (s = 0; s < scopes; s++) {
		struct dap_exec_scope scope;
		struct dap_ui_row meta = { .kind = DAP_UI_ROW_SCOPE,
			.suspension_epoch = dap_exec_epoch(),
			.parent = -1 };
		char line[DAP_UI_NAME_COLS + 32];

		if (!dap_exec_scope(s, &scope)) {
			continue;
		}
		meta.stable_id = scope.variables_reference;
		line[0] = '\0';
		append_capped(line, sizeof(line), scope.name, DAP_UI_NAME_COLS);
		if (scope.expensive) {
			append_plain(line, sizeof(line), " (expensive)");
		}
		rb_line(&meta, line);
		for (i = 0; i < count; i++) {
			struct dap_exec_variable v;

			if (!dap_exec_variable(i, &v) || v.scope != (int)s
			    || !variable_visible(&v)) {
				continue;
			}
			render_variable(i, &v);
			expansion_refetch(i, &v);
		}
	}
	if (rb.omitted > 0) {
		rb_text_line("… %zu line%s omitted", rb.omitted,
		    rb.omitted == 1 ? "" : "s");
	}
}

/* ------------------------------- the threads -------------------------- */

static void render_threads(void)
{
	size_t count = dap_exec_thread_count();
	int selected = dap_exec_selected_thread();
	size_t i;

	rb_text_line("Threads — %zu", count);
	for (i = 0; i < count; i++) {
		struct dap_exec_thread t;
		struct dap_ui_row meta = { .kind = DAP_UI_ROW_THREAD,
			.suspension_epoch = dap_exec_epoch(),
			.parent = -1,
			.navigable = true };
		char line[DAP_UI_NAME_COLS + 64];
		char id[32];

		if (!dap_exec_thread(i, &t)) {
			continue;
		}
		meta.stable_id = t.id;
		line[0] = '\0';
		append_plain(
		    line, sizeof(line), t.id == selected ? "> " : "  ");
		snprintf(id, sizeof(id), "%d ", t.id);
		append_plain(line, sizeof(line), id);
		append_capped(line, sizeof(line), t.name, DAP_UI_NAME_COLS);
		append_plain(
		    line, sizeof(line), t.stopped ? "  stopped" : "  running");
		rb_line(&meta, line);
	}
	if (rb.omitted > 0) {
		rb_text_line("… %zu thread%s omitted", rb.omitted,
		    rb.omitted == 1 ? "" : "s");
	}
}

/* ----------------------------- the breakpoints ------------------------ */

static void render_breakpoint_line(
    size_t source, size_t index, const struct dap_breakpoint_info *info)
{
	struct dap_ui_row meta = { .kind = DAP_UI_ROW_BREAKPOINT,
		.stable_id = info->line,
		.parent = (int)source,
		.navigable = true };
	char line[PATH_MAX + 128];
	char where[64];

	(void)index;
	line[0] = '\0';
	append_plain(line, sizeof(line), "  ");
	append_capped(line, sizeof(line),
	    dap_breakpoint_source_display_path(source), DAP_UI_VALUE_COLS);
	snprintf(where, sizeof(where), ":%d ", info->line);
	append_plain(line, sizeof(line), where);
	append_plain(line, sizeof(line),
	    !info->enabled	 ? " disabled"
		: info->verified ? " verified"
				 : " pending");
	if (info->temporary) {
		append_plain(line, sizeof(line), " temporary");
	}
	if (info->message[0]) {
		append_plain(line, sizeof(line), " — ");
		append_capped(
		    line, sizeof(line), info->message, DAP_UI_NAME_COLS);
	}
	rb_line(&meta, line);
}

static void render_breakpoints(void)
{
	size_t sources = dap_breakpoint_source_count();
	size_t s, i;

	rb_text_line("Breakpoints — %zu", dap_breakpoint_count());
	for (s = 0; s < sources; s++) {
		size_t n = dap_breakpoint_source_length(s);

		for (i = 0; i < n; i++) {
			struct dap_breakpoint_info info;

			if (dap_breakpoint_get(s, i, &info) && info.line > 0) {
				render_breakpoint_line(s, i, &info);
			}
		}
	}
	if (rb.omitted > 0) {
		rb_text_line("… %zu breakpoint%s omitted", rb.omitted,
		    rb.omitted == 1 ? "" : "s");
	}
}

/* ------------------------------- the repaint -------------------------- */

static void repaint_pane(enum dap_ui_pane pane)
{
	rb_begin();
	switch (pane) {
	case DAP_UI_PANE_STACK:
		render_stack();
		break;
	case DAP_UI_PANE_LOCALS:
		render_locals();
		break;
	case DAP_UI_PANE_THREADS:
		render_threads();
		break;
	case DAP_UI_PANE_BREAKPOINTS:
		render_breakpoints();
		break;
	default:
		return;
	}
	(void)rb_commit(pane);
}

/* Every model-derived pane that is on screen.  The transcripts are not
 * here: they are appended to as their bytes arrive and a repaint has
 * nothing to add to them. */
static void repaint_all(void)
{
	static const enum dap_ui_pane derived[]
	    = { DAP_UI_PANE_STACK, DAP_UI_PANE_LOCALS, DAP_UI_PANE_THREADS,
		      DAP_UI_PANE_BREAKPOINTS };
	size_t i;

	for (i = 0; i < sizeof(derived) / sizeof(*derived); i++) {
		if (pane_displayed(derived[i])) {
			repaint_pane(derived[i]);
		}
	}
}

void dap_ui_refresh_now(void)
{
	dirty = false;
	due_ms = 0;
	repaint_all();
}

static void layout_restore(void);

int dap_ui_poll(void)
{
	/* Nothing under a prompt.  A pane repaint may create a buffer and a
	 * restore rearranges every window, and the prompt is standing in the
	 * world both of them would change (src/bufmgr.c's
	 * kg_event_prompt_enter() draws the same line for the event queue). */
	if (kg_event_prompt_active()) {
		return 0;
	}
	if (layout.restore_pending) {
		layout.restore_pending = false;
		layout_restore();
		return 1;
	}
	if (!dirty || now_ms() < due_ms) {
		return 0;
	}
	dirty = false;
	due_ms = 0;
	repaint_all();
	return 1;
}

/* -------------------------------- showing ----------------------------- */

bool dap_ui_show(enum dap_ui_pane pane)
{
	int slot = pane_slot(pane);

	if (slot < 0) {
		return false;
	}
	if (!pane_table[pane].transcript) {
		repaint_pane(pane);
		slot = buf_find_open(pane_table[pane].name);
	}
	if (slot < 0) {
		return false;
	}
	win_display_buffer_other_window(slot);
	return true;
}

/* -------------------------------- layout ------------------------------ */

/* The buffer the grid's source slot shows.  The current one, unless the
 * user pressed F12 from inside a pane -- a grid that listed one buffer
 * twice is refused by win_arrange_grid(), and refusing the whole layout
 * because the user was reading the stack would be a poor answer. */
static struct kg_buffer_handle source_buffer(void)
{
	int i;

	if (!dap_ui_current_buffer_is_pane()) {
		return buf_handle(buf_current);
	}
	for (i = 0; i < MAX_BUFFERS; i++) {
		const char *name = buflist[i].filename;
		size_t p;
		bool pane = false;

		if (!buflist[i].active || !name) {
			continue;
		}
		for (p = 0; p < DAP_UI_PANE_COUNT; p++) {
			pane = pane || strcmp(name, pane_table[p].name) == 0;
		}
		if (!pane) {
			return buf_handle(i);
		}
	}
	return buf_handle(buf_current);
}

/* The plan's arrangement, in ONE transaction:
 *
 *     *dap-repl*   | *dap-locals*
 *     source       | *dap-output*
 *     *dap-stack*  | *dap-breakpoints* / *dap-threads*
 *
 * win_arrange_grid() validates the handles, the window count and the
 * geometry BEFORE it mutates anything, so a terminal too small to hold six
 * windows refuses cleanly and leaves the user's layout alone. */
static bool layout_apply(void)
{
	static const enum dap_ui_pane order[6]
	    = { DAP_UI_PANE_REPL, DAP_UI_PANE_COUNT /* the source */,
		      DAP_UI_PANE_STACK, DAP_UI_PANE_LOCALS, DAP_UI_PANE_OUTPUT,
		      DAP_UI_PANE_BREAKPOINTS };
	struct kg_buffer_handle buffers[6];
	int rows_per_column[2] = { 3, 3 };
	enum kg_window_layout_result result;
	size_t i;

	if (layout.active) {
		return true;
	}
	for (i = 0; i < 6; i++) {
		enum dap_ui_pane pane = order[i];
		int slot;

		if (pane == DAP_UI_PANE_COUNT) {
			buffers[i] = source_buffer();
			continue;
		}
		if (pane == DAP_UI_PANE_BREAKPOINTS && show_threads) {
			pane = DAP_UI_PANE_THREADS;
		}
		slot = pane_slot(pane);
		if (slot < 0) {
			editor_set_status_message(
			    "dap-many-windows: no room for the panes");
			return false;
		}
		buffers[i] = buf_handle(slot);
	}
	/* Saved BEFORE the grid, and exactly once: this is the snapshot the
	 * toggle-off and the end of the session put back. */
	win_configuration_save(&layout.saved);
	result = win_arrange_grid(2, rows_per_column, buffers, 6, 1);
	if (result != KG_WINDOW_LAYOUT_OK) {
		win_configuration_clear(&layout.saved);
		editor_set_status_message(
		    "dap-many-windows: the debug layout does not fit");
		return false;
	}
	layout.active = true;
	dap_ui_refresh_now();
	return true;
}

/* Put the user's windows back and forget the snapshot.  A snapshot outlives
 * exactly one restore: a later F12 saves the THEN-CURRENT layout, which is
 * the only reading under which two toggles in a row are not a time machine.
 *
 * Windows the user split or deleted while the grid was up are discarded
 * with it -- the configuration that comes back is the pre-grid one. */
static void layout_restore(void)
{
	if (!layout.active) {
		return;
	}
	layout.active = false;
	(void)win_configuration_restore(&layout.saved);
	win_configuration_clear(&layout.saved);
}

void dap_ui_layout_toggle(void)
{
	if (layout.active) {
		layout_restore();
		layout.wanted = false;
	} else if (layout_apply()) {
		layout.wanted = true;
	} else {
		/* Whatever refused it has already said why, and "Debug layout
		 * off" over the top of that would be the editor answering its
		 * own question. */
		return;
	}
	editor_set_status_message(
	    "Debug layout %s", layout.active ? "on" : "off");
}

void dap_ui_layout_session_started(void)
{
	if (layout.wanted) {
		(void)layout_apply();
	}
}

void dap_ui_layout_session_ended(void)
{
	if (!layout.active) {
		return;
	}
	/* Under a prompt the restore waits for the poll that is not: a
	 * session dying while the user is answering a question does not get
	 * to rearrange the windows the question is on. */
	if (kg_event_prompt_active()) {
		layout.restore_pending = true;
		return;
	}
	layout_restore();
}

bool dap_ui_layout_active(void) { return layout.active; }

/* ------------------------------ the info map -------------------------- */

/* The row point is on, verified three ways before anything acts on it: the
 * buffer is still the pane kg last rendered, the transaction that rendered
 * it finished, and the line is one of the ones it rendered. */
static bool current_row(enum dap_ui_pane *pane_out, struct dap_ui_row *out)
{
	enum dap_ui_pane pane = current_pane();
	struct dap_ui_pane_state *p;
	int row = wcur()->rowoff + wcur()->cy;

	if (pane == DAP_UI_PANE_COUNT) {
		return false;
	}
	p = &panes[pane];
	if (p->generation_begun != p->generation_done || row < 0
	    || (size_t)row >= p->row_count) {
		return false;
	}
	*pane_out = pane;
	*out = p->rows[row];
	return true;
}

/* A row derived from a suspension the program has already left names ids
 * that every measured adapter answers with success and wrong data [M-4].
 * So it is refused, and says so. */
static bool row_is_current(const struct dap_ui_row *row)
{
	switch (row->kind) {
	case DAP_UI_ROW_FRAME:
	case DAP_UI_ROW_THREAD:
	case DAP_UI_ROW_SCOPE:
	case DAP_UI_ROW_VARIABLE:
		return row->suspension_epoch == dap_exec_epoch();
	default:
		return true;
	}
}

static void ret_frame(const struct dap_ui_row *row)
{
	size_t count = dap_exec_frame_count();
	size_t i;

	if (!row->navigable) {
		editor_set_status_message(
		    "This frame has no source kg can open");
		return;
	}
	for (i = 0; i < count; i++) {
		struct dap_exec_frame f;
		char path[PATH_MAX];

		if (!dap_exec_frame(i, &f) || f.id != row->stable_id) {
			continue;
		}
		snprintf(path, sizeof(path), "%s", f.path ? f.path : "");
		/* Select first, visit second: selecting bumps the selection
		 * generation, which is what stops the locals of the frame the
		 * user just left from painting over the new one -- and the
		 * frame's own storage may move while it does, which is why
		 * the path is copied out before the call. */
		(void)dap_exec_frame_select_at(i);
		/* The column is a 1-BASED BYTE column and DAP columns are not
		 * one (doc/coordinates.md), so kg passes 1. */
		(void)editor_visit_file_position(DAP_UI_WHO, path, f.line, 1,
		    panes[DAP_UI_PANE_STACK].buffer);
		dap_ui_changed();
		return;
	}
	editor_set_status_message("That frame is no longer in the stack");
}

static void ret_breakpoint(const struct dap_ui_row *row)
{
	const char *path = dap_breakpoint_source_path((size_t)row->parent);
	struct dap_breakpoint_info info;

	if (!path || !dap_breakpoint_find(path, row->stable_id, &info)) {
		editor_set_status_message("That breakpoint is gone");
		return;
	}
	(void)editor_visit_file_position(DAP_UI_WHO, path, info.line, 1,
	    panes[DAP_UI_PANE_BREAKPOINTS].buffer);
}

static void ret_variable(const struct dap_ui_row *row)
{
	char key[DAP_UI_KEY_MAX];
	size_t index = (size_t)row->stable_id;
	int slot;

	if (row->stable_id < 0 || !variable_key(index, key, sizeof(key))) {
		editor_set_status_message("That variable is gone");
		return;
	}
	slot = expanded_find(key);
	if (slot >= 0) {
		expanded[slot].used = false;
		dap_ui_changed();
		return;
	}
	if (!row->navigable) {
		editor_set_status_message("This variable has no children");
		return;
	}
	if (!expanded_add(key)) {
		editor_set_status_message(
		    "Too many expanded variables; collapse one first");
		return;
	}
	slot = expanded_find(key);
	if (slot >= 0) {
		expanded[slot].asked_epoch = dap_exec_epoch();
	}
	(void)dap_exec_expand_variable(index);
	dap_ui_changed();
}

void dap_ui_info_ret(void)
{
	struct dap_ui_row row;
	enum dap_ui_pane pane;

	if (!current_row(&pane, &row) || row.kind == DAP_UI_ROW_NONE) {
		editor_set_status_message("Nothing on this line");
		return;
	}
	if (!row_is_current(&row)) {
		editor_set_status_message(
		    "That line is from an earlier stop; the pane is about to "
		    "refresh");
		return;
	}
	switch (row.kind) {
	case DAP_UI_ROW_FRAME:
		ret_frame(&row);
		break;
	case DAP_UI_ROW_THREAD:
		editor_set_status_message(
		    "Thread %d is the one kg is looking at", row.stable_id);
		break;
	case DAP_UI_ROW_BREAKPOINT:
		ret_breakpoint(&row);
		break;
	case DAP_UI_ROW_VARIABLE:
		ret_variable(&row);
		break;
	default:
		editor_set_status_message("Nothing to do on this line");
		break;
	}
}

/* `d`, and only in a breakpoint row: deleting the line under point in the
 * stack would be a debugger inventing an operation the protocol does not
 * have. */
void dap_ui_info_delete(void)
{
	struct dap_ui_row row;
	enum dap_ui_pane pane;
	const char *path;

	if (!current_row(&pane, &row) || row.kind != DAP_UI_ROW_BREAKPOINT) {
		editor_set_status_message("Not a breakpoint line");
		return;
	}
	path = dap_breakpoint_source_path((size_t)row.parent);
	if (!path || !dap_breakpoint_remove_at(path, row.stable_id)) {
		editor_set_status_message("That breakpoint is gone");
		return;
	}
	editor_set_status_message("Breakpoint removed");
	dap_ui_changed();
	dap_ui_refresh_now();
}

void dap_ui_info_toggle_enable(void)
{
	struct dap_ui_row row;
	enum dap_ui_pane pane;
	struct dap_breakpoint_info info;
	const char *path;

	if (!current_row(&pane, &row) || row.kind != DAP_UI_ROW_BREAKPOINT) {
		editor_set_status_message("Not a breakpoint line");
		return;
	}
	path = dap_breakpoint_source_path((size_t)row.parent);
	if (!path || !dap_breakpoint_find(path, row.stable_id, &info)
	    || !dap_breakpoint_set_enabled(
		path, row.stable_id, !info.enabled)) {
		editor_set_status_message("That breakpoint is gone");
		return;
	}
	editor_set_status_message(
	    "Breakpoint %s", info.enabled ? "disabled" : "enabled");
	dap_ui_changed();
	dap_ui_refresh_now();
}

/* The two panes share one slot of the layout, as GDB's do, so the hidden
 * one needs a way back: this is it, and it is bound in the panes
 * themselves. */
void dap_ui_info_toggle_breakpoints_threads(void)
{
	enum dap_ui_pane want;
	int slot;

	show_threads = !show_threads;
	want = show_threads ? DAP_UI_PANE_THREADS : DAP_UI_PANE_BREAKPOINTS;
	slot = pane_slot(want);
	if (slot < 0) {
		editor_set_status_message("No room for that pane");
		return;
	}
	repaint_pane(want);
	slot = buf_find_open(pane_table[want].name);
	if (slot < 0) {
		return;
	}
	if (dap_ui_current_buffer_is_pane()) {
		(void)buf_select(slot);
	} else {
		win_display_buffer_other_window(slot);
	}
	editor_set_status_message("%s", pane_table[want].name);
}

/* ------------------------------- lifecycle ---------------------------- */

void dap_ui_init(void)
{
	size_t i;

	for (i = 0; i < DAP_UI_PANE_COUNT; i++) {
		panes[i].at_line_start = true;
	}
}

void dap_ui_reset(void)
{
	free(rb.text);
	memset(&rb, 0, sizeof(rb));
	memset(panes, 0, sizeof(panes));
	memset(expanded, 0, sizeof(expanded));
	memset(&layout, 0, sizeof(layout));
	show_threads = false;
	dirty = false;
	due_ms = 0;
	dap_ui_init();
}

/* ------------------------------ test seams ---------------------------- */

bool dap_ui_test_row(enum dap_ui_pane pane, size_t row, struct dap_ui_row *out)
{
	if (pane >= DAP_UI_PANE_COUNT || row >= panes[pane].row_count) {
		return false;
	}
	*out = panes[pane].rows[row];
	return true;
}

size_t dap_ui_test_row_count(enum dap_ui_pane pane)
{
	return pane < DAP_UI_PANE_COUNT ? panes[pane].row_count : 0;
}

unsigned dap_ui_test_generation(enum dap_ui_pane pane)
{
	return pane < DAP_UI_PANE_COUNT ? panes[pane].generation_done : 0;
}

size_t dap_ui_test_transcript_bytes(enum dap_ui_pane pane)
{
	return pane < DAP_UI_PANE_COUNT ? panes[pane].bytes : 0;
}

bool dap_ui_test_transcript_truncated(enum dap_ui_pane pane)
{
	return pane < DAP_UI_PANE_COUNT ? panes[pane].truncated : false;
}

bool dap_ui_test_expanded(size_t index) { return variable_is_expanded(index); }
