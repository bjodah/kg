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

/* ---- A window's view of a buffer ----
 *
 * A window names its buffer by handle, never by slot.  These four are the
 * whole supported vocabulary; they live in bufmgr.c beside the identity
 * they check. */

/* The buffer `w` shows, or NULL when it shows none.  An inactive window, a
 * detached view and a window left on a killed buffer all answer NULL. */
struct editor_buffer *win_buffer(const struct editor_window *w);

/* The slot `w` shows, or -1.  The only supported way to get an index out
 * of a window, and the reason a failed resolution never becomes one. */
int win_buffer_slot(const struct editor_window *w);

/* Whether `w` shows the buffer `handle` names.  A handle that no longer
 * resolves matches nothing, on either side: two views of nothing are not
 * views of the same thing. */
int win_shows_buffer(
    const struct editor_window *w, struct kg_buffer_handle handle);

/* Hand the buffer a window is leaving the point that window had in it. */
void buf_remember_view(const struct editor_window *w);

/* Point `w` at buffer slot `slot`, resuming where that buffer was last
 * left.  A slot holding no live buffer leaves `w` alone. */
void buf_attach_view(struct editor_window *w, int slot);

/* Take `w` off whatever it shows, banking its point first.  The view is
 * left naming nothing. */
void buf_detach_view(struct editor_window *w);

#endif /* KG_BUFHANDLE_H */
