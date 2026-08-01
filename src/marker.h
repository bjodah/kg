/* marker.h — persistent buffer byte position markers */

#ifndef KG_MARKER_H
#define KG_MARKER_H

#include "bufhandle.h"
#include <stddef.h>
#include <stdint.h>

struct editor_buffer;

enum kg_marker_gravity {
	KG_MARKER_GRAV_LEFT = 0,
	KG_MARKER_GRAV_RIGHT = 1,
};

enum kg_marker_result {
	KG_MARKER_OK = 0,
	KG_MARKER_GONE = 1,
	KG_MARKER_TYPE_ERROR = 2,
};

struct kg_marker_handle {
	struct kg_buffer_handle buffer;
	uint32_t id;
	uint32_t generation;
};

/* A buffer mark is a persistent byte marker plus the one geometry detail a
 * byte position cannot encode: point may sit in virtual chars space past
 * EOL while defining a rectangle.  -1 means the marker's resolved column. */
struct kg_buffer_mark {
	struct kg_marker_handle marker;
	int virtual_chars_col;
};

struct kg_marker {
	uint32_t id;
	uint32_t generation;
	size_t position; /* flat byte offset */
	enum kg_marker_gravity gravity;
	bool active;
};

struct kg_marker_store {
	struct kg_marker *items;
	size_t count;
	size_t capacity;
	uint32_t next_id;
};

/* API */
struct kg_marker_handle kg_marker_create(
    struct editor_buffer *b, size_t pos, enum kg_marker_gravity gravity);
enum kg_marker_result kg_marker_resolve(
    struct kg_marker_handle handle, size_t *out_pos);
enum kg_marker_result kg_marker_set_position(
    struct kg_marker_handle handle, size_t pos);
enum kg_marker_result kg_marker_set_gravity(
    struct kg_marker_handle handle, enum kg_marker_gravity gravity);
enum kg_marker_result kg_marker_get_gravity(
    struct kg_marker_handle handle, enum kg_marker_gravity *out_gravity);
bool kg_marker_delete(struct kg_marker_handle handle);
void kg_marker_relocate_all(struct editor_buffer *b, size_t begin_byte,
    size_t end_byte, size_t replacement_len);
void kg_marker_detach_all(struct editor_buffer *b);
void kg_marker_store_free(struct editor_buffer *b);
bool kg_marker_get_row_col(
    struct kg_marker_handle handle, int *out_row, int *out_col);

/* The buffer-owned active mark.  It has left insertion gravity and is
 * addressed through the same stable handle as every other marker. */
bool kg_mark_is_set(const struct editor_buffer *b);
bool kg_mark_get_position(const struct editor_buffer *b, size_t *out_pos);
bool kg_mark_get_row_col(
    const struct editor_buffer *b, int *out_row, int *out_col);
bool kg_mark_set_position(struct editor_buffer *b, size_t pos);
bool kg_mark_set_row_col(struct editor_buffer *b, int row, int col);
void kg_mark_clear(struct editor_buffer *b);
void kg_mark_ring_clear(struct editor_buffer *b);

#endif /* KG_MARKER_H */
