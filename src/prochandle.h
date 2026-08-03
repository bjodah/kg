#ifndef KG_PROCHANDLE_H
#define KG_PROCHANDLE_H

#include <stdint.h>

/* A reference to a table entry in src/process_table.c that outlives the
 * poll or command that took it.
 *
 * This is its own header, following src/bufhandle.h's precedent: src/event.h
 * needs this type for its process event payload and must not be made to
 * include the whole process table to get it.  Unlike a buffer or window
 * handle (bufhandle.h's struct kg_buffer_handle/kg_window_handle), there is
 * no separate identity counter here -- a process table entry has no `id`
 * distinct from its slot, only a generation that the table bumps every time
 * the slot changes occupant, so (slot, generation) alone is the whole
 * identity check.  A zeroed handle (generation 0) never resolves: the
 * table's generations start at 1. */
struct kg_process_handle {
	uint32_t slot;
	uint32_t generation;
};

#endif /* KG_PROCHANDLE_H */
