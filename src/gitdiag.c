/* gitdiag.c — the two git-mode diagnostics, and the seam that paints them
 *
 * See src/gitdiag.h for what is diagnosed and why it is not a
 * highlighter.  The rules below are the ones src/syntax_legacy.c's
 * gitcommit_syntax() and gitrebase_syntax() painted before this module
 * existed, word for word: the same subject limit, the same action
 * vocabulary (the facade's, via syntax_git_rebase_action_name()), the
 * same "only fixup and merge take -C/-c" flag test, and the same spans.
 * They are asserted in test/test_gitdiag.c, which both backend
 * configurations build.
 */

#include "gitdiag.h"
#include "bufhandle.h"
#include "decor.h"
#include "def.h"
#include "marker.h"
#include "syntax.h"
#include "vgeom.h"
#include <stdint.h>
#include <string.h>

/* ---- The pure half ---- */

int gitdiag_commit_subject_span(erow *row,
    const struct kg_display_options *options, struct kg_gitdiag_span *out)
{
	if (row->rsize <= KG_GITDIAG_SUBJECT_LIMIT) {
		return 0;
	}
	out->start
	    = render_to_chars_col(row, KG_GITDIAG_SUBJECT_LIMIT, options);
	out->end = row->size;
	/* A subject that is over the limit only because a TAB expands past
	 * it has no character of its own out there to mark, and an empty
	 * span is nothing a reader can see. */
	return out->start < out->end;
}

/* Append [start, end) to a bounded span list, dropping it once the list
 * is full.  Returns the new count. */
static int span_add(
    struct kg_gitdiag_span *out, int max, int n, int start, int end)
{
	if (n < max) {
		out[n].start = start;
		out[n].end = end;
		n++;
	}
	return n;
}

int gitdiag_rebase_row_spans(
    const char *line, int len, struct kg_gitdiag_span *out, int max)
{
	int i, w, n = 0, flags_ok, takes_commit = 0;
	const char *action;

	if (len == 0 || line[0] == '#') {
		return 0;
	}
	i = syntax_git_rebase_skip_ws(line, len, 0);
	w = syntax_git_rebase_skip_word(line, len, i);
	if (w == i) {
		return 0;
	}
	action = syntax_git_rebase_action_name(line + i, w - i, &takes_commit);
	if (!action) {
		/* A typo in the action word fails the whole rebase, so it is
		 * the one diagnosis worth making about a line nothing else
		 * here can read. */
		return span_add(out, max, 0, i, w);
	}
	if (strcmp(action, "exec") == 0) {
		return 0; /* the rest of the line is a shell command */
	}
	flags_ok = strcmp(action, "fixup") == 0 || strcmp(action, "merge") == 0;
	if (!takes_commit && !flags_ok) {
		return 0; /* takes no commit and no flags: nothing to check */
	}
	/* Option words before the hash: only fixup and merge take -C/-c, so
	 * any other flag makes git reject the whole todo. */
	i = syntax_git_rebase_skip_ws(line, len, w);
	while (i < len && line[i] == '-') {
		w = syntax_git_rebase_skip_word(line, len, i);
		if (!flags_ok || w - i != 2 || !strchr("Cc", line[i + 1])) {
			n = span_add(out, max, n, i, w);
		}
		i = syntax_git_rebase_skip_ws(line, len, w);
	}
	return n;
}

/* ---- The editor side ---- */

/* Below src/search.c's isearch match (priority 1) and level with
 * show-paren's: a diagnostic says something about the text that is true
 * whether or not you are looking at it, and the thing you just searched
 * for is the thing you are looking at. */
#define KG_GITDIAG_PRIORITY 0

/* What this module has published, and what it was published from.  The
 * decorations are addressed by handle -- there is no query that hands
 * back a deletable one -- so the handles are kept here; the store is
 * fixed (see KG_GITDIAG_MAX_DECOR), which is what makes this module
 * allocation-free.
 *
 * The stamp describes the last *git* buffer republished, so a repaint
 * walking a window list of ordinary buffers never invalidates it.  Two
 * git buffers on screen at once do thrash it, and then each frame
 * recomputes both -- correct, and cheaper than a second table for a
 * situation git itself never creates. */
static struct kg_decor_handle diag_decor[KG_GITDIAG_MAX_DECOR];
static int diag_count;

static struct {
	uint64_t buffer_id;
	uint64_t layout_generation;
	const struct editor_syntax *syntax;
	int valid;
} diag_state;

static int decor_names_buffer(
    struct kg_decor_handle h, struct kg_buffer_handle b)
{
	return h.buffer.slot == b.slot && h.buffer.id == b.id
	    && h.buffer.generation == b.generation;
}

/* Retire every diagnostic published for `b`, and every one whose buffer
 * has gone in the meantime -- a buffer kill frees its whole decoration
 * store, and the handles naming it are the entries this store would
 * otherwise keep forever. */
static void gitdiag_drop(struct editor_buffer *b)
{
	struct kg_buffer_handle bh = buf_handle_of(b);
	int i, kept = 0;

	for (i = 0; i < diag_count; i++) {
		if (decor_names_buffer(diag_decor[i], bh)
		    || kg_decor_resolve(diag_decor[i], NULL) != KG_DECOR_OK) {
			kg_decor_delete(diag_decor[i]);
			continue;
		}
		diag_decor[kept++] = diag_decor[i];
	}
	diag_count = kept;
	if (diag_state.valid && diag_state.buffer_id == b->id) {
		diag_state.valid = 0;
	}
}

/* Publish one span of row `row` as a warning decoration.  The endpoint
 * gravities are src/search.c's fully-formed-span pair: typing at either
 * edge does not extend a diagnosis, and the next repaint recomputes it
 * anyway. */
static void gitdiag_publish(
    struct editor_buffer *b, int row, const struct kg_gitdiag_span *span)
{
	size_t start, end;
	struct kg_decor_handle h;

	if (diag_count >= KG_GITDIAG_MAX_DECOR) {
		return;
	}
	start = buffer_row_col_to_position(b, row, span->start);
	end = buffer_row_col_to_position(b, row, span->end);
	h = kg_decor_create(b, start, end, KG_MARKER_GRAV_RIGHT,
	    KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING, KG_GITDIAG_PRIORITY,
	    true);
	if (h.id != 0) {
		diag_decor[diag_count++] = h;
	}
}

static void gitdiag_publish_commit(struct editor_buffer *b)
{
	struct kg_gitdiag_span span;
	int row = syntax_buffer_git_commit_subject(b);

	if (row < 0) {
		return;
	}
	if (gitdiag_commit_subject_span(&b->row[row], &b->display, &span)) {
		gitdiag_publish(b, row, &span);
	}
}

static void gitdiag_publish_rebase(struct editor_buffer *b)
{
	struct kg_gitdiag_span spans[KG_GITDIAG_ROW_SPANS_MAX];
	int j, k, n;

	for (j = 0; j < b->numrows; j++) {
		erow *r = &b->row[j];

		n = gitdiag_rebase_row_spans(
		    r->chars, r->size, spans, KG_GITDIAG_ROW_SPANS_MAX);
		for (k = 0; k < n; k++) {
			gitdiag_publish(b, j, &spans[k]);
		}
	}
}

/* Whether what is on screen for `b` was computed from exactly this
 * buffer, content and mode. */
static int gitdiag_state_current(const struct editor_buffer *b)
{
	return diag_state.valid && diag_state.buffer_id == b->id
	    && diag_state.layout_generation == b->layout_generation
	    && diag_state.syntax == b->syntax;
}

void gitdiag_update(struct editor_buffer *b)
{
	int commit, rebase;

	if (!b || !b->active) {
		return;
	}
	commit = syntax_buffer_is_git_commit(b);
	rebase = syntax_buffer_is_git_rebase(b);
	if (!commit && !rebase) {
		/* Not a git buffer -- but it may have been one a moment ago,
		 * and a mode change is not an edit, so the retire is
		 * unconditional rather than guarded by the stamp. */
		gitdiag_drop(b);
		return;
	}
	if (gitdiag_state_current(b)) {
		return;
	}
	gitdiag_drop(b);
	if (commit) {
		gitdiag_publish_commit(b);
	} else {
		gitdiag_publish_rebase(b);
	}
	diag_state.buffer_id = b->id;
	diag_state.layout_generation = b->layout_generation;
	diag_state.syntax = b->syntax;
	diag_state.valid = 1;
}
