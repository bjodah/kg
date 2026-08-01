/* decor.h — compact, marker-anchored buffer decorations
 *
 * A decoration is a temporary visual span: a face and a priority attached
 * to the byte range between two markers.  It carries no strings, callbacks,
 * runtime values or arbitrary text properties -- a renderer reads only the
 * fixed fields below.  See src/marker.h for the position model this is
 * built on; a decoration's range is never stored as its own pair of byte
 * offsets, only as two marker handles that relocate with the text.
 *
 * This header stops at storage and lifecycle: creation, deletion, handle
 * resolution, and the two edit-integration hooks buffer.c calls
 * (kg_decor_compact() after marker relocation, kg_decor_store_free() on
 * buffer kill/reuse).  The renderer's visible-range query is a later
 * slice's addition, not this one's.
 */

#ifndef KG_DECOR_H
#define KG_DECOR_H

#include "bufhandle.h"
#include "marker.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct editor_buffer;

/* A compact, closed set of visual faces -- not a text property bag.  A new
 * consumer adds a value here rather than smuggling a string or a color
 * through the record. */
enum kg_decor_face {
	KG_DECOR_FACE_MATCH = 0, /* Isearch/query-replace's current match. */
	KG_DECOR_FACE_WARNING = 1, /* A parsed diagnostic, e.g. compile. */
};

enum kg_decor_result {
	KG_DECOR_OK = 0,
	KG_DECOR_GONE = 1,
	KG_DECOR_TYPE_ERROR = 2,
};

struct kg_decor_handle {
	struct kg_buffer_handle buffer;
	uint32_t id;
	uint32_t generation;
};

/* The private record: two marker handles standing in for a byte range,
 * a face, a priority, and a flags byte -- endpoint gravity (cached from
 * the markers, for a query that wants it without two extra resolves) and
 * the evaporate bit.  No strings, callbacks, or open-ended properties;
 * see decor.c for the flag bit layout, which nothing outside this module
 * reads directly. */
struct kg_decor {
	uint32_t id;
	uint32_t generation;
	struct kg_marker_handle start;
	struct kg_marker_handle end;
	enum kg_decor_face face;
	uint8_t priority;
	uint8_t flags;
};

/* The buffer-owned store: a packed array of live records only, kept sorted
 * by resolved start, then end, then priority, then ID.  Deletion and
 * evaporation compact it in place; there are no inactive holes to scan
 * past, unlike src/marker.h's store. */
struct kg_decor_store {
	struct kg_decor *items;
	size_t count;
	size_t capacity;
	uint32_t next_id;
};

/* What kg_decor_resolve() reports: the marker-resolved span alongside the
 * three fixed fields a renderer needs and nothing more. */
struct kg_decor_info {
	size_t start;
	size_t end;
	enum kg_decor_face face;
	uint8_t priority;
	bool evaporate;
};

/* Create a decoration spanning [start_pos, end_pos) of `b`, anchored by two
 * markers with the stated gravities.  Reversed endpoints are normalized
 * (the smaller position becomes the start, carrying the gravity its
 * argument named); a fully formed match highlight -- excluded from growing
 * to either side when text is typed at its edge -- states
 * KG_MARKER_GRAV_RIGHT for `start_gravity` and KG_MARKER_GRAV_LEFT for
 * `end_gravity`.  `evaporate` marks the decoration for removal once an
 * edit leaves it empty (start == end); a non-evaporating empty decoration
 * simply sits at one point.
 *
 * Stages the store's capacity and both endpoint markers before publishing
 * the record.  On any failure -- bad face, exhausted IDs, or an
 * allocation -- deletes whatever it staged and returns a handle that
 * resolves as gone, leaving the store exactly as it was. */
struct kg_decor_handle kg_decor_create(struct editor_buffer *b,
    size_t start_pos, size_t end_pos, enum kg_marker_gravity start_gravity,
    enum kg_marker_gravity end_gravity, enum kg_decor_face face,
    uint8_t priority, bool evaporate);

/* The decoration's current, marker-resolved span, face, priority and
 * evaporate flag, in one lookup.  Gone when the handle is stale, the
 * buffer is gone, or either endpoint marker has gone -- a decoration with
 * only one live endpoint is never reported as a guessed one-sided range.
 * `out` may be NULL to just test liveness. */
enum kg_decor_result kg_decor_resolve(
    struct kg_decor_handle handle, struct kg_decor_info *out);

/* Delete a decoration and release both its endpoint markers.  Returns
 * false for an already-gone handle. */
bool kg_decor_delete(struct kg_decor_handle handle);

/* The no-fail publication-tail pass: drop every decoration with a gone
 * endpoint, then every evaporating decoration an edit left empty, deleting
 * their markers and compacting the store in place.  Allocates nothing.
 * buffer.c calls this once, immediately after kg_marker_relocate_all() (an
 * ordinary edit) or kg_marker_detach_all() (broad row adoption, where
 * every marker is now gone and this clears every decoration with it). */
void kg_decor_compact(struct editor_buffer *b);

/* Free `b`'s decoration store.  Called everywhere kg_marker_store_free()
 * is: buffer kill, slot reuse, and shutdown.  A decoration never outlives
 * the markers it points at, so this runs before or alongside that call,
 * never after the buffer identity it names is gone. */
void kg_decor_store_free(struct editor_buffer *b);

/* Test-only allocation-failure seam for this module's own store growth
 * (the store's first calloc, and decor_store_reserve()'s realloc) --
 * distinct from kg_edit_fail_alloc_after() in edit.h, which governs the
 * edit-staging path, and from whatever seam src/marker.c may grow for its
 * own allocator.  Lets the next `n` such allocations succeed and fails the
 * one after that, then disarms itself; a negative `n` disarms it
 * immediately, which is how the editor always runs.  Nothing outside
 * test/ calls this. */
void kg_decor_fail_alloc_after(int n);

#endif /* KG_DECOR_H */
