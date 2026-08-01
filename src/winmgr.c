/* ========================= Window management ============================== */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bufhandle.h"
#include "def.h"
#include "event.h"

struct editor_window winlist[MAX_WINDOWS];
int win_current = 0;
int win_count = 0;
int win_total_rows = 0;
int win_total_cols = 0;

/* Next window identity to hand out.  Same policy as buf_next_id in
 * bufmgr.c: 64 bits, starts at 1 so a zeroed slot names no window, and a
 * counter that reaches UINT64_MAX refuses to reuse rather than wrap. */
static uint64_t win_next_id = 1;

/* The kg_slot_table view of winlist[], for the generic identity checks in
 * bufhandle.h; see buf_slot_table in bufmgr.c, its counterpart. */
static const struct kg_slot_table win_slot_table = { winlist, MAX_WINDOWS,
	sizeof(winlist[0]), offsetof(struct editor_window, active),
	offsetof(struct editor_window, id),
	offsetof(struct editor_window, generation) };

/* Give `slot` a fresh window identity: bump its generation so any handle
 * taken on whatever was there stops resolving, then hand out the next id
 * (or leave it 0, unresolvable, once the counter is exhausted).  Called
 * whenever a slot starts a new window -- win_init() and both splits -- and
 * deliberately not by anything that merely copies a window's viewport. */
static void win_claim_slot(int slot)
{
	winlist[slot].generation++;
	if (win_next_id == UINT64_MAX) {
		winlist[slot].id = 0;
		editor_set_status_message("Window identity exhausted.");
		return;
	}
	winlist[slot].id = win_next_id++;
}

struct kg_window_handle win_handle(int slot)
{
	struct kg_window_handle handle = { -1, 0, 0 };

	if (!kg_slot_active_at(win_slot_table, slot)) {
		return handle;
	}
	handle.slot = slot;
	handle.id = winlist[slot].id;
	handle.generation = winlist[slot].generation;
	return handle;
}

struct kg_window_handle win_handle_of(const struct editor_window *w)
{
	int slot = kg_slot_find(win_slot_table, w);

	return slot < 0 ? (struct kg_window_handle) { -1, 0, 0 }
			: win_handle(slot);
}

struct editor_window *win_resolve(struct kg_window_handle handle)
{
	enum kg_slot_check r = kg_slot_resolves(
	    win_slot_table, handle.slot, handle.id, handle.generation);

	return r == KG_SLOT_OK ? &winlist[handle.slot] : NULL;
}

int win_handle_slot(struct kg_window_handle handle)
{
	return win_resolve(handle) ? handle.slot : -1;
}

/* Recompute the layout of all active windows.
 *
 * Windows are organised into column groups (col_group field).  Windows sharing
 * the same col_group value are stacked vertically in the same column; different
 * col_group values are placed side-by-side.
 *
 * Layout algorithm:
 *   1. Collect distinct col_group values in order of first appearance.
 *   2. Divide win_total_cols equally among column groups.
 *   3. Within each column group, divide the usable rows equally among its
 * windows. Each window band = (text rows) + 1 mode-line row.
 *   4. One global echo-area row is reserved at win_total_rows.
 */
void win_reflow(void)
{
	int col_groups[MAX_WINDOWS];
	int num_col_groups = 0;
	int usable, col_w, col_rem, col_x;
	int i, j;

	if (win_count == 0) {
		return;
	}

	/* Collect distinct col_group values in appearance order. */
	for (i = 0; i < MAX_WINDOWS; i++) {
		int found = 0;
		if (!winlist[i].active) {
			continue;
		}
		for (j = 0; j < num_col_groups; j++) {
			if (col_groups[j] == winlist[i].col_group) {
				found = 1;
				break;
			}
		}
		if (!found) {
			col_groups[num_col_groups++] = winlist[i].col_group;
		}
	}

	if (num_col_groups == 0) {
		return;
	}

	usable
	    = win_total_rows - 1; /* reserve 1 row for the global echo area */
	/* Defensive: apply_window_size() keeps win_total_* at or above
	 * KG_MIN_ROWS/KG_MIN_COLS, but a caller that sets them directly
	 * must not be able to hand out a negative height or a zero-width
	 * window, which would put a mode line on terminal row 0. */
	if (usable < 1) {
		usable = 1;
	}
	/* Reserve one column per inter-group gap for the vertical separator. */
	col_w = (win_total_cols - (num_col_groups - 1)) / num_col_groups;
	col_rem = (win_total_cols - (num_col_groups - 1)) % num_col_groups;
	col_x = 1; /* VT100 columns are 1-based */

	for (j = 0; j < num_col_groups; j++) {
		int cg = col_groups[j];
		int cw = col_w + (j == num_col_groups - 1 ? col_rem : 0);
		int n = 0; /* windows in this col_group */
		int row_h, row_rem, row_y, win_idx;

		for (i = 0; i < MAX_WINDOWS; i++) {
			if (winlist[i].active && winlist[i].col_group == cg) {
				n++;
			}
		}

		row_h = usable / n;
		row_rem = usable % n;
		row_y = 1;
		win_idx = 0;

		for (i = 0; i < MAX_WINDOWS; i++) {
			int band_h;
			if (!winlist[i].active || winlist[i].col_group != cg) {
				continue;
			}

			band_h = row_h + (win_idx < row_rem ? 1 : 0);
			winlist[i].y = row_y;
			winlist[i].x = col_x;
			winlist[i].h = band_h
			    - 1; /* text rows; mode line uses the last row */
			winlist[i].w = cw;
			if (winlist[i].h < 0) {
				winlist[i].h = 0;
			}
			if (winlist[i].w < 1) {
				winlist[i].w = 1;
			}
			row_y += band_h;
			win_idx++;
		}

		col_x += cw
		    + 1; /* +1 skips the separator column between groups */
	}

	/* Clamp cursor to new bounds. */
	if (wcur()->cy >= wcur()->h) {
		wcur()->cy = wcur()->h - 1;
	}
	if (wcur()->cx >= wcur()->w) {
		wcur()->cx = wcur()->w - 1;
	}
	if (wcur()->cy < 0) {
		wcur()->cy = 0;
	}
	if (wcur()->cx < 0) {
		wcur()->cx = 0;
	}
}

/* The active window showing the buffer `handle` names, or -1.  The one
 * place "is this buffer on screen?" is answered. */
static int win_showing(struct kg_buffer_handle handle)
{
	int i;

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (winlist[i].active
		    && win_shows_buffer(&winlist[i], handle)) {
			return i;
		}
	}
	return -1;
}

#if KG_DEBUG_STATE
#include <stdio.h>
#include <stdlib.h>

static void state_fail(const char *where, const char *what)
{
	fprintf(stderr, "kg: state invariant failed at %s: %s\n", where, what);
	abort();
}

#define STATE_CHECK(cond)                                                      \
	do {                                                                   \
		if (!(cond)) {                                                 \
			state_fail(where, #cond);                              \
		}                                                              \
	} while (0)

/* The seventh invariant, for one marker handle a live buffer is supposed to
 * own: `owner`'s mark, a mark ring entry, or a decoration endpoint.  A
 * zeroed handle (an unset mark, or a mark-ring slot past mark_ring_len
 * never reaches here) owns nothing and is skipped rather than asked to
 * resolve.  Otherwise it must both resolve *and* resolve to `owner` --
 * two different failures a corrupted `.buffer` field can produce: a
 * marker that has simply gone, and one that now answers for somebody
 * else's buffer, which resolving alone would not catch. */
static void check_marker_owner(
    const char *where, struct kg_marker_handle m, struct kg_buffer_handle owner)
{
	if (!m.id) {
		return;
	}
	STATE_CHECK(m.buffer.slot == owner.slot && m.buffer.id == owner.id
	    && m.buffer.generation == owner.generation);
	STATE_CHECK(kg_marker_resolve(m, NULL) == KG_MARKER_OK);
}

/* The seventh invariant, for everything one live buffer owns: its mark,
 * every live entry of its mark ring, and both endpoint markers of every
 * decoration in its store.  Split out of kg_state_check() so that
 * function's own branching stays where the six invariants it already had
 * left it -- this is a second function, not a second cost against the
 * same one. */
static void check_buffer_owned_markers(
    const char *where, struct editor_buffer *b, struct kg_buffer_handle self)
{
	size_t k;

	check_marker_owner(where, b->mark.marker, self);
	for (k = 0; k < (size_t)b->mark_ring_len; k++) {
		check_marker_owner(where, b->mark_ring[k].marker, self);
	}
	if (b->decorations) {
		for (k = 0; k < b->decorations->count; k++) {
			struct kg_decor *d = &b->decorations->items[k];
			check_marker_owner(where, d->start, self);
			check_marker_owner(where, d->end, self);
		}
	}
}

/* What the buffer table, the window table and the selection are supposed
 * to agree on at a lifecycle commit -- the seven invariants Plan 04 names. */
void kg_state_check(const char *where)
{
	int i, j, buffers = 0, windows = 0;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];

		if (!b->active) {
			continue;
		}
		buffers++;
		/* A live buffer has an identity, and rows a count and a
		 * capacity can describe. */
		STATE_CHECK(b->id);
		STATE_CHECK(b->numrows >= 0);
		STATE_CHECK(b->row_capacity >= b->numrows);
		STATE_CHECK(b->row || !b->numrows);
		check_buffer_owned_markers(where, b, buf_handle(i));
		for (j = i + 1; j < MAX_BUFFERS; j++) {
			if (!buflist[j].active) {
				continue;
			}
			/* Identities are unique, and live row storage has
			 * one owner: two buffers sharing a row array is a
			 * double free waiting for the second kill. */
			STATE_CHECK(buflist[j].id != b->id);
			STATE_CHECK(!b->row || buflist[j].row != b->row);
		}
	}
	STATE_CHECK(buffers == buf_count);

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			continue;
		}
		windows++;
		/* Every active window resolves; win_check_handles() is what
		 * makes this true for a release build. */
		STATE_CHECK(win_buffer(&winlist[i]));
	}
	STATE_CHECK(windows == win_count);
	/* And the selected window is showing the current buffer, or the
	 * mode line describes one buffer while the keys edit another. */
	STATE_CHECK(!win_count || win_buffer(wcur()) == bcur());
}
#endif /* KG_DEBUG_STATE */

/* Put every active window back on a live buffer.
 *
 * Every path that ends a buffer already detaches the views on it, so
 * reaching this with work to do means one did not: it is a bug, not a
 * state to render.  The response is conservative rather than clever --
 * the window takes the buffer the session is in, which is the one
 * definitely alive -- and it says so, because a window quietly changing
 * what it shows is how the bug would otherwise stay hidden.
 *
 * Runs at the top of the main loop, before the repaint, so a window is
 * never drawn from a handle that has stopped resolving. */
void win_check_handles(void)
{
	int i;

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active || win_buffer(&winlist[i])) {
			continue;
		}
		buf_detach_view(&winlist[i]);
		buf_attach_view(&winlist[i], buf_current);
		editor_set_status_message(
		    "Window %d had lost its buffer.", i + 1);
	}
}

/* Initialise the window list with a single window covering the whole screen.
 * Called once from init_editor() after update_window_size(). */
void win_init(void)
{
	memset(winlist, 0, sizeof(winlist));
	win_current = 0;
	win_count = 1;

	winlist[0].active = 1;
	winlist[0].col_group = 0;
	win_claim_slot(0);
	/* No-op during init_editor(), which runs before any buffer exists:
	 * buf_load_args()' first buf_select() is what gives window 0 its
	 * buffer.  It matters for a caller that opens the buffers first. */
	buf_attach_view(&winlist[0], buf_current);

	/* win_total_rows/cols are set by update_window_size() before
	 * win_init(). */
	win_reflow();
}

/* Publish the KG_EVENT_VIEW_ATTACHED a split's new window earns.  Unlike
 * buf_attach_view(), `w` already shows the buffer by the time this runs --
 * the struct copy in win_split_horizontal()/win_split_vertical() did that,
 * deliberately, so the new view keeps the split window's own point rather
 * than resuming at the buffer's last_point -- so this only announces the
 * attach that already happened.  A refused reservation costs only the
 * event: `w` is correctly attached either way, the same as any other
 * lifecycle producer's refusal. */
static void win_publish_split_attach(struct editor_window *w)
{
	struct kg_event_reservation res = kg_event_reserve_lifecycle();

	if (!res.valid) {
		return;
	}
	kg_event_publish_lifecycle(
	    &res, kg_event_make_view_attached(win_handle_of(w), w->buf));
}

/* Split the current window horizontally (C-x 2): the current window shrinks
 * to the top half; a new window showing the same buffer appears below.
 * The new window stays in the same column group. */
void win_split_horizontal(void)
{
	int i, slot = -1;

	if (win_count >= MAX_WINDOWS) {
		editor_set_status_message(
		    "Too many windows (%d max).", MAX_WINDOWS);
		return;
	}
	if (winlist[win_current].h < 3) {
		editor_set_status_message("Window too small to split.");
		return;
	}

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return;
	}

	/* New window inherits the same buffer, view, and col_group: a split
	 * copies the view, which is what "the same place in the same buffer"
	 * means and what the record already says.  Copying the buffer handle
	 * is deliberate -- two views of one buffer, not two names for it.
	 * The window's own identity is the opposite: win_claim_slot() below
	 * overwrites whatever the struct copy just gave `slot`, because a
	 * split is a new view and needs a name of its own. */
	winlist[slot] = winlist[win_current];
	winlist[slot].active = 1;
	win_claim_slot(slot);
	win_publish_split_attach(&winlist[slot]);

	win_count++;
	win_reflow();
}

/* Split the current window vertically (C-x 3): the current window becomes the
 * left half; a new window showing the same buffer appears to the right.
 * The new window is placed in a new column group. */
void win_split_vertical(void)
{
	int i, slot = -1, max_cg = 0;

	if (win_count >= MAX_WINDOWS) {
		editor_set_status_message(
		    "Too many windows (%d max).", MAX_WINDOWS);
		return;
	}
	if (winlist[win_current].w < 6) {
		editor_set_status_message("Window too small to split.");
		return;
	}

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			if (slot < 0) {
				slot = i;
			}
			continue;
		}
		if (winlist[i].col_group > max_cg) {
			max_cg = winlist[i].col_group;
		}
	}
	if (slot < 0) {
		return;
	}

	/* New window: same buffer handle and view, but a new column group
	 * (rightmost) and, as in the horizontal split, a fresh identity. */
	winlist[slot] = winlist[win_current];
	winlist[slot].active = 1;
	winlist[slot].col_group = max_cg + 1;
	win_claim_slot(slot);
	win_publish_split_attach(&winlist[slot]);

	win_count++;
	win_reflow();
}

/* Switch focus to the next window (C-x o). */
void win_cycle_next(void)
{
	int i;

	if (win_count <= 1) {
		editor_set_status_message("No other windows.");
		return;
	}

	for (i = 1; i <= MAX_WINDOWS; i++) {
		int idx = (win_current + i) % MAX_WINDOWS;
		if (winlist[idx].active) {
			win_current = idx;
			break;
		}
	}

	/* The window already holds its own view; all that changes is which
	 * buffer is current.  A window whose handle no longer resolves gives
	 * -1, which buf_select() declines rather than indexes. */
	buf_select(win_buffer_slot(wcur()));
	editor_set_status_message(
	    "%s", bcur()->filename ? bcur()->filename : "[new]");
}

/* Delete the current window (C-x 0). */
void win_delete_current(void)
{
	int i;

	if (win_count <= 1) {
		editor_set_status_message("Only one window.");
		return;
	}

	buf_detach_view(&winlist[win_current]);
	winlist[win_current].active = 0;
	/* Belt and braces alongside the active flag above, matching
	 * buf_kill_commit()'s own double coverage: a handle taken on this
	 * window stops resolving here, not only once the slot is claimed by
	 * a future split. */
	winlist[win_current].generation++;
	win_count--;

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (winlist[i].active) {
			win_current = i;
			break;
		}
	}

	win_reflow();
	buf_select(win_buffer_slot(wcur()));
}

/* Delete all other windows, leaving only the current one (C-x 1).  Each
 * discarded view banks its point first, the way C-x 0 does: a window's
 * point is the only record of where the buffer it showed was being read,
 * so throwing the window away without it makes the buffer resume wherever
 * some earlier view left off. */
void win_delete_others(void)
{
	int i;

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (i == win_current || !winlist[i].active) {
			continue;
		}
		buf_detach_view(&winlist[i]);
		winlist[i].active = 0;
		winlist[i].generation++;
	}
	win_count = 1;
	win_reflow();
}

/* Point `w` at `buffer_index` showing its start.  Output buffers are
 * meant to be read from the top (or, for a compilation, from the end that
 * win_position_at_end() then picks), so this deliberately does not resume
 * where the buffer was last left. */
static void win_show_at_top(struct editor_window *w, int buffer_index)
{
	buf_detach_view(w);
	w->buf = buf_handle(buffer_index);
	w->cx = w->cy = 0;
	w->rowoff = w->coloff = 0;
	w->rowoff_visual = 0;
	w->desired_visual_col = -1;
}

void win_display_buffer_other_window(int buffer_index)
{
	struct kg_buffer_handle h = buf_handle(buffer_index);
	int j;

	if (!buf_resolve(h) || win_showing(h) >= 0) {
		return;
	}

	if (win_count == 1 && winlist[win_current].h >= 6) {
		win_split_horizontal();
		for (j = 0; j < MAX_WINDOWS; j++) {
			if (winlist[j].active && j != win_current) {
				win_show_at_top(&winlist[j], buffer_index);
				break;
			}
		}
		return;
	}

	if (win_count >= 2) {
		j = (win_current + 1) % MAX_WINDOWS;
		while (j != win_current) {
			if (winlist[j].active) {
				win_show_at_top(&winlist[j], buffer_index);
				return;
			}
			j = (j + 1) % MAX_WINDOWS;
		}
	}

	buf_select(buffer_index);
}

/* True if win_display_buffer_other_window(buffer_index) can show the buffer
 * without stealing the current window: it's already visible somewhere, a
 * split is possible, or a second window already exists to reuse.  Lets a
 * caller that must not steal the current window (e.g. shell output) decide
 * whether to call win_display_buffer_other_window() at all. */
int win_can_display_buffer_other_window(int buffer_index)
{
	struct kg_buffer_handle h = buf_handle(buffer_index);

	if (!buf_resolve(h)) {
		return 0;
	}
	if (win_showing(h) >= 0) {
		return 1;
	}
	if (win_count == 1 && winlist[win_current].h >= 6) {
		return 1;
	}

	return win_count >= 2;
}

void win_position_at_end(int buffer_index)
{
	struct kg_buffer_handle h = buf_handle(buffer_index);
	struct editor_buffer *b = buf_resolve(h);
	int numrows, i;

	if (!b) {
		return;
	}
	numrows = b->numrows;
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct editor_window *w = &winlist[i];
		if (w->active && win_shows_buffer(w, h)) {
			int rowoff = (numrows > w->h) ? (numrows - w->h) : 0;
			w->rowoff = rowoff;
			w->cy = (numrows > 0) ? (numrows - 1 - rowoff) : 0;
			w->cx = 0;
		}
	}
}
