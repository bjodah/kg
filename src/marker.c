/* marker.c — persistent buffer byte position markers */

#include "marker.h"
#include "def.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static struct kg_buffer_handle buffer_get_handle(const struct editor_buffer *b)
{
	for (int i = 0; b && i < MAX_BUFFERS; i++) {
		if (b == &buflist[i]) {
			return buf_handle(i);
		}
	}
	struct kg_buffer_handle zero = { -1, 0, 0 };
	return zero;
}

static bool marker_gravity_valid(enum kg_marker_gravity gravity)
{
	return gravity == KG_MARKER_GRAV_LEFT
	    || gravity == KG_MARKER_GRAV_RIGHT;
}

static struct kg_marker *marker_store_claim_slot(struct kg_marker_store *store)
{
	for (size_t i = 0; i < store->count; i++) {
		if (!store->items[i].active) {
			return &store->items[i];
		}
	}
	if (store->count == store->capacity) {
		size_t old_cap = store->capacity;
		size_t new_cap;

		if (old_cap > SIZE_MAX / 2) {
			return NULL;
		}
		new_cap = old_cap ? old_cap * 2 : 8;
		if (new_cap > SIZE_MAX / sizeof(*store->items)) {
			return NULL;
		}
		struct kg_marker *new_items
		    = realloc(store->items, new_cap * sizeof(*new_items));
		if (!new_items) {
			return NULL;
		}
		memset(new_items + old_cap, 0,
		    (new_cap - old_cap) * sizeof(*new_items));
		store->items = new_items;
		store->capacity = new_cap;
	}
	return &store->items[store->count++];
}

struct kg_marker_handle kg_marker_create(
    struct editor_buffer *b, size_t pos, enum kg_marker_gravity gravity)
{
	struct kg_marker_handle null_handle = { { -1, 0, 0 }, 0, 0 };
	struct kg_buffer_handle buffer = buffer_get_handle(b);
	if (!b || !marker_gravity_valid(gravity)) {
		return null_handle;
	}
	if (buffer.slot < 0) {
		return null_handle;
	}

	size_t b_len = (size_t)buffer_byte_length(b);
	if (pos > b_len) {
		pos = b_len;
	}

	if (!b->markers) {
		b->markers = calloc(1, sizeof(*b->markers));
		if (!b->markers) {
			return null_handle;
		}
		b->markers->next_id = 1;
	}

	struct kg_marker_store *store = b->markers;
	if (store->next_id == UINT32_MAX) {
		return null_handle;
	}
	struct kg_marker *m = marker_store_claim_slot(store);
	if (!m) {
		return null_handle;
	}
	m->id = store->next_id++;
	m->generation++;
	m->position = pos;
	m->gravity = gravity;
	m->active = true;

	struct kg_marker_handle handle;
	handle.buffer = buffer;
	handle.id = m->id;
	handle.generation = m->generation;
	return handle;
}

static struct kg_marker *store_find(
    struct kg_marker_store *store, uint32_t id, uint32_t generation)
{
	if (!store) {
		return NULL;
	}
	for (size_t i = 0; i < store->count; i++) {
		if (store->items[i].active && store->items[i].id == id
		    && store->items[i].generation == generation) {
			return &store->items[i];
		}
	}
	return NULL;
}

enum kg_marker_result kg_marker_resolve(
    struct kg_marker_handle handle, size_t *out_pos)
{
	struct editor_buffer *b = buf_resolve(handle.buffer);
	if (!b || !b->markers) {
		return KG_MARKER_GONE;
	}
	struct kg_marker *m
	    = store_find(b->markers, handle.id, handle.generation);
	if (!m) {
		return KG_MARKER_GONE;
	}
	if (out_pos) {
		*out_pos = m->position;
	}
	return KG_MARKER_OK;
}

enum kg_marker_result kg_marker_set_position(
    struct kg_marker_handle handle, size_t pos)
{
	struct editor_buffer *b = buf_resolve(handle.buffer);
	if (!b || !b->markers) {
		return KG_MARKER_GONE;
	}
	struct kg_marker *m
	    = store_find(b->markers, handle.id, handle.generation);
	if (!m) {
		return KG_MARKER_GONE;
	}
	size_t b_len = (size_t)buffer_byte_length(b);
	m->position = (pos > b_len) ? b_len : pos;
	return KG_MARKER_OK;
}

enum kg_marker_result kg_marker_set_gravity(
    struct kg_marker_handle handle, enum kg_marker_gravity gravity)
{
	if (!marker_gravity_valid(gravity)) {
		return KG_MARKER_TYPE_ERROR;
	}
	struct editor_buffer *b = buf_resolve(handle.buffer);
	if (!b || !b->markers) {
		return KG_MARKER_GONE;
	}
	struct kg_marker *m
	    = store_find(b->markers, handle.id, handle.generation);
	if (!m) {
		return KG_MARKER_GONE;
	}
	m->gravity = gravity;
	return KG_MARKER_OK;
}

enum kg_marker_result kg_marker_get_gravity(
    struct kg_marker_handle handle, enum kg_marker_gravity *out_gravity)
{
	struct editor_buffer *b = buf_resolve(handle.buffer);
	if (!b || !b->markers) {
		return KG_MARKER_GONE;
	}
	struct kg_marker *m
	    = store_find(b->markers, handle.id, handle.generation);
	if (!m) {
		return KG_MARKER_GONE;
	}
	if (out_gravity) {
		*out_gravity = m->gravity;
	}
	return KG_MARKER_OK;
}

bool kg_marker_delete(struct kg_marker_handle handle)
{
	struct editor_buffer *b = buf_resolve(handle.buffer);
	if (!b || !b->markers) {
		return false;
	}
	struct kg_marker *m
	    = store_find(b->markers, handle.id, handle.generation);
	if (!m) {
		return false;
	}
	m->active = false;
	return true;
}

void kg_marker_relocate_all(struct editor_buffer *b, size_t begin_byte,
    size_t end_byte, size_t replacement_len)
{
	if (!b || !b->markers || b->markers->count == 0) {
		return;
	}
	size_t old_len = (end_byte > begin_byte) ? (end_byte - begin_byte) : 0;
	struct kg_marker_store *store = b->markers;

	for (size_t i = 0; i < store->count; i++) {
		struct kg_marker *m = &store->items[i];
		if (!m->active) {
			continue;
		}
		size_t pos = m->position;
		if (old_len == 0) {
			if (pos > begin_byte) {
				pos += replacement_len;
			} else if (pos == begin_byte) {
				if (m->gravity == KG_MARKER_GRAV_RIGHT) {
					pos = begin_byte + replacement_len;
				}
			}
		} else {
			if (pos >= end_byte) {
				if (replacement_len >= old_len) {
					pos += replacement_len - old_len;
				} else {
					pos -= old_len - replacement_len;
				}
			} else if (pos >= begin_byte) {
				if (m->gravity == KG_MARKER_GRAV_LEFT) {
					pos = begin_byte;
				} else {
					pos = begin_byte + replacement_len;
				}
			}
		}
		m->position = pos;
	}
}

void kg_marker_detach_all(struct editor_buffer *b)
{
	if (!b || !b->markers) {
		return;
	}
	for (size_t i = 0; i < b->markers->count; i++) {
		b->markers->items[i].active = false;
	}
}

void kg_marker_store_free(struct editor_buffer *b)
{
	if (!b || !b->markers) {
		return;
	}
	free(b->markers->items);
	free(b->markers);
	b->markers = NULL;
}

bool kg_marker_get_row_col(
    struct kg_marker_handle handle, int *out_row, int *out_col)
{
	size_t pos;
	int row, col;

	if (kg_marker_resolve(handle, &pos) != KG_MARKER_OK) {
		return false;
	}
	struct editor_buffer *b = buf_resolve(handle.buffer);
	if (!b) {
		return false;
	}
	if (buffer_position_to_row_col(b, pos, &row, &col) != 1) {
		return false;
	}
	if (out_row) {
		*out_row = row;
	}
	if (out_col) {
		*out_col = col;
	}
	return true;
}

bool kg_mark_is_set(const struct editor_buffer *b)
{
	return b && kg_marker_resolve(b->mark.marker, NULL) == KG_MARKER_OK;
}

bool kg_mark_get_position(const struct editor_buffer *b, size_t *out_pos)
{
	return b && kg_marker_resolve(b->mark.marker, out_pos) == KG_MARKER_OK;
}

bool kg_mark_get_row_col(
    const struct editor_buffer *b, int *out_row, int *out_col)
{
	int row, col;

	if (!b || !kg_marker_get_row_col(b->mark.marker, &row, &col)) {
		return false;
	}
	if (b->mark.virtual_chars_col >= 0) {
		col = b->mark.virtual_chars_col;
	}
	if (out_row) {
		*out_row = row;
	}
	if (out_col) {
		*out_col = col;
	}
	return true;
}

bool kg_mark_set_position(struct editor_buffer *b, size_t pos)
{
	if (!b) {
		return false;
	}
	if (kg_marker_set_position(b->mark.marker, pos) == KG_MARKER_OK) {
		b->mark.virtual_chars_col = -1;
		return true;
	}
	b->mark.marker = kg_marker_create(b, pos, KG_MARKER_GRAV_LEFT);
	b->mark.virtual_chars_col = -1;
	return b->mark.marker.id != 0;
}

bool kg_mark_set_row_col(struct editor_buffer *b, int row, int col)
{
	if (!b) {
		return false;
	}
	int virtual_col = -1;

	if (b->numrows > 0 && row >= 0 && row < b->numrows
	    && col > b->row[row].size) {
		virtual_col = col;
	}
	if (!kg_mark_set_position(b, buffer_row_col_to_position(b, row, col))) {
		return false;
	}
	b->mark.virtual_chars_col = virtual_col;
	return true;
}

void kg_mark_clear(struct editor_buffer *b)
{
	if (!b) {
		return;
	}
	kg_marker_delete(b->mark.marker);
	b->mark = (struct kg_buffer_mark) { .virtual_chars_col = -1 };
	b->mark_highlight = 0;
}

void kg_mark_ring_clear(struct editor_buffer *b)
{
	if (!b) {
		return;
	}
	for (int i = 0; i < b->mark_ring_len; i++) {
		kg_marker_delete(b->mark_ring[i].marker);
		b->mark_ring[i]
		    = (struct kg_buffer_mark) { .virtual_chars_col = -1 };
	}
	b->mark_ring_len = 0;
}
