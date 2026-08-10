/* lsp_diag.c — see lsp_diag.h.
 *
 * Compiled in both configurations, the way src/xref.c is: the command
 * table has one unconditional row for each command here, so a WITH_LSP=0
 * kg answers `M-x lsp-diagnostics` with the reason there is no answer
 * rather than with "Unknown command".
 */

#include "lsp_diag.h"

#ifdef KG_USE_LSP

#include "bufhandle.h"
#include "cmd.h"
#include "compile_nav.h"
#include "decor.h"
#include "def.h"
#include "event.h"
#include "localvars.h"
#include "lsp_json.h"
#include "lsp_server.h"
#include "lsp_sync.h"
#include "lsp_uri.h"
#include "marker.h"
#include "next_error.h"
#include "syntax.h"
#include "visit.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lsp_client;

/* Each command's name, as every message it prints spells it: the name a
 * user would type at M-x, so a refusal can be looked up from what it
 * said. */
#define DIAG_WHO "lsp-diagnostics"
#define DIAG_WHO_GOTO "lsp-diagnostics-goto"

/* ------------------------------- the store ---------------------------- */

/* One diagnostic, as it arrived.  The two positions are the protocol's --
 * a line and a character counted in `struct diag_file::encoding` -- and
 * they stay that way until something with the target row's bytes in front
 * of it converts them (diag_flat_pos below).  `message` is malloc'd and
 * bounded; a store of fixed-size message buffers would be a quarter of a
 * megabyte of static that a session with no language server in it still
 * pays for. */
struct diag_item {
	int line;
	int end_line;
	long long character;
	long long end_character;
	int severity;
	char *message;
};

/* Everything one file's last publish left behind.
 *
 * `version` is what the publish carried, or -1 when it carried none, and
 * is the whole of the staleness rule: a later publish naming a smaller
 * version is describing text the server has already been sent a newer
 * copy of.
 *
 * `painted` and `decors` are the marks currently on screen for this file.
 * They are tracked rather than recomputed because the decoration store is
 * buffer-wide and shared -- isearch's match and show-paren's pair live in
 * it too -- so "remove what I put there" is only answerable by having kept
 * the handles. */
struct diag_file {
	char *path;
	long long version;
	enum lsp_position_encoding encoding;
	struct diag_item items[LSP_DIAG_MAX_PER_FILE];
	size_t count;
	/* How many the publish carried, capped or not, so the listing can
	 * say when it cut rather than showing a short answer as a complete
	 * one. */
	size_t reported;
	struct kg_buffer_handle painted;
	struct kg_decor_handle decors[LSP_DIAG_MAX_PER_FILE];
	size_t decor_count;
};

static struct diag_file g_files[LSP_DIAG_MAX_FILES];

/* The listing, flattened: one entry per row, in the order the rows are
 * printed, which is what RET indexes and what next-error walks.  Rebuilt
 * by every publish, so the two views never disagree about how many there
 * are. */
struct diag_row {
	size_t file;
	size_t item;
};

static struct diag_row g_rows[LSP_DIAG_MAX_FILES * LSP_DIAG_MAX_PER_FILE];
static size_t g_row_count;

static struct kg_buffer_handle g_listing;
/* Bumped by every publish; a cursor from an earlier generation is treated
 * as never having visited anything (src/compile_nav.h). */
static uint32_t g_generation;
static struct compile_nav_cursor g_cursor = { -1, 0 };

/* Drop the marks this file put in a buffer.  Every handle either resolves
 * and is deleted or has already gone with the rows that anchored it (a
 * reload, a kill), which kg_decor_delete() answers false for and this
 * ignores: what matters is that the module stops claiming them. */
static void diag_unpaint(struct diag_file *f)
{
	size_t i;

	for (i = 0; i < f->decor_count; i++) {
		(void)kg_decor_delete(f->decors[i]);
	}
	f->decor_count = 0;
	f->painted = (struct kg_buffer_handle) { 0 };
}

/* Empty a file's entry: its marks, its messages, its path.  The slot is
 * left zeroed and so is free for the next file to claim. */
static void diag_file_clear(struct diag_file *f)
{
	size_t i;

	diag_unpaint(f);
	for (i = 0; i < f->count; i++) {
		free(f->items[i].message);
	}
	free(f->path);
	memset(f, 0, sizeof(*f));
}

static struct diag_file *diag_file_find(const char *path)
{
	size_t i;

	for (i = 0; i < LSP_DIAG_MAX_FILES; i++) {
		if (g_files[i].path && strcmp(g_files[i].path, path) == 0) {
			return &g_files[i];
		}
	}
	return NULL;
}

/* The entry this path publishes into: its own if it has one, else the
 * first slot holding nothing, else the first slot whose last publish was
 * empty -- a file somebody fixed still owns a slot, and it is the one
 * worth taking.  NULL when eight files all still have something to say,
 * which drops the ninth publish rather than silently forgetting one of
 * the eight. */
static struct diag_file *diag_file_slot(const char *path)
{
	struct diag_file *f = diag_file_find(path);
	size_t i;

	if (f) {
		return f;
	}
	for (i = 0; i < LSP_DIAG_MAX_FILES; i++) {
		if (!g_files[i].path) {
			return &g_files[i];
		}
	}
	for (i = 0; i < LSP_DIAG_MAX_FILES; i++) {
		if (g_files[i].count == 0) {
			diag_file_clear(&g_files[i]);
			return &g_files[i];
		}
	}
	return NULL;
}

/* ------------------------------- reading ------------------------------ */

/* A server's message text, bounded and made safe to be a listing row: one
 * line, since a row's position in the listing is what RET indexes results
 * by, and no control bytes, which display_glyph_at() would render but
 * which would still cost the listing its one-row-per-result property.
 * Truncation may cut a UTF-8 sequence in half; that costs one replacement
 * glyph on screen and nothing else. */
static char *diag_message_copy(const char *text, size_t len)
{
	char *out;
	size_t i;

	if (len > LSP_DIAG_MESSAGE_MAX) {
		len = LSP_DIAG_MESSAGE_MAX;
	}
	out = malloc(len + 1);
	if (!out) {
		return NULL;
	}
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)text[i];

		out[i] = c < 0x20 || c == 0x7f ? ' ' : text[i];
	}
	out[len] = '\0';
	return out;
}

/* A Position, in the protocol's own units.  A line that is not a number is
 * a diagnostic kg cannot place; a character that is not one is column
 * zero, which is what a server omitting it means. */
static bool diag_read_position(
    const struct lsp_json_value *pos, int *line, long long *character)
{
	long long n = lsp_json_int(lsp_json_get(pos, "line"), -1);

	if (lsp_json_kind_of(pos) != LSP_JSON_OBJECT || n < 0 || n > INT_MAX) {
		return false;
	}
	*line = (int)n;
	*character = lsp_json_int(lsp_json_get(pos, "character"), 0);
	if (*character < 0) {
		*character = 0;
	}
	return true;
}

/* One Diagnostic.  `range` and `message` are the only members kg reads:
 * the code, the source and the related information are all things a
 * listing one line wide has nowhere to put.  A severity the protocol does
 * not name is kept as it arrived rather than corrected -- see
 * diag_severity_text(). */
static bool diag_read_item(
    const struct lsp_json_value *v, struct diag_item *out)
{
	const struct lsp_json_value *range = lsp_json_get(v, "range");
	const char *message;
	size_t len = 0;

	if (!diag_read_position(
		lsp_json_get(range, "start"), &out->line, &out->character)) {
		return false;
	}
	if (!diag_read_position(lsp_json_get(range, "end"), &out->end_line,
		&out->end_character)) {
		out->end_line = out->line;
		out->end_character = out->character;
	}
	out->severity
	    = (int)lsp_json_int(lsp_json_get(v, "severity"), LSP_DIAG_ERROR);
	message = lsp_json_str(lsp_json_get(v, "message"), &len);
	out->message
	    = diag_message_copy(message ? message : "", message ? len : 0);
	return out->message != NULL;
}

/* Position order within a file, so the listing and the next-error walk are
 * both in the order a reader would work through the file.  An insertion
 * sort because the bound is 64 and the input is very nearly sorted
 * already: servers publish in the order they found things. */
static void diag_sort(struct diag_file *f)
{
	size_t i, j;

	for (i = 1; i < f->count; i++) {
		struct diag_item key = f->items[i];

		for (j = i; j > 0; j--) {
			struct diag_item *p = &f->items[j - 1];

			if (p->line < key.line
			    || (p->line == key.line
				&& p->character <= key.character)) {
				break;
			}
			f->items[j] = *p;
		}
		f->items[j] = key;
	}
}

/* Path order across files, so a listing over two files is stable whatever
 * order the servers happened to publish in.  Same shape and same reason as
 * diag_sort(), over eight entries. */
static void diag_rows_build(void)
{
	size_t order[LSP_DIAG_MAX_FILES];
	size_t n = 0;
	size_t i, j;

	g_row_count = 0;
	for (i = 0; i < LSP_DIAG_MAX_FILES; i++) {
		if (!g_files[i].path) {
			continue;
		}
		for (j = n; j > 0; j--) {
			if (strcmp(g_files[order[j - 1]].path, g_files[i].path)
			    <= 0) {
				break;
			}
			order[j] = order[j - 1];
		}
		order[j] = i;
		n++;
	}
	for (i = 0; i < n; i++) {
		for (j = 0; j < g_files[order[i]].count; j++) {
			g_rows[g_row_count].file = order[i];
			g_rows[g_row_count].item = j;
			g_row_count++;
		}
	}
}

/* ------------------------------- painting ----------------------------- */

/* The buffer visiting `path`, or NULL.  Matched on the absolute path the
 * sync layer would send for that buffer, which is the same string the URI
 * was decoded into, so a buffer opened by a relative name still matches
 * the file a server is talking about. */
static struct editor_buffer *diag_buffer_for_path(
    const char *path, struct kg_buffer_handle *out)
{
	char abs[PATH_MAX];
	int i;

	for (i = 0; i < buf_count; i++) {
		if (lsp_sync_abs_path(&buflist[i], abs, sizeof(abs))
		    && strcmp(abs, path) == 0) {
			*out = buf_handle(i);
			return &buflist[i];
		}
	}
	return NULL;
}

/* A protocol position as a flat byte offset into `b`.  This is the mirror
 * of the encoding src/xref.c applies on the way out, and it is here rather
 * than at read time for that module's reason: a character offset can only
 * be decoded against the bytes of the row it is on, and those are not
 * known until a buffer visits the file.  A line past the end of the buffer
 * lands at its end, which is what buffer_row_col_to_position() does with
 * one. */
static size_t diag_flat_pos(struct editor_buffer *b,
    enum lsp_position_encoding enc, int line, long long character)
{
	const erow *r;
	size_t col;

	if (line < 0 || line >= b->numrows) {
		return buffer_byte_length(b);
	}
	r = &b->row[line];
	col = lsp_pos_decode(enc, r->chars, (size_t)r->size, (size_t)character);
	return buffer_row_col_to_position(b, line, (int)col);
}

/* Put this file's marks in whatever buffer visits it, replacing whatever
 * it had there.  Nothing to paint into is not a failure: the diagnostics
 * are kept, and the buffer-opened subscriber paints them the moment one
 * appears.
 *
 * One face for every severity.  The renderer's colour channel is a single
 * SGR foreground number (src/syntax.c's editor_syntax_to_color()), and
 * KG_DECOR_FACE_WARNING is already the red an error wants; the numbers
 * left over are ones other faces already mean, so a KG_DECOR_FACE_ERROR
 * would be a second name for the same paint.  Severity is spelled out in
 * the listing instead, which is where it is legible.  What severity does
 * change here is the decoration's priority, so that where an error and a
 * warning overlap the error is the one on screen. */
static void diag_paint(struct diag_file *f)
{
	struct kg_buffer_handle handle = { 0 };
	struct editor_buffer *b = diag_buffer_for_path(f->path, &handle);
	size_t i;

	diag_unpaint(f);
	if (!b || b->numrows == 0) {
		return;
	}
	f->painted = handle;
	for (i = 0; i < f->count; i++) {
		const struct diag_item *it = &f->items[i];
		size_t start
		    = diag_flat_pos(b, f->encoding, it->line, it->character);
		size_t end = diag_flat_pos(
		    b, f->encoding, it->end_line, it->end_character);

		/* An empty range is a real answer -- "here, before this
		 * character" -- and a zero-width mark is not visible, so it
		 * covers the one byte it points at. */
		if (end <= start) {
			end = start + 1;
		}
		f->decors[f->decor_count++]
		    = kg_decor_create(b, start, end, KG_MARKER_GRAV_RIGHT,
			KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING,
			it->severity == LSP_DIAG_ERROR ? 1 : 0, true);
	}
}

/* ------------------------------ the publish --------------------------- */

/* Read the array into `f`, which already holds nothing.  Elements past the
 * cap are counted and dropped; an element kg cannot place is dropped and
 * not counted, since `reported` is what the listing's truncation notice
 * counts against. */
static void diag_fill(struct diag_file *f, const struct lsp_json_value *list)
{
	size_t n = lsp_json_len(list);
	size_t i;

	for (i = 0; i < n; i++) {
		struct diag_item item = { 0 };

		if (!diag_read_item(lsp_json_at(list, i), &item)) {
			free(item.message);
			continue;
		}
		f->reported++;
		if (f->count == LSP_DIAG_MAX_PER_FILE) {
			free(item.message);
			continue;
		}
		f->items[f->count++] = item;
	}
	diag_sort(f);
}

/* The path a publish is about, or false for a URI naming something kg
 * cannot open -- another host, another scheme -- which is a server to
 * ignore rather than a file to guess at. */
static bool diag_publish_path(const struct lsp_json_value *params, char *out,
    size_t out_size, long long *version)
{
	const struct lsp_json_value *uri = lsp_json_get(params, "uri");

	if (lsp_json_kind_of(uri) != LSP_JSON_STRING) {
		return false;
	}
	*version = lsp_json_int(lsp_json_get(params, "version"), -1);
	return lsp_uri_to_path(lsp_json_str(uri, NULL), out, out_size);
}

void lsp_diag_publish(
    const struct lsp_json_value *params, enum lsp_position_encoding enc)
{
	char path[PATH_MAX];
	long long version = -1;
	struct diag_file *f;
	char *owned;

	if (!diag_publish_path(params, path, sizeof(path), &version)) {
		return;
	}
	f = diag_file_slot(path);
	if (!f) {
		return;
	}
	/* Stale: the server is describing text it has since been sent a
	 * newer copy of, and its positions were measured against that older
	 * text.  A publish with no version is never stale. */
	if (f->path && version >= 0 && f->version > version) {
		return;
	}
	owned = f->path ? f->path : strdup(path);
	if (!owned) {
		return;
	}
	f->path = NULL; /* diag_file_clear() must not free what is kept */
	diag_file_clear(f);
	f->path = owned;
	f->version = version;
	f->encoding = enc;
	diag_fill(f, lsp_json_get(params, "diagnostics"));
	diag_paint(f);
	diag_rows_build();
	g_generation++;
	g_cursor = (struct compile_nav_cursor) { -1, g_generation };
}

/* ------------------------------ the wiring ---------------------------- */

static void diag_on_notification(struct lsp_client *c, const char *method,
    const struct lsp_json_value *params)
{
	if (strcmp(method, "textDocument/publishDiagnostics") != 0) {
		return;
	}
	lsp_diag_publish(params, lsp_client_caps(c)->position_encoding);
}

/* A buffer has started visiting a file.  Whatever a server already said
 * about that file belongs on its rows, which is why this is a subscriber
 * and not something the publish path can do alone: the publish may well
 * have arrived while the file had no buffer at all. */
static enum kg_event_cb_status diag_on_buffer_opened(
    const struct kg_event *ev, enum kg_event_resolution resolution, void *ctx)
{
	struct editor_buffer *b = buf_resolve(ev->payload.buffer_life.buffer);
	char abs[PATH_MAX];
	struct diag_file *f;

	(void)resolution;
	(void)ctx;
	if (b && lsp_sync_abs_path(b, abs, sizeof(abs))
	    && (f = diag_file_find(abs)) != NULL) {
		diag_paint(f);
	}
	return KG_EVENT_CB_CONTINUE;
}

void lsp_diag_install(void)
{
	static struct kg_event_subscriber_token open_token;

	lsp_client_set_notify_hook(diag_on_notification);
	/* Idempotent, for lsp_sync_install()'s reason: a token that named
	 * nothing, or whose slot a later registration reused, unsubscribes
	 * nothing (src/event.h). */
	(void)kg_event_unsubscribe(open_token);
	open_token = kg_event_subscribe(
	    1u << KG_EVENT_BUFFER_OPENED, diag_on_buffer_opened, NULL);
}

/* ------------------------------ the listing --------------------------- */

/* Emacs' flymake listing names the severities `error`, `warning`, `note`;
 * kg's listing rows are compilation-shaped (`path:line:col: severity:
 * message`), which is the same word in the same place, so the two agree
 * where it matters.  A severity outside the protocol's four is named
 * neutrally rather than guessed at. */
static const char *diag_severity_text(int severity)
{
	switch (severity) {
	case LSP_DIAG_ERROR:
		return "error";
	case LSP_DIAG_WARNING:
		return "warning";
	case LSP_DIAG_INFORMATION:
		return "note";
	case LSP_DIAG_HINT:
		return "hint";
	default:
		return "diagnostic";
	}
}

/* One line into the listing.  A line that would not fit its own buffer is
 * truncated rather than dropped: the row's position in the listing is what
 * RET indexes by, so a missing row would move every diagnostic below it. */
[[gnu::format(printf, 2, 3)]] static bool diag_append_line(
    int slot, const char *fmt, ...)
{
	char line[PATH_MAX + LSP_DIAG_MESSAGE_MAX + 64];
	va_list ap;
	int written;

	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	written = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (written < 0) {
		return false;
	}
	if ((size_t)written >= sizeof(line)) {
		written = (int)sizeof(line) - 1;
	}
	return buf_append_special_text(slot, line, (size_t)written) == 0;
}

/* Build (or rebuild) the listing from the store.  Returns its slot, or -1
 * when there was no room for it.
 *
 * `text_syntax` for xref's and occur's reason: the compilation record
 * carries no highlighting at all, so reusing it would change nothing but
 * the mode line, which would then say "Compilation" in a buffer no
 * compilation produced.
 *
 * The column is the protocol's `character` counted from one and not a byte
 * column, exactly as *xref* prints it: turning it into one needs the
 * target row's bytes, which is what the visit does when RET asks for them.
 * The two agree on every ASCII line. */
static int diag_render(void)
{
	int slot
	    = buf_prepare_special_text(LSP_DIAG_BUFFER_NAME, &text_syntax, 1);
	size_t i;

	if (slot < 0
	    || !diag_append_line(slot, "%zu diagnostic%s\n", g_row_count,
		g_row_count == 1 ? "" : "s")) {
		return -1;
	}
	for (i = 0; i < g_row_count; i++) {
		const struct diag_file *f = &g_files[g_rows[i].file];
		const struct diag_item *it = &f->items[g_rows[i].item];

		if (!diag_append_line(slot, "%s:%d:%lld: %s: %s\n",
			buf_basename(f->path), it->line + 1, it->character + 1,
			diag_severity_text(it->severity), it->message)) {
			return -1;
		}
	}
	for (i = 0; i < LSP_DIAG_MAX_FILES; i++) {
		if (g_files[i].reported > g_files[i].count) {
			(void)diag_append_line(slot, "… %s: %zu more\n",
			    buf_basename(g_files[i].path),
			    g_files[i].reported - g_files[i].count);
		}
	}
	return slot;
}

/* ------------------------------ navigation ---------------------------- */

/* Visit row `index`: leave the listing's window when there is another one
 * to land in, open the file, and place point -- in two steps, because the
 * second cannot be taken before the first.  The stored `character` is
 * counted in the server's encoding and can only be decoded against the
 * bytes of the target row, which kg does not have until the file is open,
 * so the visit lands at the start of the line and the column is placed
 * afterwards (src/xref.c takes the same two steps for the same reason). */
static void diag_visit(size_t index)
{
	const struct diag_file *f = &g_files[g_rows[index].file];
	const struct diag_item *it = &f->items[g_rows[index].item];
	struct editor_buffer *b;
	size_t col;
	int row;

	if (!editor_visit_file_position(
		DIAG_WHO, f->path, it->line + 1, 1, g_listing)) {
		return;
	}
	b = bcur();
	row = editor_current_filerow_or_eof();
	if (row < 0 || row >= b->numrows) {
		return;
	}
	col = lsp_pos_decode(f->encoding, b->row[row].chars,
	    (size_t)b->row[row].size, (size_t)it->character);
	editor_goto_line_direct(row + 1, (int)col + 1);
}

/* next-error/previous-error, once the listing owns the keys.  The shape is
 * occur_move()'s exactly -- prefix argument as a step count, a message
 * when it ran past an end, then the visit -- because the two are the same
 * command over two stores. */
static void diag_move(int direction)
{
	bool clamped;
	int step;

	if (g_row_count == 0) {
		editor_set_status_message("No diagnostics");
		return;
	}
	step = editor.current_prefix.supplied ? editor.current_prefix.value : 1;
	g_cursor = compile_nav_cursor_advance(
	    g_cursor, g_generation, g_row_count, step * direction, &clamped);
	if (clamped) {
		editor_set_status_message(direction > 0
			? "No more diagnostics"
			: "No earlier diagnostics");
	}
	diag_visit((size_t)g_cursor.index);
}

static const struct next_error_source g_next_error_source
    = { DIAG_WHO, diag_move };

/* Whether point is in the listing this store describes.  The buffer handle
 * rather than the name, for occur's reason: a listing the user killed and
 * a new buffer that took its slot are both called *Diagnostics*, and only
 * the handle tells them apart. */
static bool diag_in_listing(void)
{
	return g_listing.id != 0 && buf_handle_slot(g_listing) == buf_current;
}

void editor_lsp_diagnostics_goto(int fd)
{
	int row = wcur()->rowoff + wcur()->cy;

	(void)fd;
	if (!diag_in_listing()) {
		editor_set_status_message(
		    DIAG_WHO_GOTO ": not in " LSP_DIAG_BUFFER_NAME);
		return;
	}
	/* Row 0 is the header and the rows after the last diagnostic are the
	 * truncation notices, so both answer "nothing here" rather than the
	 * nearest one. */
	if (row < 1 || (size_t)row > g_row_count) {
		editor_set_status_message(
		    DIAG_WHO_GOTO ": no diagnostic on this line");
		return;
	}
	/* A visit is where the next-error walk now is, so that M-g n after a
	 * RET continues from the diagnostic just visited (Emacs'
	 * next-error-found). */
	g_cursor = (struct compile_nav_cursor) { row - 1, g_generation };
	diag_visit((size_t)row - 1);
}

/* -------------------------------- the command ------------------------- */

/* Bring the current buffer to its server's attention, so that a server
 * that has never been told about this document publishes about it.
 *
 * This is not what Emacs' flymake-show-buffer-diagnostics does -- there,
 * the checker is already running -- but kg opens a document lazily, on the
 * first command that needs one (src/lsp_sync.h), so a listing command that
 * asked nothing would list nothing forever in a session where no xref
 * command had been run.  Best-effort and silent: no server for this mode,
 * or a buffer with no file, is not a reason to refuse to show what other
 * files already said. */
static void diag_touch_current(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	struct editor_buffer *b = bcur();
	struct lsp_client *c;
	char path[PATH_MAX];

	if (!lsp_sync_abs_path(b, path, sizeof(path))) {
		return;
	}
	c = lsp_server_for(
	    b->syntax ? b->syntax->id : KG_MODE_TEXT, path, &status);
	if (c) {
		(void)lsp_sync_before_request(c, buf_handle(buf_current));
	}
}

void editor_lsp_diagnostics(int fd)
{
	int slot;

	(void)fd;
	diag_touch_current();
	if (g_row_count == 0) {
		editor_set_status_message("No diagnostics");
		return;
	}
	slot = diag_render();
	if (slot < 0) {
		editor_set_status_message(DIAG_WHO ": no room for the listing");
		return;
	}
	g_listing = buf_handle(slot);
	next_error_set_source(&g_next_error_source);
	win_display_buffer_other_window(slot);
	editor_set_status_message("%zu diagnostic%s — RET visits, q closes",
	    g_row_count, g_row_count == 1 ? "" : "s");
}

/* ------------------------------ test seams ---------------------------- */

size_t lsp_diag_test_row_count(void) { return g_row_count; }

bool lsp_diag_test_row(size_t index, const char **out_path, int *out_line,
    long long *out_character, int *out_severity, const char **out_message)
{
	const struct diag_file *f;
	const struct diag_item *it;

	if (index >= g_row_count) {
		return false;
	}
	f = &g_files[g_rows[index].file];
	it = &f->items[g_rows[index].item];
	if (out_path) {
		*out_path = f->path;
	}
	if (out_line) {
		*out_line = it->line;
	}
	if (out_character) {
		*out_character = it->character;
	}
	if (out_severity) {
		*out_severity = it->severity;
	}
	if (out_message) {
		*out_message = it->message;
	}
	return true;
}

void lsp_diag_test_reset(void)
{
	size_t i;

	for (i = 0; i < LSP_DIAG_MAX_FILES; i++) {
		diag_file_clear(&g_files[i]);
	}
	g_row_count = 0;
	g_listing = (struct kg_buffer_handle) { 0 };
	g_generation++;
	g_cursor = (struct compile_nav_cursor) { -1, g_generation };
}

#else /* !KG_USE_LSP */

#include "def.h"

void lsp_diag_install(void) { }

void lsp_diag_publish(
    const struct lsp_json_value *params, enum lsp_position_encoding enc)
{
	(void)params;
	(void)enc;
}

void editor_lsp_diagnostics(int fd)
{
	(void)fd;
	editor_set_status_message("kg was built without LSP support");
}

void editor_lsp_diagnostics_goto(int fd)
{
	(void)fd;
	editor_set_status_message("kg was built without LSP support");
}

size_t lsp_diag_test_row_count(void) { return 0; }

bool lsp_diag_test_row(size_t index, const char **out_path, int *out_line,
    long long *out_character, int *out_severity, const char **out_message)
{
	(void)index;
	(void)out_path;
	(void)out_line;
	(void)out_character;
	(void)out_severity;
	(void)out_message;
	return false;
}

void lsp_diag_test_reset(void) { }

#endif /* KG_USE_LSP */
