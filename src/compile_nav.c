/* compile_nav.c — see compile_nav.h. */

#include "compile_nav.h"

#include "compile.h"
#include "decor.h"
#include "def.h"
#include "marker.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

/* One diagnostic beyond what compile_parse.h already gives: where it sits
 * inside *compilation* (a marker plus a highlight decoration, both created
 * when the line committed) and, lazily -- created the first time
 * next-error visits it -- where it last put point in the source file.
 * `source_marker_valid` is what tells a stale/never-visited entry apart
 * from a live one; a false `kg_marker_handle` alone cannot, since a
 * zeroed handle and a deleted one both resolve as gone. */
struct compile_nav_extra {
	struct kg_marker_handle line_marker;
	struct kg_decor_handle line_decor;
	bool source_marker_valid;
	struct kg_buffer_handle source_buffer;
	struct kg_marker_handle source_marker;
};

struct compile_nav_run {
	struct compile_diag_parser parser;
	struct compile_diag_store diags;
	struct compile_nav_extra extra[KG_COMPILE_DIAG_MAX];
	struct kg_buffer_handle compilation_buffer;
};

static struct compile_nav_run g_run;
/* Bumped by every compile_nav_reset(); see compile_nav_cursor_advance()'s
 * comment for what a mismatch against this means. */
static uint32_t g_generation;
/* The command-owned cursor: next-error/previous-error's own place in
 * g_run.diags, not a field of it. */
static struct compile_nav_cursor g_cursor = { -1, 0 };

void compile_nav_reset(struct kg_buffer_handle compilation_buffer,
    const char *initial_cwd, size_t initial_cwd_len)
{
	for (size_t i = 0; i < g_run.diags.count; i++) {
		if (g_run.extra[i].source_marker_valid) {
			kg_marker_delete(g_run.extra[i].source_marker);
		}
	}
	memset(&g_run, 0, sizeof(g_run));
	compile_diag_parser_init(&g_run.parser, initial_cwd, initial_cwd_len);
	compile_diag_store_init(&g_run.diags);
	g_run.compilation_buffer = compilation_buffer;

	g_generation++;
	g_cursor = (struct compile_nav_cursor) { -1, g_generation };
}

void compile_nav_ingest_line(
    const char *line, size_t len, size_t line_start_pos)
{
	size_t before = g_run.diags.count;
	struct editor_buffer *cb;
	struct compile_nav_extra *ex;

	compile_diag_parse_line(&g_run.parser, &g_run.diags, line, len);
	if (g_run.diags.count == before) {
		return; /* not a diagnostic line: nothing to attach */
	}
	cb = buf_resolve(g_run.compilation_buffer);
	if (!cb) {
		return; /* the compilation buffer is gone */
	}

	ex = &g_run.extra[before];
	*ex = (struct compile_nav_extra) { 0 };
	ex->line_marker
	    = kg_marker_create(cb, line_start_pos, KG_MARKER_GRAV_LEFT);
	ex->line_decor = kg_decor_create(cb, line_start_pos,
	    line_start_pos + len, KG_MARKER_GRAV_LEFT, KG_MARKER_GRAV_RIGHT,
	    KG_DECOR_FACE_WARNING, 0, true);
}

struct compile_nav_cursor compile_nav_cursor_advance(
    struct compile_nav_cursor cursor, uint32_t current_generation, size_t count,
    int step, bool *clamped)
{
	long base, target;

	if (cursor.generation != current_generation) {
		cursor.index = -1;
	}
	if (count == 0) {
		if (clamped) {
			*clamped = true;
		}
		return (struct compile_nav_cursor) { -1, current_generation };
	}
	if (step == 0) {
		step = 1;
	}

	base = cursor.index;
	if (base < 0) {
		/* Never visited: the first forward move starts one before the
		 * first record, the first backward move one after the last,
		 * so a single step of either direction lands on that end. */
		base = (step > 0) ? -1 : (long)count;
	}
	target = base + step;
	if (target < 0) {
		target = 0;
	} else if (target >= (long)count) {
		target = (long)count - 1;
	}
	if (clamped) {
		*clamped = (target != base + step);
	}
	return (struct compile_nav_cursor) { (int)target, current_generation };
}

/* Whether `s` embeds a raw NUL: compiler output is not guaranteed
 * NUL-free, and open()ing a path built past one would silently name
 * something other than what the diagnostic said. */
static bool has_embedded_nul(const char *s, size_t len)
{
	return memchr(s, '\0', len) != NULL;
}

/* The diagnostic's file, joined with its recorded directory context when
 * relative, as a NUL-terminated path.  False for anything this module will
 * not trust enough to open: a truncated or NUL-embedding path or cwd, or a
 * result too long for `out`. */
static bool diag_build_path(
    const struct compile_diag *d, char *out, size_t out_size)
{
	size_t need;

	if (d->file_len == 0 || d->file_truncated
	    || has_embedded_nul(d->file, d->file_len)) {
		return false;
	}
	if (d->file[0] == '/' || d->cwd_len == 0) {
		if (d->file_len >= out_size) {
			return false;
		}
		memcpy(out, d->file, d->file_len);
		out[d->file_len] = '\0';
		return true;
	}
	if (d->cwd_truncated || has_embedded_nul(d->cwd, d->cwd_len)) {
		return false;
	}
	need = d->cwd_len + 1 + d->file_len;
	if (need >= out_size) {
		return false;
	}
	memcpy(out, d->cwd, d->cwd_len);
	out[d->cwd_len] = '/';
	memcpy(out + d->cwd_len + 1, d->file, d->file_len);
	out[need] = '\0';
	return true;
}

/* Move focus to another window before displaying a diagnostic's source, so
 * *compilation* stays visible: only when there are at least two windows and
 * the current one is showing it, otherwise a jump reuses whatever window
 * the user is already in, like any other buffer switch.  Picks the next
 * window in the C-x o cycle rather than hunting for one already showing the
 * target -- a deliberately simple rule, not an attempt to reproduce
 * display-buffer's window reuse. */
static void nav_pick_window(void)
{
	if (win_count <= 1
	    || !win_shows_buffer(wcur(), g_run.compilation_buffer)) {
		return;
	}
	for (int i = 1; i <= MAX_WINDOWS; i++) {
		int idx = (win_current + i) % MAX_WINDOWS;

		if (winlist[idx].active && idx != win_current) {
			win_current = idx;
			return;
		}
	}
}

/* Open `path`'s buffer (reusing it if already open) and land it in the
 * window nav_pick_window() chose.  A path that resolves to a directory, or
 * that stat() cannot see at all, is reported and refused rather than
 * handed to buf_open_path() -- which would happily start a new, empty
 * buffer for a file that does not exist, exactly the "reused storage"
 * this module's contract refuses to paper over with. */
static bool nav_open_source(const char *path)
{
	struct stat st;

	if (buf_find_open(path) < 0) {
		if (stat(path, &st) != 0) {
			editor_set_status_message(
			    "next-error: %s: %s", path, strerror(errno));
			return false;
		}
		if (S_ISDIR(st.st_mode)) {
			editor_set_status_message(
			    "next-error: %s is a directory", path);
			return false;
		}
		if (buf_count >= MAX_BUFFERS) {
			editor_set_status_message(
			    "next-error: too many open buffers (%d max)",
			    MAX_BUFFERS);
			return false;
		}
	}
	nav_pick_window();
	buf_open_path(path, 0);
	return bcur()->filename && strcmp(bcur()->filename, path) == 0;
}

/* Remember where this visit landed, so a later one can prefer the marker
 * over the diagnostic's original line/column -- which is what keeps
 * next-error accurate across edits made to the file in between. */
static void nav_remember_marker(struct compile_nav_extra *ex)
{
	size_t pos = buffer_row_col_to_position(
	    bcur(), editor_current_filerow_or_eof(), editor_current_filecol());

	ex->source_buffer = buf_handle(buf_current);
	ex->source_marker = kg_marker_create(bcur(), pos, KG_MARKER_GRAV_LEFT);
	ex->source_marker_valid = ex->source_marker.id != 0;
}

/* Try to revisit via the marker a previous visit left.  False when there is
 * none, or it (or the buffer it lives in) has gone stale -- a killed or
 * reused source buffer -- which also clears `ex->source_marker_valid` so
 * the caller falls back to re-opening the file by path fresh, rather than
 * trusting a dangling handle into whatever now occupies its old slot. */
static bool nav_visit_marker(struct compile_nav_extra *ex)
{
	struct editor_buffer *sb;
	size_t pos;
	int row, col;

	if (!ex->source_marker_valid) {
		return false;
	}
	sb = buf_resolve(ex->source_buffer);
	if (!sb || kg_marker_resolve(ex->source_marker, &pos) != KG_MARKER_OK) {
		ex->source_marker_valid = false;
		return false;
	}
	nav_pick_window();
	if (!buf_select(buf_handle_slot(ex->source_buffer))) {
		return true; /* refused under event pressure; still handled */
	}
	if (buffer_position_to_row_col(bcur(), pos, &row, &col) == 1) {
		editor_goto_line_direct(row + 1, col + 1);
	}
	return true;
}

static void nav_visit(int idx)
{
	struct compile_diag *d = &g_run.diags.entries[idx];
	struct compile_nav_extra *ex = &g_run.extra[idx];
	char path[PATH_MAX];

	if (nav_visit_marker(ex)) {
		return;
	}
	if (!diag_build_path(d, path, sizeof(path))) {
		editor_set_status_message(
		    "next-error: diagnostic path was truncated in compiler "
		    "output");
		return;
	}
	if (!nav_open_source(path)) {
		return;
	}
	editor_goto_line_direct(
	    (int)d->line, d->has_column ? (int)d->column : 1);
	nav_remember_marker(ex);
}

static void nav_move(int direction)
{
	bool clamped;
	int step;

	if (g_run.diags.count == 0) {
		editor_set_status_message("No compilation diagnostics");
		return;
	}
	step = editor.current_prefix.supplied ? editor.current_prefix.value : 1;
	g_cursor = compile_nav_cursor_advance(g_cursor, g_generation,
	    g_run.diags.count, step * direction, &clamped);
	if (clamped) {
		editor_set_status_message(direction > 0
			? "No more diagnostics"
			: "No previous diagnostics");
	}
	nav_visit(g_cursor.index);
}

void editor_next_error(int fd)
{
	(void)fd;
	nav_move(1);
}

void editor_previous_error(int fd)
{
	(void)fd;
	nav_move(-1);
}

static const struct compile_diag_hooks g_hooks
    = { compile_nav_reset, compile_nav_ingest_line };

void compile_nav_install(void) { compilation_set_diag_hooks(&g_hooks); }

size_t compile_nav_test_count(void) { return g_run.diags.count; }

bool compile_nav_test_diag(size_t index, struct compile_diag *out)
{
	if (index >= g_run.diags.count) {
		return false;
	}
	if (out) {
		*out = g_run.diags.entries[index];
	}
	return true;
}

bool compile_nav_test_line_marker_pos(size_t index, size_t *out_pos)
{
	if (index >= g_run.diags.count) {
		return false;
	}
	return kg_marker_resolve(g_run.extra[index].line_marker, out_pos)
	    == KG_MARKER_OK;
}
