#ifndef KG_BUFHANDLE_H
#define KG_BUFHANDLE_H

#include <stdint.h>

/* How a buffer is named, and how a view says which one it shows.
 *
 * This is the seam between the buffer table and everything that outlives a
 * single command.  It is its own header because `struct editor_window`
 * stores a handle by value and is defined before the buffer table is, so
 * def.h needs the type before it needs either record.
 *
 * Forward declarations only: the records live in def.h, and a consumer that
 * dereferences one includes def.h anyway.  Keeping them out is what lets
 * this header compile on its own (`make header-check`).
 */
struct editor_buffer;
struct editor_window;

/* A reference to a buffer that outlives the command that took it.
 *
 * A bare slot index does not: buf_kill() frees the slot and the next open
 * hands it to an unrelated buffer, so an index kept across commands can
 * quietly start naming somebody else's text.  The identity pair survives
 * that, because buf_resolve() refuses to answer unless the slot still holds
 * the same (id, generation) the handle was taken from.  A zeroed handle
 * names nothing. */
struct kg_buffer_handle {
	int slot;
	uint64_t id;
	uint64_t generation;
};

/* The handle naming the buffer in `slot`, or a handle naming nothing. */
struct kg_buffer_handle buf_handle(int slot);

/* The buffer `handle` names, or NULL once it is gone. */
struct editor_buffer *buf_resolve(struct kg_buffer_handle handle);

/* The slot `handle` names, or -1.  Never index an array with the return
 * value without testing it: -1 is the answer for a handle that has died. */
int buf_handle_slot(struct kg_buffer_handle handle);

/* Hand the buffer a window is leaving the point that window had in it. */
void buf_remember_view(const struct editor_window *w);

/* Point `w` at buffer slot `slot`, resuming where that buffer was last
 * left. */
void buf_attach_view(struct editor_window *w, int slot);

#endif /* KG_BUFHANDLE_H */
