/* stubs_win.c — window globals and no-op window entry points for tests that
 * link bufmgr.o but not winmgr.o.  test_winmgr links the real winmgr.o
 * instead of this file, which is why these live apart from stubs_buffer.c. */

#include "../src/bufhandle.h"
#include "../src/def.h"
#include <stddef.h>

struct editor_window winlist[MAX_WINDOWS];
int win_current = 0;
int win_count = 0;
int win_total_rows = 24;
int win_total_cols = 80;

void win_display_buffer_other_window(int idx) { (void)idx; }
void win_position_at_end(int idx) { (void)idx; }
void win_position_at_row(int idx, int row)
{
	(void)idx;
	(void)row;
}

/* A window's own identity, for suites that link bufmgr.o's producers
 * (buf_attach_view()/buf_detach_view_commit() call win_handle_of()) but not
 * the real winmgr.c that normally claims one.  Genuine, not a fake: this is
 * the same generic identity check winmgr.c uses, over the same winlist[]
 * these tests poke `.active` in directly -- there is just no win_claim_slot()
 * call, so a window's id here is whatever the test left it (0 unless a test
 * sets one), the same shortcut reset_current_buffer() documents for
 * buflist[]'s id. */
static const struct kg_slot_table win_slot_table = { winlist, MAX_WINDOWS,
	sizeof(winlist[0]), offsetof(struct editor_window, active),
	offsetof(struct editor_window, id),
	offsetof(struct editor_window, generation) };

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
