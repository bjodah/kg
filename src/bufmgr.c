/* ========================= Buffer management ============================== */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform.h"
#else
#include <sys/types.h>
#include <time.h>
#endif

#include "bufhandle.h"
#include "bufmgr.h"
#include "cmdstate.h"
#include "compile.h"
#include "decor.h"
#include "def.h"
#include "edit.h"
#include "event.h"
#include "kbd.h"
#include "keyevent.h"
#include "lisp.h"
#include "localvars.h"
#include "marker.h"
#include "perf.h"
#include "process_table.h"
#include "syntax.h"
#include "yank.h"

/* Key sets the minibuffer and the pickers ask about.  A list rather than
 * a run of comparisons: see key_in_list(). */
static const struct key_event erase_keys[]
    = { { KEY_BASE_DELETE, 0 }, { 'h', KEY_MOD_CTRL }, { KEY_BASE_DEL, 0 } };
static const struct key_event history_keys[] = { { 'p', KEY_MOD_META },
	{ 'n', KEY_MOD_META }, { KEY_BASE_UP, 0 }, { KEY_BASE_DOWN, 0 },
	{ 'p', KEY_MOD_CTRL }, { 'n', KEY_MOD_CTRL } };
static const struct key_event history_back_keys[]
    = { { 'p', KEY_MOD_META }, { KEY_BASE_UP, 0 }, { 'p', KEY_MOD_CTRL } };
static const struct key_event picker_next_keys[]
    = { { KEY_BASE_RIGHT, 0 }, { 'f', KEY_MOD_CTRL } };
static const struct key_event picker_prev_keys[]
    = { { KEY_BASE_LEFT, 0 }, { 'b', KEY_MOD_CTRL } };
static const struct key_event cancel_keys[]
    = { { KEY_BASE_ESC, 0 }, { 'g', KEY_MOD_CTRL } };

static void buf_picker_cycle(int *selection, int matches, int direction);
#include <fcntl.h>
#include <limits.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* Synthetic syntax records for special modes. */
static struct editor_syntax ibuffer_syntax
    = { KG_MODE_IBUFFER, "IBuffer", NULL, NULL, "", "", "", 0, NULL };
struct editor_syntax text_syntax
    = { KG_MODE_TEXT, "Text", NULL, NULL, "", "", "", 0, NULL };
struct editor_syntax lisp_interaction_syntax
    = { KG_MODE_LISP_INTERACTION, "Lisp Interaction", NULL, NULL, ";", "", "",
	      HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL };
struct editor_syntax compilation_syntax
    = { KG_MODE_COMPILATION, "Compilation", NULL, NULL, "", "", "", 0, NULL };

/* Column offset of the filename field in a *Buffer List* data row.
 * Format: " %c  %-24s  %6d  %-14s  %s"
 *          1+1+2+24  +2+6  +2+14  +2 = 54  */
#define IBUF_FILENAME_OFFSET 54
#define IBUF_NAME "*Buffer List*"
#define HELP_NAME "*help*"

struct editor_buffer buflist[MAX_BUFFERS];
int buf_current = 0;
int buf_count = 0;

#define AUTOREVERT_POLL_INTERVAL_SEC 2

static void silent_revert_current(void);
static void buf_apply_local_settings(void);

/* Next buffer identity to hand out.  Starts at 1 so a zeroed slot — and a
 * zeroed handle — names no buffer.
 *
 * 64 bits, because the counter's whole job is the claim that it never
 * repeats and 32 bits does not get to make it.  4.29 billion claims is a
 * long session but it is a number, and a wrapped id would hand a live
 * buffer the identity a long-dead one's handle still holds — the exact
 * confusion handles exist to prevent.  At 64 bits, one claim per
 * nanosecond runs for 584 years. */
static uint64_t buf_next_id = 1;

/* Hand `slot` to a buffer that is not the one that had it.  This is the only
 * place identity is assigned, so every handle taken before the handover stops
 * resolving at exactly the point the old buffer stopped existing. */
static void buf_claim_slot(int slot)
{
	buflist[slot].generation++;
	if (buf_next_id == UINT64_MAX) {
		/* Exhausted.  Reuse is refused rather than wrapped: the
		 * slot keeps id 0, which names no buffer, so it is
		 * unreachable by handle instead of confusable with one. */
		buflist[slot].id = 0;
		editor_set_status_message("Buffer identity exhausted.");
		return;
	}
	buflist[slot].id = buf_next_id++;
}

/* Give `slot` a resolvable identity: reserve capacity for the
 * KG_EVENT_BUFFER_OPENED event a fresh identity produces, claim the slot,
 * mark it active, then publish -- in that order, so a full queue refuses
 * the open before the slot resolves rather than after.  A no-op that
 * always succeeds when `slot` is already active: most callers (switching
 * to a buffer that already exists) never touch the reservation at all.
 * Returns 0, the slot untouched, only when the reservation was refused. */
static int buf_activate_slot(int slot)
{
	struct kg_event_reservation res;

	if (buflist[slot].active) {
		return 1;
	}
	res = kg_event_reserve_lifecycle();
	if (!res.valid) {
		return 0;
	}
	buf_claim_slot(slot);
	buflist[slot].active = 1;
	kg_event_publish_lifecycle(
	    &res, kg_event_make_buffer_opened(buf_handle(slot)));
	return 1;
}

/* The kg_slot_table view of buflist[], for the generic identity checks in
 * bufhandle.h -- see struct kg_slot_table's comment there for why a second
 * handle family shares this instead of repeating it. */
static const struct kg_slot_table buf_slot_table = { buflist, MAX_BUFFERS,
	sizeof(buflist[0]), offsetof(struct editor_buffer, active),
	offsetof(struct editor_buffer, id),
	offsetof(struct editor_buffer, generation) };

struct kg_buffer_handle buf_handle(int slot)
{
	struct kg_buffer_handle handle = { -1, 0, 0 };

	if (!kg_slot_active_at(buf_slot_table, slot)) {
		return handle;
	}
	handle.slot = slot;
	handle.id = buflist[slot].id;
	handle.generation = buflist[slot].generation;
	return handle;
}

struct kg_buffer_handle buf_handle_of(const struct editor_buffer *b)
{
	int slot = kg_slot_find(buf_slot_table, b);

	return slot < 0 ? (struct kg_buffer_handle) { -1, 0, 0 }
			: buf_handle(slot);
}

/* The buffer `handle` names, or NULL once it is gone.  Callers must handle
 * NULL: it is the answer for a killed buffer and for a slot that has been
 * handed to somebody else, which are the two cases a bare index confuses.
 * KG_SLOT_UNNAMED (out of range, or a zeroed id) never counts as stale: a
 * view that names nothing on purpose is asked about on every window scan
 * and must not inflate the counter. */
struct editor_buffer *buf_resolve(struct kg_buffer_handle handle)
{
	enum kg_slot_check r;
	void *rec = kg_slot_resolve(
	    buf_slot_table, handle.slot, handle.id, handle.generation, &r);

	if (r == KG_SLOT_STALE) {
		KG_PERF_INC(KG_PERF_HANDLE_STALE);
	}
	return rec;
}

/* The slot `handle` names, or -1.  For the call sites that still speak in
 * indices; they at least stop speaking in stale ones. */
int buf_handle_slot(struct kg_buffer_handle handle)
{
	return buf_resolve(handle) ? handle.slot : -1;
}

struct editor_buffer *win_buffer(const struct editor_window *w)
{
	return buf_resolve(w->buf);
}

int win_buffer_slot(const struct editor_window *w)
{
	return buf_handle_slot(w->buf);
}

int win_shows_buffer(
    const struct editor_window *w, struct kg_buffer_handle handle)
{
	const struct editor_buffer *shown = win_buffer(w);

	/* Pointer equality after both sides resolve, so a dead handle is
	 * unequal to everything including another dead one. */
	return shown && shown == buf_resolve(handle);
}

/* Free `w`'s visual-line geometry index (src/vgeom.h), if it has one.
 * Every window lifecycle transition below -- losing its buffer, gaining
 * a different one, session teardown -- has to do this regardless of
 * whether visual-line mode is ever used, so it cannot go through
 * src/vgeom.c's own API without pulling that module's dependencies (the
 * row-geometry primitives in src/mode.c, editor_cursor_goto()) into
 * every test binary that merely manages windows.  A plain free() on the
 * opaque pointer is safe without them: src/vgeom.c documents, at
 * struct kg_vgeom_index's definition, that the index is exactly one
 * malloc()'d block, so freeing it needs no knowledge of its layout. */
static void window_vgeom_reset(struct editor_window *w)
{
	free(w->vgeom);
	w->vgeom = NULL;
}

/* Every window's, at once: editor_cleanup()'s "session teardown" leg of
 * the same lifetime rule, pulled out so that call is a single statement
 * rather than a second loop counted against its own complexity budget. */
static void window_vgeom_reset_all(void)
{
	for (int i = 0; i < MAX_WINDOWS; i++) {
		window_vgeom_reset(&winlist[i]);
	}
}

/* Hand the buffer a window is leaving the point that window had in it, so
 * the next view to visit that buffer can resume there.  Called whenever a
 * view detaches; this is the only writer of last_point. */
void buf_remember_view(const struct editor_window *w)
{
	struct editor_buffer *b = win_buffer(w);

	if (!b) {
		return;
	}
	b->last_point.cx = w->cx;
	b->last_point.cy = w->cy;
	b->last_point.rowoff = w->rowoff;
	b->last_point.coloff = w->coloff;
	b->last_point.rowoff_visual = w->rowoff_visual;
}

/* Resolve `h` and reserve capacity for the KG_EVENT_VIEW_ATTACHED event
 * buf_attach_view() would publish -- both while a refusal still costs `w`
 * nothing.  NULL when the slot does not resolve, `w` already shows it (no
 * transition to publish), or the queue has no room; `*res` is a live
 * reservation only in the case this returns non-NULL. */
static struct editor_buffer *buf_attach_prepare(struct editor_window *w,
    struct kg_buffer_handle h, struct kg_event_reservation *res)
{
	struct editor_buffer *b = buf_resolve(h);

	if (!b || win_shows_buffer(w, h)) {
		return NULL;
	}
	*res = kg_event_reserve_lifecycle();
	return res->valid ? b : NULL;
}

/* Point `w` at buffer slot `slot`, resuming where that buffer was last
 * left.  The goal column does not survive the move: it is only meaningful
 * between two consecutive vertical motions in one buffer.  Reservation
 * failure leaves `w` showing whatever it already did. */
void buf_attach_view(struct editor_window *w, int slot)
{
	struct kg_buffer_handle h = buf_handle(slot);
	struct kg_event_reservation res = { 0 };
	struct editor_buffer *b = buf_attach_prepare(w, h, &res);

	if (!b) {
		return;
	}
	buf_remember_view(w);
	/* `w` is provably switching to a different buffer here (see
	 * buf_attach_prepare()'s win_shows_buffer() check above), so
	 * whatever geometry index it had cached is for the buffer it is
	 * about to stop showing.  buf_detach_view_commit() covers the
	 * detach-then-attach path; this covers buf_select()'s direct
	 * attach, which does not detach first. */
	window_vgeom_reset(w);
	w->buf = h;
	w->cx = b->last_point.cx;
	w->cy = b->last_point.cy;
	w->rowoff = b->last_point.rowoff;
	w->coloff = b->last_point.coloff;
	w->rowoff_visual = b->last_point.rowoff_visual;
	w->desired_visual_col = -1;
	kg_event_publish_lifecycle(
	    &res, kg_event_make_view_attached(win_handle_of(w), h));
}

/* The whole of buf_detach_view(): pulled out so the wrapper stays a single
 * call (see its comment for why).  Reserves capacity for the
 * KG_EVENT_VIEW_DETACHED event before detaching; when `w` shows nothing
 * there is no transition to publish, so it just clears `w` as before.  A
 * refused reservation leaves `w` attached -- a state win_check_handles()
 * already recovers from if the buffer it names dies anyway. */
static void buf_detach_view_commit(struct editor_window *w)
{
	struct kg_buffer_handle old = w->buf;
	struct kg_event_reservation res;

	if (!buf_resolve(old)) {
		buf_remember_view(w);
		w->buf = (struct kg_buffer_handle) { 0, 0, 0 };
		window_vgeom_reset(w);
		return;
	}
	res = kg_event_reserve_lifecycle();
	if (!res.valid) {
		return;
	}
	buf_remember_view(w);
	w->buf = (struct kg_buffer_handle) { 0, 0, 0 };
	window_vgeom_reset(w);
	kg_event_publish_lifecycle(
	    &res, kg_event_make_view_detached(win_handle_of(w), old));
}

void buf_detach_view(struct editor_window *w) { buf_detach_view_commit(w); }

/* Show buflist[slot] in the selected window and make it the current buffer.
 * This is the whole of what switching buffers is now: there is no state to
 * copy out of the outgoing buffer and none to copy into the incoming one.
 * The slot claim is here because a buffer being built into a free slot is
 * made current before it has any content. */
int buf_select(int slot)
{
	struct editor_buffer *b;

	if (slot < 0 || slot >= MAX_BUFFERS) {
		return 0;
	}
	b = &buflist[slot];
	if (!buf_activate_slot(slot)) {
		editor_set_status_message(
		    "Too many pending events; buffer not opened.");
		return 0;
	}
	if (win_count > 0) {
		buf_attach_view(wcur(), slot);
	}
	buf_current = slot;
	/* readonly is derived, so it is recomputed rather than stored. */
	editor_refresh_readonly_state();
	/* A cycle a command was part-way through belongs to the buffer it
	 * was cycling; the next invocation in this one starts over. */
	cmd_clear_transient();

	/* If this buffer was flagged stale while it sat in its slot, reload
	 * it now — but only when the user has opted in and there are no
	 * unsaved edits to lose. */
	if (b->disk_changed && !b->dirty
	    && (b->auto_revert || global_auto_revert)) {
		silent_revert_current();
	}
	return 1;
}

/* Clamp `w`'s cursor (cx/cy/rowoff/coloff) to whatever `b` can hold,
 * without recentering.  After a reload the file may be shorter than the
 * old view; this keeps the cursor on a real row and column while
 * preserving the user's scroll position when possible. */
static void clamp_view_to_buffer(
    struct editor_window *w, const struct editor_buffer *b)
{
	int filerow, filecol, rowsize;

	if (b->numrows == 0) {
		w->cx = w->cy = w->rowoff = w->coloff = 0;
		return;
	}

	filerow = w->rowoff + w->cy;
	filecol = w->coloff + w->cx;
	if (filerow >= b->numrows) {
		filerow = b->numrows - 1;
	}
	if (filerow < 0) {
		filerow = 0;
	}

	rowsize = b->row[filerow].size;
	if (filecol > rowsize) {
		filecol = rowsize;
	}
	if (filecol < 0) {
		filecol = 0;
	}

	if (w->rowoff > filerow) {
		w->rowoff = filerow;
	}
	if (w->rowoff + w->h <= filerow) {
		w->rowoff = filerow - w->h + 1;
	}
	if (w->rowoff < 0) {
		w->rowoff = 0;
	}
	w->cy = filerow - w->rowoff;

	if (w->coloff > filecol) {
		w->coloff = filecol;
	}
	if (w->coloff + w->w <= filecol) {
		w->coloff = filecol - w->w + 1;
	}
	if (w->coloff < 0) {
		w->coloff = 0;
	}
	w->cx = filecol - w->coloff;
}

/* Reload the current buffer's file from disk and reset undo history.
 * Leaves the cursor wherever the caller had it set; callers that want
 * the post-reload cursor clamped to the new file size should call
 * clamp_view_to_buffer() afterwards, once per window on the buffer.
 *
 * Syncs the fresh state back into buflist[buf_current] so other windows
 * showing the same buffer don't dangle on the old, freed filename pointer
 * after editor_open's realloc. */
void buf_reload_from_disk(void)
{
	if (!bcur()->filename) {
		return;
	}

	struct temp_load_result res;
	if (load_file_transactional(bcur()->filename, &res) != 0) {
		editor_set_status_message("Reload failed: %s", strerror(errno));
		free_load_result(&res);
		return;
	}

	kg_mark_clear(bcur());
	bcur()->shift_select = 0;
	bcur()->rect_mode = 0;

	commit_load_result(&res);

	free_load_result(&res);

	buf_apply_local_settings();

	undo_free();
	undo_init();
	undo_mark_clean();
}

/* Auto-revert path: reload the buffer from disk and try to keep the user's
 * viewport intact (rather than recentering on the cursor as an explicit
 * M-x revert-buffer would).  Never called against a dirty buffer.
 *
 * Every window on the buffer is clamped, not just the selected one.  A
 * second window keeps its own point, and buf_attach_view() has nothing to
 * do for a window already on the buffer, so an unclamped one stays past
 * the new end of file until the user types there -- and typing at a row
 * that does not exist appends the blank lines to reach it. */
static void silent_revert_current(void)
{
	int saved_cx = wcur()->cx;
	int saved_cy = wcur()->cy;
	int saved_rowoff = wcur()->rowoff;
	int saved_coloff = wcur()->coloff;
	int i;

	buf_reload_from_disk();

	wcur()->cx = saved_cx;
	wcur()->cy = saved_cy;
	wcur()->rowoff = saved_rowoff;
	wcur()->coloff = saved_coloff;

	/* Resolving each view and comparing the buffer it lands on is the
	 * whole test: a window showing nothing resolves to NULL, and bcur()
	 * is never NULL. */
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (win_buffer(&winlist[i]) == bcur()) {
			clamp_view_to_buffer(&winlist[i], bcur());
		}
	}

	editor_set_status_message(
	    "Reverted %s from disk", buf_basename(bcur()->filename));
}

/* Walk every active buffer, stat its underlying file, and update the
 * disk_changed flag when its identity disagrees with our last seen
 * snapshot, or when it cannot be sampled at all.  Cheap on a local FS; we
 * still rate-limit to keep network
 * stat() latency from compounding across keystrokes.
 *
 * Returns 1 if anything visible changed (a flag transitioned, or a buffer
 * was silently reverted) and the screen wants a redraw.  Returns 0 when
 * the debounce skipped the scan or nothing of consequence shifted. */
int autorevert_poll(void)
{
	static time_t last_poll;
	time_t now = time(NULL);
	int refresh_needed = 0;
	int i;

	if (now - last_poll < AUTOREVERT_POLL_INTERVAL_SEC) {
		return 0;
	}
	last_poll = now;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];
		struct file_snapshot now;
		enum file_change_state state;
		int new_changed;

		if (!b->active || is_special_buffer(b->filename)) {
			continue;
		}

		(void)file_snapshot_path(b->filename, &now);
		state = file_snapshot_compare(&b->disk, &now);
		new_changed = state != FILE_SAME;
		if (new_changed != b->disk_changed) {
			b->disk_changed = new_changed;
			refresh_needed = 1;
		}
		/* Reload only what can still be read back.  A file that
		 * vanished, or that we cannot even stat, leaves the buffer as
		 * the last copy of its contents: flag it and leave it be.
		 * Only the current buffer is reverted here: reloading another
		 * one would move a point this loop cannot see. */
		if (i == buf_current && state == FILE_DIFFERENT
		    && now.id.present && !b->dirty
		    && (b->auto_revert || global_auto_revert)) {
			silent_revert_current();
			refresh_needed = 1;
		}
	}
	return refresh_needed;
}

/* Reset editor to a clean empty state and initialise a fresh undo stack.
 * Used before loading a new file into editor. */
/* Empty the current buffer and the session state that goes with it, ready
 * for a file to be read in.  Callers select the slot the new buffer is to
 * live in *before* calling: the buffer's text no longer passes through
 * `editor` on its way to a slot, so this always empties the slot that is
 * about to be filled rather than the one the user was in.  Freeing the
 * rows and the undo chain rather than dropping them is what makes that
 * safe for a slot that still held something. */
static void buf_reset(void)
{
	struct editor_buffer *b = bcur();

	wcur()->cx = wcur()->cy = 0;
	wcur()->rowoff = wcur()->coloff = 0;
	editor_free_all_rows(b);
	b->dirty = 0;
	b->syntax = NULL;
	bcur()->filename = NULL;
	kg_mark_clear(bcur());
	kg_mark_ring_clear(bcur());
	bcur()->shift_select = 0;
	bcur()->rect_mode = 0;
	key_reset_pending_sequence();
	editor.prefix_pending = 0;
	editor.prefix_arg = 0;
	editor.prefix_no_digits = 0;
	editor.paste_mode = 0;
	bcur()->readonly = 0;
	bcur()->readonly_local = 0;
	bcur()->readonly_override = -1;
	bcur()->compile_command[0] = '\0';
	bcur()->compile_command_user_override = 0;
	memset(&bcur()->disk, 0, sizeof(bcur()->disk));
	bcur()->disk_changed = 0;
	bcur()->auto_revert = 0;
	bcur()->visual_line_mode = 0;
	wcur()->rowoff_visual = 0;
	bcur()->overwrite_mode = 0;
	undo_stack_free(&b->undostack);
	undo_init();
}

/* Slot holding the open buffer called `name`, or -1 when it is not
 * open. */
static int buf_find_by_name(const char *name)
{
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename
		    && strcmp(buflist[i].filename, name) == 0) {
			return i;
		}
	}
	return -1;
}

/* The public half of buf_find_by_name(): a caller (next-error) that has to
 * decide *where* to display a file -- reuse a window already showing it,
 * or not -- before it can open it needs the answer before buf_open_path()
 * would give it one. */
int buf_find_open(const char *path) { return buf_find_by_name(path); }

/* Lowest unused buffer slot, or -1 when the table is full. */
static int buf_first_free_slot(void)
{
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active) {
			return i;
		}
	}
	return -1;
}

/* Render a unique display name for buflist[idx] into `out`.  If no other
 * active buffer shares its basename the bare basename is used; otherwise
 * the immediate parent directory is prepended ("dir/foo"), matching
 * Emacs' uniquify "forward" style.  Doesn't try to resolve deeper
 * collisions where two buffers share both basename and parent dir
 * (rare; the file name column in C-x C-b still distinguishes them). */
void buf_display_name(int idx, char *out, size_t outsize)
{
	struct editor_buffer *b = &buflist[idx];
	const char *path = b->filename;
	const char *base = buf_basename(path);
	const char *parent_end, *parent_start;
	int parent_len, i, dup = 0;

	if (path) {
		for (i = 0; i < MAX_BUFFERS; i++) {
			if (i == idx || !buflist[i].active
			    || !buflist[i].filename) {
				continue;
			}
			if (strcmp(buf_basename(buflist[i].filename), base)
			    == 0) {
				dup = 1;
				break;
			}
		}
	}

	if (!dup) {
		snprintf(out, outsize, "%s", base);
		return;
	}

	parent_end = strrchr(path, '/');
	if (!parent_end || parent_end == path) {
		snprintf(out, outsize, "%s", base);
		return;
	}
	parent_start = parent_end - 1;
	while (parent_start > path && parent_start[-1] != '/') {
		parent_start--;
	}
	parent_len = parent_end - parent_start;
	snprintf(out, outsize, "%.*s/%s", parent_len, parent_start, base);
}

/* Reset the echo-area cursor state and clear the status line.  Centralises
 * the "leaving the minibuffer" handshake so every exit path agrees --
 * including, now, kg_event_prompt_leave(): every read that entered through
 * kg_event_prompt_enter() (editor_read_line_with_history(),
 * editor_read_line_path()) returns through exactly this one function, so
 * this is the one place that balances it. */
static int prompt_done(int rc)
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
static int prompt_cursor_col(
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
	editor.echo_cursor_col = prompt_cursor_col(prompt, plen, buf, cursor);
	editor_refresh_screen();
}

/* Seed a path-prompt buffer with the full directory path of the current
 * buffer's file (trailing '/' included), shortened with "~" when the
 * path lives under $HOME.  Resolves relative paths via realpath so the
 * user can backspace up the tree to navigate elsewhere.  Falls back to
 * the directory part of the recorded filename when realpath fails (the
 * file doesn't exist on disk yet), and to empty for special / unsaved
 * buffers. */
void editor_prompt_prefill_dir(char *buf, int bufsize)
{
	const char *fn = bcur()->filename;
	const char *home, *path, *slash;
	char *abs = NULL;
	int dir_len, home_len;

	buf[0] = '\0';
	if (!fn || is_special_buffer(fn)) {
		return;
	}
	abs = realpath(fn, NULL);
	path = abs ? abs : fn; /* fall back to as-recorded path */
	slash = strrchr(path, '/');
	if (!slash) {
		free(abs);
		return;
	}
	dir_len = (int)(slash - path) + 1;

	home = getenv("HOME");
	home_len = home ? (int)strlen(home) : 0;
	if (home_len > 0 && dir_len > home_len
	    && strncmp(path, home, home_len) == 0 && path[home_len] == '/') {
		int rest = dir_len - home_len;
		if (rest + 2 <= bufsize) {
			buf[0] = '~';
			memcpy(buf + 1, path + home_len, rest);
			buf[1 + rest] = '\0';
		}
	} else if (dir_len + 1 <= bufsize) {
		memcpy(buf, path, dir_len);
		buf[dir_len] = '\0';
	}
	free(abs);
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
 * and prompt_done() is what keeps the last prompt kill from coalescing
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
		    = prompt_cursor_col(prompt, plen, buf, cursor);
		editor_refresh_screen();
		yank->note = NULL;
		return;
	}
	prompt_refresh(prompt, plen, buf, cursor);
}

/* Offset just past the whitespace run and the word that follow `pos`,
 * i.e. where M-f lands. */
static int minibuf_word_end(const char *buf, int len, int pos)
{
	while (pos < len && isspace((unsigned char)buf[pos])) {
		pos++;
	}
	while (pos < len && !isspace((unsigned char)buf[pos])) {
		pos++;
	}
	return pos;
}

/* Offset at the start of the word preceding `pos`, i.e. where M-b
 * lands. */
static int minibuf_word_start(const char *buf, int pos)
{
	while (pos > 0 && isspace((unsigned char)buf[pos - 1])) {
		pos--;
	}
	while (pos > 0 && !isspace((unsigned char)buf[pos - 1])) {
		pos--;
	}
	return pos;
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

static int minibuf_edit_key(int fd, struct key_event c, char *buf, int bufsize,
    int *cursor, int *len, int *overflow, struct minibuf_yank *yank)
{
	char seq[4];
	int n, raw;

	if (KEY_IS(c, KEY_BASE_DELETE, 0) || KEY_IS(c, KEY_BASE_DEL, 0)
	    || KEY_IS(c, 'h', KEY_MOD_CTRL)) {
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
	int hist_index = -1; /* -1 == editing the draft, not a history entry */
	char draft[bufsize];
	int draft_cursor = cursor;
	int draft_overflow = 0;
	struct minibuf_yank yank = { 0 };

	buf[len] = '\0';
	/* kg_event_drain_safe() must defer for the whole of this read, not
	 * just its final keystroke: the prompt is on screen and mid-edit
	 * from its first character, so a callback repainting the echo area
	 * (or killing the buffer the read is scoped to) could otherwise run
	 * underneath it.  Balanced by prompt_done(), this function's one
	 * exit path. */
	kg_event_prompt_enter();
	/* Keystrokes read here are the prompt's, not commands: nothing that
	 * happens inside it may look like a repeat of what ran before it. */
	cmd_clear_transient();
	while (1) {
		minibuf_prompt_paint(prompt, plen, buf, cursor, &yank);
		c = editor_read_key(fd);
		/* Every prompt keystroke is its own kill-class boundary, and
		 * yank-pop eligibility is exactly "the key before this one
		 * was a yank": clear it here, and only a successful C-y/M-y
		 * sets it again. */
		cmd_state_prompt_keystroke();
		yank.eligible = yank.valid;
		yank.valid = 0;
		if (hist && KEY_IN_LIST(history_keys, c)) {
			int dir = KEY_IN_LIST(history_back_keys, c) ? 1 : -1;
			const char *entry;

			if (dir > 0 && hist_index < 0) {
				snprintf(draft, bufsize, "%s", buf);
				draft_cursor = cursor;
				draft_overflow = overflow;
			}
			entry = minibuf_history_walk(
			    hist, dir, &hist_index, draft);
			if (entry) {
				len = (int)strnlen(entry, bufsize - 1);
				memmove(buf, entry, len);
				buf[len] = '\0';
				if (hist_index < 0) {
					cursor = draft_cursor;
					overflow = draft_overflow;
				} else {
					cursor = len;
					overflow = 0;
				}
			}
			continue;
		}
		if (minibuf_edit_key(
			fd, c, buf, bufsize, &cursor, &len, &overflow, &yank)) {
			continue;
		}
		if (KEY_IN_LIST(cancel_keys, c)) {
			return prompt_done(-1);
		} else if (KEY_IS(c, KEY_BASE_RET, 0)) {
			if (overflow == 0) {
				minibuf_history_add(hist, buf);
			}
			return prompt_done(overflow > 0);
		}
	}
}

/* Append printf-style at `*off` into `msg[size]`, clamping `*off` to
 * size-1 so subsequent calls don't underflow `size - *off`. */
void editor_msg_appendf(char *msg, int size, int *off, const char *fmt, ...)
{
	va_list ap;
	int n;
	int avail = size - *off;

	if (avail <= 1) {
		msg[size - 1] = '\0';
		*off = size - 1;
		return;
	}
	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	n = vsnprintf(msg + *off, (size_t)avail, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	*off += n;
	if (*off > size - 1) {
		*off = size - 1;
	}
}

/* Is `name` the basename of any currently-open buffer's filename?
 * Used to push already-open files to the back of the path picker so
 * the default selection lands on something not yet open, matching
 * Emacs' ido-mode behaviour.  Basename-only comparison mirrors how
 * Emacs identifies "the same file" across distinct prompt paths. */
static int file_open_in_buflist(const char *name)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename
		    && strcmp(buf_basename(buflist[i].filename), name) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Stable-partition entries[0..n) so files already open in a buffer
 * move to the tail, with the in-group rank+name order preserved on
 * both sides.  Two passes keep the implementation obvious; entries[]
 * is small (≤PICKER_MAX_ENTRIES). */
static void push_open_files_back(struct path_entry *entries, int n)
{
	struct path_entry tmp[PICKER_MAX_ENTRIES];
	int i, k = 0;

	memset(tmp, 0, sizeof(tmp));
	for (i = 0; i < n; i++) {
		if (!file_open_in_buflist(entries[i].name)) {
			tmp[k++] = entries[i];
		}
	}
	for (i = 0; i < n; i++) {
		if (file_open_in_buflist(entries[i].name)) {
			tmp[k++] = entries[i];
		}
	}
	memcpy(entries, tmp, n * sizeof(*entries));
}

/* Render an ido-style "{a | b | …}" pick list into `msg` starting at
 * `*off`.  Caller has already written prompt + typed text up to `*off`.
 *
 *   - names[i]: i-th match's display name (caller's choice of order).
 *   - n:        names[] count, also the count rendered in-line.
 *   - n_total:  total match count; shown as "(+M more)" when > n.
 *   - sel:      0-based index into names[] of the selected entry.
 *
 * The window of visible entries is sized so `sel` is always present
 * even when the full list overflows the terminal width; trimmed sides
 * get "… | " / " | …" markers.
 *
 * Returns the byte offset in `msg` where the selected entry starts, for
 * editor_set_status_emphasis(), or -1 when nothing was rendered.  The
 * entry's name is a filename or a buffer name, so it is written as plain
 * text; the emphasis is the renderer's, addressed by position. */
int editor_picker_render(char *msg, int msg_size, int *off,
    const char *const *names, int n, int n_total, int sel)
{
	int budget, used, win_start, win_end, i;
	int sel_off = -1;

	if (n <= 0) {
		editor_msg_appendf(msg, msg_size, off, "[no match]");
		return -1;
	}
	if (sel < 0) {
		sel = 0;
	}
	if (sel >= n) {
		sel = n - 1;
	}

	/* Leave room for the framing markers: "{" + "}" + worst-case
	 * "… | " (4 cols) + " | …" (4 cols) = 10. */
	budget = win_total_cols - *off - 10;
	if (budget < 0) {
		budget = 0;
	}
	used = 0;
	win_start = sel;
	win_end = sel;
	for (i = sel; i < n; i++) {
		int w = (int)strlen(names[i]) + (i > sel ? 3 : 0); /* " | " */
		if (i > sel && used + w > budget) {
			break;
		}
		used += w;
		win_end = i + 1;
	}
	for (i = sel - 1; i >= 0; i--) {
		int w = (int)strlen(names[i]) + 3;
		if (used + w > budget) {
			break;
		}
		win_start = i;
		used += w;
	}

	editor_msg_appendf(msg, msg_size, off, "{");
	if (win_start > 0) {
		editor_msg_appendf(msg, msg_size, off, "… | ");
	}
	for (i = win_start; i < win_end; i++) {
		if (i > win_start) {
			editor_msg_appendf(msg, msg_size, off, " | ");
		}
		if (i == sel) {
			sel_off = *off;
		}
		editor_msg_appendf(msg, msg_size, off, "%s", names[i]);
	}
	if (win_end < n) {
		editor_msg_appendf(msg, msg_size, off, " | …");
	}
	editor_msg_appendf(msg, msg_size, off, "}");
	if (n_total > n) {
		editor_msg_appendf(
		    msg, msg_size, off, "  (+%d more)", n_total - n);
	}
	return sel_off;
}

/* Emphasise the pick list's selected entry in the message just set.
 * `sel_off` is what editor_picker_render() returned. */
void editor_picker_emphasise(
    int sel_off, const char *const *names, int n, int sel)
{
	editor_set_status_emphasis(sel_off,
	    (sel_off >= 0 && sel >= 0 && sel < n) ? (int)strlen(names[sel])
						  : 0);
}

/* Does the typed path end in a "." or ".." component?  Emacs answers such
 * a path with the directory it names rather than completing onto anything
 * inside it, which is what makes "C-x C-f . RET" open the prompt's own
 * directory; realpath() in dired_open() folds the component away. */
static int path_ends_in_dot_component(const char *buf)
{
	const char *base = buf_basename(buf);

	return strcmp(base, ".") == 0 || strcmp(base, "..") == 0;
}

static void path_prompt_redraw(const char *prompt, char *buf, int cursor,
    int *sel, struct path_entry *entries, char *dir, char *file, char *lcp,
    char *msg, int *matches, int *total, int *flen)
{
	const char *names[PICKER_MAX_ENTRIES] = { 0 };
	int off, i, sel_off;

	editor_path_split(buf, dir, 256, file, 256);
	*flen = (int)strlen(file);
	*total = editor_path_complete_entries(
	    dir, file, entries, PICKER_MAX_ENTRIES, lcp, 256);
	*matches = *total > PICKER_MAX_ENTRIES ? PICKER_MAX_ENTRIES
					       : (*total < 0 ? 0 : *total);
	if (*sel >= *matches) {
		*sel = *matches > 0 ? *matches - 1 : 0;
	}
	push_open_files_back(entries, *matches);
	for (i = 0; i < *matches; i++) {
		names[i] = entries[i].name;
	}
	off = 0;
	editor_msg_appendf(msg, 1024, &off, "%s%s ", prompt, buf);
	sel_off = editor_picker_render(
	    msg, 1024, &off, names, *matches, *total, *sel);
	editor_set_status_message("%s", msg);
	editor_picker_emphasise(sel_off, names, *matches, *sel);
	editor.echo_cursor_col
	    = prompt_cursor_col(prompt, (int)strlen(prompt), buf, cursor);
	editor_refresh_screen();
}

static void path_handle_erase(int fd, struct key_event c, char *buf,
    int bufsize, int *cursor, int *len, int *overflow, int *sel)
{
	if (*cursor < *len) {
		/* Mid-line: minibuf_edit_key has already moved the cursor
		 * back over the deleted glyph, and it must stay there.
		 * Hoisting `*cursor = *len` out of the two at-end arms below
		 * snapped it to end of line after every mid-path erase, so
		 * the next typed character landed at the far end.  Erase
		 * keys never reach the yank branches, so no record. */
		minibuf_edit_key(
		    fd, c, buf, bufsize, cursor, len, overflow, NULL);
	} else if (*overflow > 0) {
		/* Retire a refused insertion before deleting anything --
		 * exactly what minibuf_delete_backward does, and what the
		 * two arms below did not.  The count means "your answer did
		 * not fit"; backing up over the characters that did not fit
		 * has to clear it, or the next answer is refused for the
		 * previous one's overrun even though it fits. */
		(*overflow)--;
	} else if (*len > 0 && buf[*len - 1] == '/') {
		buf[--*len] = '\0';
		while (*len > 0 && buf[*len - 1] != '/') {
			buf[--*len] = '\0';
		}
		*cursor = *len;
	} else if (*len > 0) {
		*len = utf8_glyph_start_before(buf, *len, *len);
		buf[*len] = '\0';
		*cursor = *len;
	}
	*sel = 0;
}

enum path_accept_action {
	PATH_ACCEPT_DONE,
	PATH_ACCEPT_OVERFLOW,
	PATH_ACCEPT_DESCEND,
};

static enum path_accept_action path_handle_accept(char *buf, int bufsize,
    int *cursor, int *len, int flen, int matches, int sel,
    const struct path_entry *entries, int literal, int overflow)
{
	if (!literal && matches > 0 && *cursor == *len) {
		const struct path_entry *pe = &entries[sel];
		int new_name_len = (int)strlen(pe->name);
		int add_slash = pe->is_dir ? 1 : 0;

		if (*len - flen + new_name_len + add_slash + 1 > bufsize) {
			editor_set_status_message("Path too long");
			return PATH_ACCEPT_OVERFLOW;
		}
		*len -= flen;
		memcpy(buf + *len, pe->name, new_name_len);
		*len += new_name_len;
		if (add_slash) {
			buf[(*len)++] = '/';
		}
		buf[*len] = '\0';
		*cursor = *len;
		if (pe->is_dir) {
			return PATH_ACCEPT_DESCEND;
		}
	}
	/* Every overflow exit says so in the echo area, and prompt_done()
	 * keeps that message (it clears only on cancellation).  The
	 * interactive callers then act on the non-ACCEPTED result by doing
	 * nothing, so C-x C-f dismisses with a visible reason rather than
	 * either opening a truncated path -- which is what losing the
	 * counter used to do -- or raising at the user. */
	if (editor_path_expand_tilde(buf, bufsize)) {
		editor_set_status_message("Path too long");
		return PATH_ACCEPT_OVERFLOW;
	}
	if (overflow > 0) {
		editor_set_status_message("Path too long");
		return PATH_ACCEPT_OVERFLOW;
	}
	return PATH_ACCEPT_DONE;
}

static void path_handle_tab(char *buf, int bufsize, int *cursor, int *len,
    int flen, int matches, const char *lcp, const struct path_entry *entries,
    int *sel)
{
	int llen = (int)strlen(lcp);

	if (llen > flen) {
		int extend = llen - flen;
		if (*len + extend < bufsize) {
			memcpy(buf + *len, lcp + flen, extend);
			*len += extend;
			buf[*len] = '\0';
			*cursor = *len;
		}
	} else if (matches == 1 && entries[0].is_dir && *len >= 0
	    && *len < bufsize - 1 && (*len == 0 || buf[*len - 1] != '/')) {
		buf[(*len)++] = '/';
		buf[*len] = '\0';
		*cursor = *len;
	}
	*sel = 0;
}

/* Prompt for a path with ido-style completion.  Matching directory
 * entries are rendered as a "{name1 | name2 | …}" pick-list to the
 * right of the typed text, with the selected entry shown in bold.
 * Left/Right cycle the selection.  Enter on a directory descends into
 * it; Enter on a file completes the path and returns.  M-RET (and Enter
 * on a path ending in "." or "..") accepts the typed text as it stands,
 * which is how a directory is named without descending into it.  Tab
 * still extends to the longest common prefix.  Backspace at the trailing
 * '/' deletes the whole last path component, so one keystroke walks
 * you up one level. */
enum minibuf_result editor_read_line_path(
    int fd, const char *prompt, char *buf, int bufsize)
{
	struct path_entry entries[PICKER_MAX_ENTRIES];
	char dir[256], file[256], lcp[256], msg[1024];
	/* Honour any pre-populated content (callers may seed the prompt
	 * with the current buffer's directory, à la Emacs). */
	int len = (int)strnlen(buf, bufsize - 1);
	int cursor = len;
	int overflow = 0;
	struct key_event c;
	int sel = 0;
	int matches = 0, total = 0, flen = 0;
	struct minibuf_yank yank = { 0 };

	buf[len] = '\0';
	/* See editor_read_line_with_history()'s comment: same deferral,
	 * balanced the same way by prompt_done(), this function's one exit
	 * path too. */
	kg_event_prompt_enter();
	while (1) {
		path_prompt_redraw(prompt, buf, cursor, &sel, entries, dir,
		    file, lcp, msg, &matches, &total, &flen);

		c = editor_read_key(fd);
		/* Same per-keystroke kill-class boundary and yank-pop
		 * eligibility rule as editor_read_line_with_history()'s
		 * loop; this prompt's picker rendering has no message slot,
		 * so any note the yank helpers leave is dropped unshown. */
		cmd_state_prompt_keystroke();
		yank.eligible = yank.valid;
		yank.valid = 0;
		yank.note = NULL;
		if (KEY_IN_LIST(erase_keys, c)) {
			path_handle_erase(fd, c, buf, bufsize, &cursor, &len,
			    &overflow, &sel);
		} else if (KEY_IN_LIST(cancel_keys, c)) {
			return prompt_done(MINIBUF_CANCELLED);
		} else if (KEY_IS(c, 'b', KEY_MOD_CTRL)) {
			if (cursor > 0) {
				cursor
				    = utf8_glyph_start_before(buf, len, cursor);
			}
		} else if (KEY_IS(c, 'f', KEY_MOD_CTRL)) {
			if (cursor < len) {
				cursor += utf8_glyph_span_at(buf, len, cursor);
			}
			/* C-b/C-f always edit the minibuffer, including at EOL.
			 * Keep the path picker's selection cycling on the arrow
			 * keys so the two operations do not silently steal each
			 * other's bindings. */
		} else if (KEY_IS(c, KEY_BASE_LEFT, 0)
		    || KEY_IS(c, KEY_BASE_UP, 0)) {
			buf_picker_cycle(&sel, matches, -1);
		} else if (KEY_IS(c, KEY_BASE_RIGHT, 0)
		    || KEY_IS(c, KEY_BASE_DOWN, 0)) {
			buf_picker_cycle(&sel, matches, 1);
		} else if (KEY_IS(c, KEY_BASE_RET, KEY_MOD_META)
		    || (KEY_IS(c, KEY_BASE_RET, 0)
			&& path_ends_in_dot_component(buf))) {
			/* Accept the text exactly as typed: no completion
			 * applied, no descend.  M-RET is a deliberate
			 * deviation from Emacs; the "." form is Emacs'
			 * own. */
			enum path_accept_action action
			    = path_handle_accept(buf, bufsize, &cursor, &len,
				flen, matches, sel, entries, 1, overflow);
			return prompt_done(action == PATH_ACCEPT_OVERFLOW
				? MINIBUF_OVERFLOW
				: MINIBUF_ACCEPTED);
		} else if (KEY_IS(c, KEY_BASE_RET, 0)) {
			enum path_accept_action action
			    = path_handle_accept(buf, bufsize, &cursor, &len,
				flen, matches, sel, entries, 0, overflow);
			if (action == PATH_ACCEPT_DESCEND) {
				sel = 0;
				continue;
			}
			return prompt_done(action == PATH_ACCEPT_OVERFLOW
				? MINIBUF_OVERFLOW
				: MINIBUF_ACCEPTED);
		} else if (KEY_IS(c, KEY_BASE_TAB, 0) && matches > 0
		    && cursor == len) {
			path_handle_tab(buf, bufsize, &cursor, &len, flen,
			    matches, lcp, entries, &sel);
		} else {
			if (minibuf_edit_key(fd, c, buf, bufsize, &cursor, &len,
				&overflow, &yank)) {
				sel = 0;
				continue;
			}
		}
	}
}

/* Parse and apply dir-locals, modeline, and footer local settings for the
 * current buffer.  Nonfatal on parse errors.  Preserves readonly_override
 * and (when compile_command_user_override is set) compile_command. */
static void buf_apply_local_settings(void)
{
	struct local_settings dir, modeline, footer, merged;
	char dirfile[PATH_MAX];
	int fd;
	ssize_t n;
	char *data = NULL;

	if (is_special_buffer(bcur()->filename)) {
		return;
	}

	local_settings_init(&dir);
	local_settings_init(&modeline);
	local_settings_init(&footer);

	if (dirlocals_find(bcur()->filename, dirfile, sizeof(dirfile)) == 0) {
		fd = open(dirfile,
		    O_RDONLY
#ifdef O_CLOEXEC
			| O_CLOEXEC
#endif
		);
		if (fd >= 0) {
			off_t sz = lseek(fd, 0, SEEK_END);
			if (sz > 0 && sz <= 65536) {
				lseek(fd, 0, SEEK_SET);
				data = malloc((size_t)sz);
				if (data) {
					n = read(fd, data, (size_t)sz);
					if (n > 0) {
						dirlocals_parse(
						    data, (size_t)n, &dir);
					}
				}
			}
			close(fd);
		}
	}

	localvars_parse_modeline(bcur()->row, bcur()->numrows, &modeline);
	localvars_parse_footer(bcur()->row, bcur()->numrows, &footer);

	merged = dir;
	local_settings_merge(&merged, &modeline);
	local_settings_merge(&merged, &footer);

	if (merged.compile_command_set
	    && !bcur()->compile_command_user_override) {
		memcpy(bcur()->compile_command, merged.compile_command,
		    sizeof(bcur()->compile_command));
	}

	if (merged.buffer_read_only != LOCAL_BOOL_UNSET) {
		bcur()->readonly_local
		    = (merged.buffer_read_only == LOCAL_BOOL_TRUE) ? 1 : 0;
	}
	editor_refresh_readonly_state();

	free(data);
}

/* Canonical file visit: reset, load, apply local settings.
 * Caller handles slot allocation and save/restore. */
void buf_visit_file(const char *filename, int explicit_readonly)
{
	buf_reset();

	snprintf(bcur()->compile_command, sizeof(bcur()->compile_command),
	    "make -k");
	bcur()->compile_command_user_override = 0;

	editor_select_syntax_highlight(bcur(), (char *)filename);
	editor_open((char *)filename);

	buf_apply_local_settings();

	if (explicit_readonly) {
		bcur()->readonly_override = 1;
		editor_refresh_readonly_state();
	}

	bcur()->dirty = 0;
	undo_mark_clean();
}

/* Load all command-line files into the buffer list, then start in buffer 0.
 * Called once from main() after init_editor().
 * Arguments of the form +LINE or +LINE:COL position the next file. */
/* Load one command-line file into `slot`, honoring a pending +LINE[:COL]
 * positioner (cleared either way, matching the caller's prior inline
 * behaviour).  Returns 0 without touching the slot when buf_select()
 * refused it under event-queue pressure -- buf_load_args() stops there
 * rather than opening the remaining files out of a slot sequence that has
 * started skipping entries. */
static int buf_load_one_arg(int slot, const char *filename, int readonly,
    int *pending_line, int *pending_col)
{
	if (!buf_select(slot)) {
		return 0;
	}
	buf_visit_file(filename, readonly);
	if (*pending_line > 0) {
		editor_goto_line_direct(*pending_line, *pending_col);
		*pending_line = 0;
		*pending_col = 1;
	}
	return 1;
}

void buf_load_args(int nfiles, char **filenames, int readonly)
{
	int i, slot = 0;
	int pending_line = 0, pending_col = 1;

	memset(buflist, 0, sizeof(buflist));
	buf_current = 0;
	buf_count = 0;

	if (nfiles == 0) {
		/* No files given: open an empty *scratch* buffer.  buf_select()
		 * below is what gives it its BUFFER_OPENED event, since it is
		 * the one place a slot's identity is claimed. */
		buf_current = 0;
		buf_reset();
		bcur()->filename = strdup("*scratch*");
		bcur()->syntax = &lisp_interaction_syntax;
		buf_count = 1;
		buf_select(0);
		return;
	}

	for (i = 0; i < nfiles && slot < MAX_BUFFERS; i++) {
		if (filenames[i][0] == '+') {
			/* Position specifier: +LINE or +LINE:COL */
			pending_line = 0;
			pending_col = 1;
			sscanf(filenames[i] + 1, "%d:%d", &pending_line,
			    &pending_col);
			continue;
		}
		/* The window follows the slot being filled, so a +LINE lands
		 * on that file's own view and is banked there when the next
		 * file takes the window. */
		if (!buf_load_one_arg(slot, filenames[i], readonly,
			&pending_line, &pending_col)) {
			break;
		}
		buf_count++;
		slot++;
	}
	buf_select(0);
}

/* The candidate buffer names buf_select_interactive() has cached for this
 * redraw, shaped so editor_picker_filter() can walk them. */
struct bufpick_names {
	char (*name)[128];
	int n;
};

static void buf_picker_cycle(int *selection, int matches, int direction)
{
	if (matches > 0) {
		*selection = (*selection + direction + matches) % matches;
	}
}

static const char *bufpick_name_at(int index, void *data)
{
	const struct bufpick_names *cand = data;

	return index < cand->n ? cand->name[index] : NULL;
}

enum buf_name_action {
	BUF_NAME_CONTINUE,
	BUF_NAME_ACCEPTED,
	BUF_NAME_OVERFLOW,
	/* BUF_NAME_SELECT only: RET on a query that matches nothing closes
	 * the prompt having chosen nothing, which is what C-x b has always
	 * done.  Distinct from ACCEPTED so no caller can mistake the empty
	 * answer for a name. */
	BUF_NAME_DISMISSED,
};

/* One redraw's filtered view of the buffer ring, as the accept path needs
 * it.  `ring_pos` maps a filtered index back to its place in `order`, and
 * therefore to a buflist index -- which is what makes an exact answer
 * possible where re-scanning for the returned display name is not. */
struct buf_name_view {
	int n;
	int matches;
	int sel;
	const int *order;
	const int *ring_pos;
	const char *const *names;
};

static enum buf_name_action buf_name_accept_query(
    const char *query, char *out, int outsize)
{
	if ((int)strlen(query) >= outsize) {
		return BUF_NAME_OVERFLOW;
	}
	strcpy(out, query);
	return BUF_NAME_ACCEPTED;
}

/* Answer with the display name of buflist entry `idx`, and report the
 * index itself. */
static enum buf_name_action buf_name_take(
    int idx, char *out, int outsize, int *picked)
{
	char display[128];

	buf_display_name(idx, display, sizeof(display));
	if ((int)strlen(display) >= outsize) {
		return BUF_NAME_OVERFLOW;
	}
	strcpy(out, display);
	*picked = idx;
	return BUF_NAME_ACCEPTED;
}

static enum buf_name_action buf_name_accept(struct key_event c,
    const char *query, const struct buf_name_view *view, char *out, int outsize,
    enum buf_name_mode mode, int overflow, int *picked)
{
	const int meta_ret = KEY_IS(c, KEY_BASE_RET, KEY_MOD_META);

	editor.echo_cursor_col = 0;
	editor_set_status_message("");
	*picked = -1;
	if (overflow > 0) {
		return BUF_NAME_OVERFLOW;
	}
	/* M-RET is `B`'s explicit-literal escape and nothing else's.  In the
	 * other two modes it is not an accept key at all -- C-x b ignored it
	 * before this picker was shared, and must go on ignoring it. */
	if (meta_ret && mode != BUF_NAME_ANY) {
		return BUF_NAME_CONTINUE;
	}
	if (query[0] == '\0' && mode == BUF_NAME_EXISTING) {
		return buf_name_take(buf_current, out, outsize, picked);
	}
	if (meta_ret && query[0] != '\0') {
		return buf_name_accept_query(query, out, outsize);
	}
	if (view->matches > 0) {
		return buf_name_take(view->order[view->ring_pos[view->sel]],
		    out, outsize, picked);
	}
	if (query[0] == '\0') {
		/* The ring is built from buf_current + 1 through
		 * buf_current itself, so order[0] is the first *other*
		 * buffer when one exists and buf_current when none does --
		 * exactly 07E's default for `B`, with no separate fallback
		 * to reach.  n is never 0 for the same reason. */
		return buf_name_take(view->order[0], out, outsize, picked);
	}
	if (mode == BUF_NAME_ANY) {
		return buf_name_accept_query(query, out, outsize);
	}
	/* A query naming no buffer: `b` re-prompts, which is the Lisp code's
	 * contract; C-x b closes, which is what it always did.  Answering
	 * CONTINUE for both left C-x b re-prompting forever. */
	return mode == BUF_NAME_EXISTING ? BUF_NAME_CONTINUE
					 : BUF_NAME_DISMISSED;
}

static int buf_name_insert_key(
    int fd, struct key_event c, char *query, int *qlen, int *overflow, int *sel)
{
	if (c.mods == 0 && ascii_is_print(c.base)) {
		if (*qlen < BUF_NAME_QUERY_MAX - 1) {
			query[(*qlen)++] = (char)c.base;
			query[*qlen] = '\0';
		} else {
			(*overflow)++;
		}
		*sel = 0;
		return 1;
	}
	if (c.mods == 0 && c.base >= 0x80 && c.base <= 0xFF) {
		char seq[4];
		int n = editor_read_utf8_seq(fd, (int)c.base, seq);

		if (n > 0 && *qlen + n < BUF_NAME_QUERY_MAX - 1) {
			memcpy(query + *qlen, seq, (size_t)n);
			*qlen += n;
			query[*qlen] = '\0';
			*sel = 0;
		} else if (n > 0) {
			(*overflow)++;
		}
		return 1;
	}
	return 0;
}

static int buf_name_handle_key(int fd, struct key_event c, char *query,
    int *qlen, int *overflow, int *sel, const struct buf_name_view *view,
    char *out, int outsize, enum buf_name_mode mode, int *picked)
{
	if (KEY_IN_LIST(erase_keys, c)) {
		/* Retire a refused keystroke before deleting a real one, as
		 * minibuf_delete_backward does.  Nothing cleared this count
		 * before, so one overlong query made the picker permanently
		 * inert: every later answer, however short, was OVERFLOW. */
		if (*overflow > 0) {
			(*overflow)--;
		} else if (*qlen > 0) {
			*qlen = utf8_glyph_start_before(query, *qlen, *qlen);
			query[*qlen] = '\0';
		}
		*sel = 0;
		return -2;
	}
	if (KEY_IN_LIST(picker_next_keys, c)) {
		buf_picker_cycle(sel, view->matches, 1);
		return -2;
	}
	if (KEY_IN_LIST(picker_prev_keys, c)) {
		buf_picker_cycle(sel, view->matches, -1);
		return -2;
	}
	if (KEY_IS(c, KEY_BASE_RET, KEY_MOD_META)
	    || KEY_IS(c, KEY_BASE_RET, 0)) {
		enum buf_name_action action = buf_name_accept(
		    c, query, view, out, outsize, mode, *overflow, picked);

		if (action == BUF_NAME_CONTINUE) {
			return -2;
		}
		if (action == BUF_NAME_OVERFLOW) {
			return MINIBUF_OVERFLOW;
		}
		if (action == BUF_NAME_DISMISSED) {
			out[0] = '\0';
			return MINIBUF_CANCELLED;
		}
		return MINIBUF_ACCEPTED;
	}
	if (KEY_IN_LIST(cancel_keys, c)) {
		editor.echo_cursor_col = 0;
		editor_set_status_message("");
		return MINIBUF_CANCELLED;
	}
	(void)buf_name_insert_key(fd, c, query, qlen, overflow, sel);
	return -2;
}

/* Read a buffer name with the same picker used by C-x b.  This deliberately
 * only returns text: selecting or creating the named buffer belongs to the
 * eventual command body, not argument construction. */
enum minibuf_result buf_read_name(int fd, const char *prompt, char *out,
    int outsize, enum buf_name_mode mode, int *picked)
{
	const int plen = (int)strlen(prompt);
	int order[MAX_BUFFERS], n = 0;
	char query[BUF_NAME_QUERY_MAX];
	int qlen = 0, sel = 0;
	int overflow = 0;
	int discard = -1;
	int i;
	struct key_event c;
	char msg[512];
	int off, sel_off;

	if (picked == NULL) {
		picked = &discard;
	}
	*picked = -1;
	query[0] = '\0';
	if (out == NULL || outsize < 2) {
		return MINIBUF_OVERFLOW;
	}

	/* See editor_read_line_with_history()'s comment: this is a
	 * minibuffer read too (its query line), across a loop that may run
	 * for many keystrokes before Enter or C-g.  Balanced at the loop's
	 * one exit, since this function does not funnel through
	 * prompt_done(); the refusal above returns before the pair opens.
	 * ("No other buffers." used to be an early return here and is
	 * buf_select_interactive's now, where it emits its own pair.) */
	kg_event_prompt_enter();

	/* Build ring starting from the buffer after current (most natural
	 * default). */
	for (i = 1; i <= MAX_BUFFERS; i++) {
		int idx = (buf_current + i) % MAX_BUFFERS;
		if (buflist[idx].active) {
			order[n++] = idx;
		}
	}
	{
		static char namebuf[MAX_BUFFERS][128];
		const char *names[MAX_BUFFERS];
		int ring_pos[MAX_BUFFERS];
		struct bufpick_names cand = { namebuf, n };

		while (1) {
			int matches, nprefix;

			/* Cache every candidate's display name once per redraw,
			 * then build the filtered view as prefix matches
			 * followed by mid-name matches. */
			for (i = 0; i < n; i++) {
				buf_display_name(
				    order[i], namebuf[i], sizeof(namebuf[i]));
			}

			matches = editor_picker_filter(bufpick_name_at, &cand,
			    query, names, ring_pos, MAX_BUFFERS, &nprefix);
			if (sel >= matches) {
				sel = matches > 0 ? matches - 1 : 0;
			}

			off = 0;
			editor_msg_appendf(
			    msg, sizeof(msg), &off, "%s%s ", prompt, query);
			sel_off = editor_picker_render(msg, sizeof(msg), &off,
			    names, matches, matches, sel);
			editor_set_status_message("%s", msg);
			editor_picker_emphasise(sel_off, names, matches, sel);
			editor.echo_cursor_col
			    = prompt_cursor_col(prompt, plen, query, qlen);
			editor_refresh_screen();

			c = editor_read_key(fd);
			struct buf_name_view view
			    = { n, matches, sel, order, ring_pos, names };
			int result = buf_name_handle_key(fd, c, query, &qlen,
			    &overflow, &sel, &view, out, outsize, mode, picked);
			if (result != -2) {
				kg_event_prompt_leave();
				return (enum minibuf_result)result;
			}
		}
	}
}

/* Interactive buffer selector shown in the echo area (C-x b). */
void buf_select_interactive(int fd)
{
	char name[128];
	int other = 0;
	int picked = -1;

	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (i != buf_current && buflist[i].active) {
			other = 1;
			break;
		}
	}
	if (!other) {
		/* Structurally this prompt starting and immediately
		 * finishing, so it is still one balanced pair -- which the
		 * early return had stopped emitting when the read moved into
		 * buf_read_name(). */
		kg_event_prompt_enter();
		editor_set_status_message("No other buffers.");
		kg_event_prompt_leave();
		return;
	}

	/* Select by the index the picker reports, not by re-scanning for the
	 * returned text: display names are disambiguated by one parent
	 * directory and can still collide, and the first match then wins
	 * over the entry the user actually highlighted. */
	if (buf_read_name(
		fd, "Buffer: ", name, sizeof(name), BUF_NAME_SELECT, &picked)
		== MINIBUF_ACCEPTED
	    && picked >= 0) {
		(void)buf_select(picked);
	}
}

/* buf_open_path()'s tail once a free slot has been picked: select it --
 * which is where a fresh identity gets its BUFFER_OPENED event -- then
 * visit the file.  A refused selection under event-queue pressure leaves
 * the slot untouched and already reported (buf_select() sets the status
 * message), so there is nothing left to do here. */
static void buf_open_path_new(int slot, const char *path, int readonly)
{
	if (!buf_select(slot)) {
		return;
	}
	buf_visit_file(path, readonly);
	buf_count++;
	editor_set_status_message("%s%s",
	    bcur()->filename ? bcur()->filename : "[new]",
	    readonly ? " [read-only]" : "");
}

/* Open `path` in a new buffer, or switch to the buffer already visiting
 * it.  This is C-x C-f minus the prompt, so anything that names a file
 * some other way (dired's RET) lands in exactly the same state.
 * readonly: if 1, mark the buffer read-only after loading. */
void buf_open_path(const char *path, int readonly)
{
	int slot;

	/* A directory lists.  dired_open() finds or allocates the listing's
	 * own slot — the same one M-x dired would reuse — so it must not be
	 * run inside the slot dance below. */
	if (path_is_dir(path)) {
		(void)dired_open(path);
		return;
	}

	/* Switch to existing buffer if the file is already open. */
	slot = buf_find_by_name(path);
	if (slot >= 0) {
		buf_select(slot);
		if (readonly) {
			bcur()->readonly_override = 1;
			editor_refresh_readonly_state();
		}
		editor_set_status_message("%s", bcur()->filename);
		return;
	}

	if (buf_count >= MAX_BUFFERS) {
		editor_set_status_message(
		    "Too many open buffers (%d max).", MAX_BUFFERS);
		return;
	}

	slot = buf_first_free_slot();
	if (slot < 0) {
		return; /* should not happen given buf_count check above */
	}

	/* Show the slot before filling it.  buf_reset() empties the buffer
	 * *and* the view of the window showing it, so until the window has
	 * moved, that window is still the outgoing buffer's -- and the move
	 * would then bank the emptied view as where that buffer was left. */
	buf_open_path_new(slot, path, readonly);
}

/* Prompt for a filename and open it (C-x C-f, C-x C-r). */
static void buf_open_file_ro(int fd, int readonly)
{
	char query[256];
	const char *prompt = readonly ? "Open file read-only: " : "Open file: ";

	editor_prompt_prefill_dir(query, sizeof(query));
	if (editor_read_line_path(fd, prompt, query, sizeof(query))
		!= MINIBUF_ACCEPTED
	    || query[0] == '\0') {
		return;
	}
	buf_open_path(query, readonly);
}

void buf_open_file(int fd) { buf_open_file_ro(fd, 0); }
void buf_open_file_read_only(int fd) { buf_open_file_ro(fd, 1); }

/* Write a buffer slot's rows directly to its file without switching to it.
 * Returns 0 on success, 1 on error (errno set).  The accepted snapshot
 * guards the rename, so a file that changes between the user's answer and
 * the commit is refused rather than overwritten. */
static int write_slot(struct editor_buffer *b)
{
	return editor_write_rows_to_file(
	    b->filename, b->row, b->numrows, NULL, &b->disk);
}

/* Save all modified non-special buffers, prompting for each (C-x s). */
void buf_save_all(int fd)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];

		if (!b->active || !b->dirty) {
			continue;
		}
		if (is_special_buffer(b->filename)) {
			continue;
		}

		if (!editor_confirm_yn(fd, "Save %s? (y/n) ", b->filename)) {
			continue;
		}
		if (!confirm_save_over_accepted(fd, b->filename, &b->disk)) {
			editor_set_status_message("%s not saved", b->filename);
			continue;
		}

		if (write_slot(b) == 0) {
			b->dirty = 0;
			b->disk_changed = 0;
			(void)file_snapshot_path(b->filename, &b->disk);
			if (i == buf_current) {
				bcur()->dirty = 0;
				bcur()->disk_changed = 0;
				bcur()->disk = b->disk;
				undo_mark_clean();
			}
			editor_set_status_message("Wrote %s", b->filename);
		} else {
			editor_set_status_message("Error writing %s: %s",
			    b->filename, strerror(errno));
		}
	}
}

/* Kill (close) the current buffer, prompting if modified.
 *
 * The order is the point.  The dying handle is taken while it still
 * resolves, every view on it is detached before the generation bump kills
 * the handle, and only then is the slot freed -- so no window is ever
 * active holding a handle that has stopped resolving, not even for the
 * span of this function. */
/* The destructive part of buf_kill(), for `slot` (== buf_current, still
 * bcur()) with the confirmation prompts already answered: detach every
 * view on the dying buffer, then free its resources and mark the slot
 * dead.  Brackets that with its two lifecycle events, each reserved right
 * at the point its own transition happens rather than both up front:
 *
 *   - KG_EVENT_BUFFER_KILLING is reserved and published first, while
 *     `dying` still resolves.  A refused reservation refuses the whole
 *     kill here -- nothing has been touched yet, so bcur() is still
 *     exactly what it was.
 *   - KG_EVENT_BUFFER_KILLED is reserved after cleanup, when the buffer is
 *     already gone.  There is no "old state" left to refuse back to at
 *     that point -- a kill already past its point of no return -- so a
 *     lost reservation there can only cost the event, not the kill;
 *     that is the one refusal this producer cannot make.
 *
 * Returns 0 when the KILLING reservation was refused (nothing done), 1
 * once the buffer is gone. */
static int buf_kill_commit(int slot, struct kg_buffer_handle dying)
{
	struct kg_event_reservation res = kg_event_reserve_lifecycle();
	int i;

	if (!res.valid) {
		editor_set_status_message(
		    "Too many pending events; buffer not killed.");
		return 0;
	}
	kg_event_publish_lifecycle(&res, kg_event_make_buffer_killing(dying));

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (win_shows_buffer(&winlist[i], dying)) {
			buf_detach_view(&winlist[i]);
		}
	}

	/* Free the buffer's memory and leave the slot describing nothing:
	 * a row count that outlives the rows is what the next occupant's
	 * reset would walk. */
	editor_free_all_rows(&buflist[slot]);
	undo_stack_free(&buflist[slot].undostack);
	kg_decor_store_free(&buflist[slot]);
	kg_marker_store_free(&buflist[slot]);
	free(buflist[slot].filename);

	buflist[slot].active = 0;
	buflist[slot].filename = NULL;
	buflist[slot].dirty = 0;
	buflist[slot].syntax = NULL;
	/* Every handle taken on this buffer stops resolving here. */
	buflist[slot].generation++;

	res = kg_event_reserve_lifecycle();
	if (res.valid) {
		kg_event_publish_lifecycle(
		    &res, kg_event_make_buffer_killed(dying));
	}
	return 1;
}

void buf_kill(int fd)
{
	struct kg_buffer_handle dying;
	int killed = buf_current;
	int i;

	if (bcur()->filename && strcmp(bcur()->filename, "*compilation*") == 0
	    && compilation_is_running()) {
		if (!editor_confirm_yn(
			fd, "Compilation is still running; kill it? (y/n) ")) {
			editor_set_status_message("Buffer not killed");
			return;
		}
		compilation_shutdown();
	}

	if (bcur()->dirty
	    && !editor_confirm_yn(fd, "Buffer modified, really kill? (y/n) ")) {
		editor_set_status_message("");
		return;
	}

	dying = buf_handle(killed);
	if (!buf_kill_commit(killed, dying)) {
		return;
	}
	buf_count--;

	if (buf_count == 0) {
		running = 0;
		return;
	}

	/* Switch to the nearest remaining buffer. */
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active) {
			buf_select(i);
			break;
		}
	}
	/* Every window the detach left showing nothing follows.  A view is
	 * never left naming a slot it does not own: it would resurrect the
	 * slot as an empty buffer the count knows nothing about, or start
	 * showing whichever file took the slot next. */
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (winlist[i].active && !win_buffer(&winlist[i])) {
			buf_attach_view(&winlist[i], buf_current);
		}
	}
	editor_set_status_message(
	    "%s", bcur()->filename ? bcur()->filename : "[new]");
}

/* Kill the buffer `handle` names without prompting: refused (returns 0)
 * when the buffer is modified and unsaved, when it is the last live
 * buffer, or when the event queue has no room.  Detaches every window
 * showing it, frees its resources, and re-homes the current buffer and
 * every window the detach left showing nothing, exactly as the
 * interactive buf_kill() does after its prompt -- the differences are
 * that nothing prompts and the editor never exits when the count hits
 * zero (the last-buffer refusal above is what keeps that from
 * happening).  A handle that no longer resolves returns 0.  This is
 * what Lisp's (kill-buffer) reaches. */
int buf_kill_buffer(struct kg_buffer_handle handle)
{
	int slot = buf_handle_slot(handle);
	struct editor_buffer *b = buf_resolve(handle);
	int i;

	if (!b || slot < 0) {
		return 0;
	}
	if (b->dirty) {
		return 0;
	}
	if (buf_count <= 1) {
		return 0;
	}
	if (!buf_kill_commit(slot, handle)) {
		return 0;
	}
	buf_count--;

	/* If the current buffer died, select the nearest remaining one,
	 * which also re-attaches the current window; then every window the
	 * detach left showing nothing follows it, as buf_kill() does. */
	if (!buflist[buf_current].active) {
		for (i = 0; i < MAX_BUFFERS; i++) {
			if (buflist[i].active) {
				buf_select(i);
				break;
			}
		}
	}
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (winlist[i].active && !win_buffer(&winlist[i])) {
			buf_attach_view(&winlist[i], buf_current);
		}
	}
	return 1;
}

/* buf_enter_special()'s "not already open" tail: select the free slot --
 * which is where its BUFFER_OPENED event comes from -- then reset it.
 * Returns -1, bcur() left wherever it already was, when buf_select()
 * refused the slot under event-queue pressure (already reported: it sets
 * the status message itself). */
static int buf_enter_special_new(int slot, const char *name)
{
	if (!buf_select(slot)) {
		return -1;
	}
	buf_reset();
	bcur()->filename = strdup(name);
	return slot;
}

/* Make the special buffer named `name` current so its content can be
 * rebuilt: the outgoing state is saved, and the buffer's own slot is
 * reused when it is already open, a free one taken otherwise.  Returns
 * that slot, or -1 after posting a message.  `*existing` reports whether
 * the buffer was already open, which buf_commit_special() needs.  The
 * rows are the caller's to clear; everything else is reset here. */
static int buf_enter_special(const char *name, int *existing)
{
	int slot;

	*existing = buf_find_by_name(name);
	slot = buf_first_free_slot();

	if (*existing >= 0) {
		buf_select(*existing);
		/* Content is rebuilt from scratch; drop stale ops, freeing
		 * their text first so repeated refreshes don't leak. */
		undo_free();
		undo_init();
		return *existing;
	}
	if (buf_count >= MAX_BUFFERS) {
		editor_set_status_message(
		    "Too many open buffers (%d max).", MAX_BUFFERS);
		return -1;
	}
	if (slot < 0) {
		return -1;
	}
	return buf_enter_special_new(slot, name);
}

/* Land the rebuilt special buffer in the slot buf_enter_special() picked,
 * counting it as newly opened when it was not already there. */
static void buf_commit_special(int slot, int existing)
{
	if (existing < 0) {
		buf_select(slot);
		buf_count++;
	}
}

/* Set up the special buffer named `name`: take its slot, run `populate`
 * to build rows off to the side, adopt them as the buffer's whole content,
 * then mark the buffer read-only, attach `syn`, and post `status`.  Shared
 * by buf_open_list, buf_open_help and dired.
 *
 * `populate` builds into a staged row array rather than bcur() directly
 * (edit.h's row builder): a rebuild that ran out of memory partway used to
 * leave the buffer's prior content already gone (editor_free_all_rows()
 * ran first) and the new listing half-written; staging first means a
 * failure here leaves whatever the buffer showed before untouched. */
void buf_open_special(const char *name, struct editor_syntax *syn,
    void (*populate)(erow **rows, int *numrows, int *row_capacity),
    const char *status)
{
	int existing;
	int slot = buf_enter_special(name, &existing);
	erow *rows = NULL;
	int numrows = 0, row_capacity = 0;

	if (slot < 0) {
		return;
	}

	populate(&rows, &numrows, &row_capacity);
	if (kg_row_builder_highlight(rows, numrows, syn) != 0) {
		kg_row_builder_free(&rows, &numrows, &row_capacity);
		return;
	}
	kg_buffer_adopt_rows(bcur(), &rows, &numrows, &row_capacity);

	wcur()->cx = wcur()->cy = wcur()->rowoff = wcur()->coloff = 0;
	bcur()->readonly_override = 1;
	editor_refresh_readonly_state();
	bcur()->syntax = syn;

	buf_commit_special(slot, existing);

	editor_set_status_message("%s", status);
}

static void buf_reset_slot(int slot)
{
	struct editor_buffer *b = &buflist[slot];
	uint64_t generation = b->generation;

	kg_decor_store_free(b);
	kg_marker_store_free(b);
	memset(b, 0, sizeof(*b));
	b->generation
	    = generation; /* Handovers accumulate; they don't reset. */
	buf_claim_slot(slot);
	b->readonly_override = -1;
	undo_stack_init(&b->undostack);
	b->active = 1;
}

/* Create a new empty buffer named `name` without selecting it or touching
 * any window: the backend of Lisp's (get-buffer-create).  Returns a handle
 * naming nothing when the name is empty, the table is full, or the event
 * queue refused the open.  The new buffer is clean and carries the syntax
 * its name selects, but no file. */
struct kg_buffer_handle buf_create_named(const char *name)
{
	struct kg_buffer_handle zeroed = { -1, 0, 0 };
	struct kg_event_reservation res;
	int slot;

	if (!name || !name[0] || buf_count >= MAX_BUFFERS) {
		return zeroed;
	}
	slot = buf_first_free_slot();
	if (slot < 0) {
		return zeroed;
	}
	/* buf_prepare_special_new()'s shape: reserve, reset (which claims
	 * the identity), publish, name. */
	res = kg_event_reserve_lifecycle();
	if (!res.valid) {
		return zeroed;
	}
	buf_reset_slot(slot);
	kg_event_publish_lifecycle(
	    &res, kg_event_make_buffer_opened(buf_handle(slot)));
	buflist[slot].filename = strdup(name);
	if (!buflist[slot].filename) {
		buflist[slot].active = 0;
		return zeroed;
	}
	editor_select_syntax_highlight(&buflist[slot], (char *)name);
	buflist[slot].dirty = 0;
	buf_count++;
	return buf_handle(slot);
}

/* buf_prepare_special_text()'s "not already open" path: reserve capacity
 * for the BUFFER_OPENED event, reset the free slot -- which is where
 * buf_reset_slot() claims it -- publish, then give it `name`.  Returns
 * MAX_BUFFERS, out of buf_prepare_special_text()'s valid slot range and so
 * caught by its own "target_slot >= MAX_BUFFERS" guard, when the
 * reservation was refused; the slot is left untouched. */
static int buf_prepare_special_new(int slot, const char *name)
{
	struct kg_event_reservation res = kg_event_reserve_lifecycle();

	if (!res.valid) {
		return MAX_BUFFERS;
	}
	buf_reset_slot(slot);
	kg_event_publish_lifecycle(
	    &res, kg_event_make_buffer_opened(buf_handle(slot)));
	buflist[slot].filename = strdup(name);
	buf_count++;
	return slot;
}

int buf_prepare_special_text(
    const char *name, struct editor_syntax *syntax, int readonly)
{
	int target_slot = buf_find_by_name(name);

	if (target_slot >= 0) {
		buf_clear_special_text(target_slot);
	} else {
		int slot = buf_first_free_slot();

		if (buf_count >= MAX_BUFFERS) {
			editor_set_status_message(
			    "Too many open buffers (%d max).", MAX_BUFFERS);
			return -1;
		}
		if (slot < 0) {
			return -1;
		}
		target_slot = buf_prepare_special_new(slot, name);
	}
	if (target_slot >= MAX_BUFFERS) {
		/* Both sources are in-range slots when the slot was claimed;
		 * a full event queue is what makes buf_prepare_special_new()
		 * hand back MAX_BUFFERS instead, so this is the one message
		 * this guard needs to report now that it is reachable. */
		editor_set_status_message(
		    "Too many pending events; buffer not opened.");
		return -1;
	}

	buflist[target_slot].syntax = syntax;
	buflist[target_slot].readonly_override = readonly ? 1 : -1;
	buflist[target_slot].readonly = readonly;

	return target_slot;
}

int buf_append_special_text(
    int buffer_index, const char *text, size_t text_length)
{
	struct kg_buffer_handle target = buf_handle(buffer_index);
	struct editor_buffer *b = buf_resolve(target);

	if (!b) {
		return -1;
	}
	if (text_length == 0) {
		return 0;
	}

	bool follow_viewport[MAX_WINDOWS] = { false };
	bool follow_cursor[MAX_WINDOWS] = { false };
	int old_numrows = b->numrows;

	for (int w_idx = 0; w_idx < MAX_WINDOWS; w_idx++) {
		struct editor_window *w = &winlist[w_idx];
		if (w->active && win_shows_buffer(w, target)) {
			int h = w->h;
			int rowoff = w->rowoff;
			int cursor_row = w->rowoff + w->cy;

			if (old_numrows <= 0 || rowoff + h >= old_numrows) {
				follow_viewport[w_idx] = true;
			}
			if (old_numrows <= 0 || cursor_row >= old_numrows - 1) {
				follow_cursor[w_idx] = true;
			}
		}
	}

	kg_buffer_append_internal(b, text, text_length);

	int new_numrows = b->numrows;
	for (int w_idx = 0; w_idx < MAX_WINDOWS; w_idx++) {
		struct editor_window *w = &winlist[w_idx];
		if (w->active && win_shows_buffer(w, target)) {
			int h = w->h;
			int rowoff = w->rowoff;
			int cursor_row = w->rowoff + w->cy;

			if (follow_viewport[w_idx]) {
				rowoff
				    = (new_numrows > h) ? (new_numrows - h) : 0;
			} else {
				if (rowoff > new_numrows - h) {
					rowoff = new_numrows - h;
				}
				if (rowoff < 0) {
					rowoff = 0;
				}
			}

			if (follow_cursor[w_idx]) {
				cursor_row
				    = (new_numrows > 0) ? (new_numrows - 1) : 0;
			} else {
				if (cursor_row >= new_numrows) {
					cursor_row = new_numrows - 1;
				}
				if (cursor_row < 0) {
					cursor_row = 0;
				}
			}

			w->rowoff = rowoff;
			w->cy = cursor_row - rowoff;
			w->cx = 0;
		}
	}

	return 0;
}

void buf_clear_special_text(int buffer_index)
{
	if (buffer_index < 0 || buffer_index >= MAX_BUFFERS
	    || !buflist[buffer_index].active) {
		return;
	}

	struct editor_buffer *b = &buflist[buffer_index];
	erow *rows = NULL;
	int numrows = 0, row_capacity = 0;

	kg_buffer_adopt_rows(b, &rows, &numrows, &row_capacity);

	undo_stack_free(&b->undostack);
	undo_stack_init(&b->undostack);
}

void buf_truncate_last_row(int buffer_index, size_t len_to_remove)
{
	if (buffer_index < 0 || buffer_index >= MAX_BUFFERS
	    || !buflist[buffer_index].active) {
		return;
	}
	if (len_to_remove == 0) {
		return;
	}

	struct editor_buffer *b = &buflist[buffer_index];
	size_t total = buffer_byte_length(b);

	if (len_to_remove > total) {
		return;
	}

	struct kg_edit e
	    = kg_edit_internal(b, total - len_to_remove, total, "", 0);
	(void)kg_buffer_replace(&e, NULL);
}

/* Stage the *Buffer List* rows from the buflist[] snapshot. */
static void buf_list_populate(erow **rows, int *numrows, int *row_capacity)
{
	char line[256];
	int i, j, len, size;
	const char *modename;

	len = snprintf(line, sizeof(line), " M  %-24s  %6s  %-14s  %s",
	    "Buffer", "Size", "Mode", "File");
	kg_row_builder_add_line(rows, numrows, row_capacity, line, (size_t)len);
	len = snprintf(line, sizeof(line), " -  %-24s  %6s  %-14s  %s",
	    "------", "------", "----", "----");
	kg_row_builder_add_line(rows, numrows, row_capacity, line, (size_t)len);

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];
		if (!b->active) {
			continue;
		}

		modename = b->syntax ? b->syntax->name : "Fundamental";
		size = 0;
		for (j = 0; j < b->numrows; j++) {
			size += b->row[j].size;
		}

		len = snprintf(line, sizeof(line), " %c  %-24s  %6d  %-14s  %s",
		    b->dirty ? '*' : ' ', buf_basename(b->filename), size,
		    modename, b->filename ? b->filename : "");
		kg_row_builder_add_line(
		    rows, numrows, row_capacity, line, (size_t)len);
	}
}

/* Open (or refresh) a *Buffer List* buffer in the current window (C-x C-b).
 * Lists all open buffers with modification flag, name, size, mode, and path.
 * Press q or C-x k to close. */
void buf_open_list(void)
{
	buf_open_special(IBUF_NAME, &ibuffer_syntax, buf_list_populate,
	    "Buffer list — RET to open, q or C-x k to close.");
}

/* Stage the *help* buffer rows from the static key-binding table. */
static void buf_help_populate(erow **rows, int *numrows, int *row_capacity)
{
	int i;

	for (i = 0; kg_help_lines[i]; i++) {
		kg_row_builder_add_line(rows, numrows, row_capacity,
		    kg_help_lines[i], strlen(kg_help_lines[i]));
	}
}

/* Open (or refresh) the *help* buffer in the current window (C-h).
 * Reuses the regular buffer machinery so scrolling, mode line, and
 * 'q' to close all come for free. */
void buf_open_help(void)
{
	buf_open_special(HELP_NAME, &text_syntax, buf_help_populate,
	    "Press q to exit help.");
}

/* Open the buffer named on the current IBuffer line.
 * Line format: " M  <24-char name>  <6-char size>  <14-char mode>  <filename>"
 * The full filename starts at byte offset 54. */
void buf_ibuffer_select(void)
{
	int filerow = wcur()->rowoff + wcur()->cy;
	const char *filename;
	int i;

	if (bcur()->syntax != &ibuffer_syntax) {
		return; /* only valid in IBuffer mode */
	}
	if (filerow < 2 || filerow >= bcur()->numrows) {
		return; /* skip header rows */
	}
	if (bcur()->row[filerow].size <= IBUF_FILENAME_OFFSET) {
		return;
	}

	filename = bcur()->row[filerow].chars + IBUF_FILENAME_OFFSET;
	if (!filename[0]) {
		return;
	}
	if (strcmp(filename, IBUF_NAME) == 0) {
		return; /* don't recurse */
	}

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active || !buflist[i].filename) {
			continue;
		}
		if (strcmp(buflist[i].filename, filename) == 0) {
			buf_select(i);
			editor_set_status_message("%s",
			    bcur()->filename ? bcur()->filename : "[new]");
			return;
		}
	}
	editor_set_status_message("Buffer not found: %s", filename);
}

void editor_cleanup(void)
{
	static int cleaned_up;

	if (cleaned_up) {
		return;
	}
	cleaned_up = 1;
	compilation_shutdown();
	kg_process_table_shutdown();

	/* Every window may own a visual-line geometry index (src/vgeom.h);
	 * freeing it here is what the "session teardown" leg of its
	 * lifetime rule means for a window that is never explicitly
	 * detached before the process exits. */
	window_vgeom_reset_all();

	/* Every slot owns its rows, filename, undo chain, marker store and
	 * decoration store, and no copy of any of them lives elsewhere, so
	 * one table pass is the whole teardown. */
	for (int i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];

		if (!b->active) {
			kg_decor_store_free(b);
			kg_marker_store_free(b);
			continue;
		}
		editor_free_all_rows(b);
		free(b->filename);
		b->filename = NULL;
		undo_stack_free(&b->undostack);
		kg_decor_store_free(b);
		kg_marker_store_free(b);
		b->active = 0;
	}
	bcur()->filename = NULL;
	kill_ring_free();
	rect_kill_ring_free();
	kg_lisp_shutdown();
}
