/* stubs_lispobj.c - view/buffer-handle primitives for test_lisp.c.
 * Real bufmgr.o defines these; the Lisp test harness links the lisp modules
 * but not bufmgr.o, so it needs faithful-enough versions over the same
 * buflist[] and winlist[] (real buffer.c/marker.c/undo.c back them).
 * Only EXTRA_lisp links this file, so the strong definitions here cannot
 * collide with test_compile.c's or bufmgr.o's. */

#include "../src/def.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct editor_buffer *win_buffer(const struct editor_window *w)
{
	return buf_resolve(w ? w->buf : (struct kg_buffer_handle) { -1, 0, 0 });
}

int buf_handle_slot(struct kg_buffer_handle handle)
{
	return buf_resolve(handle) ? handle.slot : -1;
}

int win_shows_buffer(
    const struct editor_window *w, struct kg_buffer_handle handle)
{
	const struct editor_buffer *shown = win_buffer(w);

	return shown != NULL && shown == buf_resolve(handle);
}

static uint64_t stub_buffer_next_id = 100;

struct kg_buffer_handle buf_create_named(const char *name)
{
	struct kg_buffer_handle zeroed = { -1, 0, 0 };
	int slot;

	if (!name || !name[0] || buf_count >= MAX_BUFFERS) {
		return zeroed;
	}
	for (slot = 0; slot < MAX_BUFFERS; slot++) {
		if (!buflist[slot].active) {
			break;
		}
	}
	if (slot >= MAX_BUFFERS) {
		return zeroed;
	}
	buflist[slot].generation++;
	buflist[slot].id = stub_buffer_next_id++;
	buflist[slot].active = 1;
	buflist[slot].filename = strdup(name);
	buflist[slot].dirty = 0;
	buflist[slot].readonly_override = -1;
	undo_stack_init(&buflist[slot].undostack);
	buf_count++;
	return buf_handle(slot);
}

int buf_kill_buffer(struct kg_buffer_handle handle)
{
	struct editor_buffer *b = buf_resolve(handle);
	int slot = handle.slot;
	int i;

	if (!b || slot < 0 || slot >= MAX_BUFFERS) {
		return 0;
	}
	if (b->dirty) {
		return 0;
	}
	if (buf_count <= 1) {
		return 0;
	}
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (winlist[i].active
		    && win_shows_buffer(&winlist[i], handle)) {
			winlist[i].buf = (struct kg_buffer_handle) { -1, 0, 0 };
		}
	}
	editor_free_all_rows(b);
	undo_stack_free(&b->undostack);
	kg_marker_store_free(b);
	kg_decor_store_free(b);
	free(b->filename);
	b->active = 0;
	b->filename = NULL;
	b->dirty = 0;
	b->generation++;
	buf_count--;
	return 1;
}
