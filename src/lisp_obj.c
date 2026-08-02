#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../fe/fe.h"
#include "def.h"
#include "edit.h"
#include "lisp_internal.h"
#include "lisp_obj.h"
#include "syntax.h"

/* ---- Adapter-owned editor objects -------------------------------------
 * A buffer object is a FeTFex0 value wrapping a record in state.object_pool.
 * The record is active exactly while its wrapper is reachable; Fe's GC
 * callback releases it.  Only this module creates FeTFex0 values, so a
 * buffer object cannot be forged from Lisp, and resolution re-checks the
 * record (active, wrapper matches) and the generation-checked handle, so a
 * stale or foreign value is caught rather than acted on. */

static struct kg_lisp_object *find_free_record(void)
{
	struct kg_lisp_object *orphan = NULL;
	size_t i;

	for (i = 0; i < LISP_MAX_OBJECTS; i++) {
		struct kg_lisp_object *rec = &state.object_pool.objects[i];

		if (!rec->active) {
			return rec;
		}
		/* Remember an orphaned record: active but with no wrapper,
		 * which is what a creation that ran out of arena leaves
		 * behind.  Unreachable by Fe, so nothing can hold it. */
		if (rec->wrapper == NULL && orphan == NULL) {
			orphan = rec;
		}
	}
	return orphan;
}

static struct kg_lisp_object *find_record_for(struct kg_buffer_handle handle)
{
	size_t i;

	for (i = 0; i < LISP_MAX_OBJECTS; i++) {
		struct kg_lisp_object *rec = &state.object_pool.objects[i];

		if (rec->active && rec->wrapper != NULL
		    && rec->kind == KG_LISP_OBJECT_BUFFER
		    && rec->buffer.slot == handle.slot
		    && rec->buffer.id == handle.id
		    && rec->buffer.generation == handle.generation) {
			return rec;
		}
	}
	return NULL;
}

struct FeObject *lisp_object_gc(struct FeContext *ctx, struct FeObject *obj)
{
	if (FeGetType(obj) == FeTFex0) {
		struct kg_lisp_object *rec = FeToPtr(ctx, obj);

		if (rec != NULL) {
			rec->active = false;
			rec->wrapper = NULL;
		}
	}
	return &nil;
}

struct FeObject *lisp_buffer_object(
    struct FeContext *ctx, struct kg_buffer_handle handle)
{
	struct kg_lisp_object *rec = find_record_for(handle);

	if (rec != NULL) {
		return rec->wrapper;
	}
	rec = find_free_record();
	if (rec == NULL) {
		FeHandleError(ctx, "too many buffer objects");
	}
	rec->kind = KG_LISP_OBJECT_BUFFER;
	rec->buffer = handle;
	rec->active = true;
	rec->wrapper = FeMakePtr(ctx, FeTFex0, rec);
	return rec->wrapper;
}

bool lisp_object_is_buffer(struct FeObject *obj)
{
	return obj != NULL && FeGetType(obj) == FeTFex0;
}

static void lisp_object_error(
    FeContext *ctx, const char *what, const char *detail)
{
	char message[512];

	(void)snprintf(message, sizeof(message), "%s: %s", what, detail);
	FeHandleError(ctx, message);
}

/* The pool record `obj` wraps, or NULL when `obj` is not a live adapter
 * object of this module's making: not a fex0, or the record is inactive
 * or belongs to a different wrapper. */
static struct kg_lisp_object *lisp_object_peek(
    struct FeContext *ctx, struct FeObject *obj)
{
	struct kg_lisp_object *rec;

	if (!lisp_object_is_buffer(obj)) {
		return NULL;
	}
	rec = FeToPtr(ctx, obj);
	if (rec == NULL || !rec->active || rec->wrapper != obj) {
		return NULL;
	}
	return rec;
}

struct editor_buffer *lisp_buffer_resolve(
    struct FeContext *ctx, struct FeObject *obj, const char *what)
{
	struct kg_lisp_object *rec;
	struct editor_buffer *b;

	rec = lisp_object_peek(ctx, obj);
	if (rec == NULL) {
		lisp_object_error(ctx, what, "expected a buffer");
	}
	b = buf_resolve(rec->buffer);
	if (b == NULL) {
		lisp_object_error(ctx, what, "buffer is dead");
	}
	return b;
}

struct kg_buffer_handle lisp_object_buffer_handle(
    struct FeContext *ctx, struct FeObject *obj)
{
	struct kg_lisp_object *rec = lisp_object_peek(ctx, obj);

	if (rec == NULL || buf_resolve(rec->buffer) == NULL) {
		return (struct kg_buffer_handle) { -1, 0, 0 };
	}
	return rec->buffer;
}

/* ---- Runtime execution context: the per-buffer point table -----------
 * Point is per buffer, not per frame (see the struct comment in
 * lisp_internal.h), so it lives in a bounded table keyed by the
 * generation-checked buffer handle, separate from struct kg_lisp_exec_ctx
 * itself.  A buffer's entry, once created, outlives every frame that
 * created or touched it -- markers are buffer-owned, so it needs no
 * frame-scoped cleanup and dies only when its buffer does. */

static bool lisp_buffer_handle_eq(
    struct kg_buffer_handle a, struct kg_buffer_handle b)
{
	return a.slot == b.slot && a.id == b.id && a.generation == b.generation;
}

/* `handle`'s point-table entry, or NULL if it has none yet. */
static struct kg_lisp_point_entry *lisp_point_find(
    struct kg_buffer_handle handle)
{
	size_t i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct kg_lisp_point_entry *e = &state.points.entries[i];

		if (e->active && lisp_buffer_handle_eq(e->buffer, handle)) {
			return e;
		}
	}
	return NULL;
}

/* A free table slot for a buffer that does not have one yet.  The table
 * holds MAX_BUFFERS entries, the same bound as the number of buffers that
 * can be live at once, so an empty slot always exists unless every slot
 * is occupied by a *live* buffer's entry -- in which case the second pass
 * cannot find anything to reclaim either, and this legitimately returns
 * NULL.  Slots pinned by a killed buffer's stale entry are what the
 * second pass reclaims, which is what makes room after a kill. */
static struct kg_lisp_point_entry *lisp_point_free_slot(void)
{
	size_t i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!state.points.entries[i].active) {
			return &state.points.entries[i];
		}
	}
	for (i = 0; i < MAX_BUFFERS; i++) {
		struct kg_lisp_point_entry *e = &state.points.entries[i];

		if (buf_resolve(e->buffer) == NULL) {
			e->active = false;
			return e;
		}
	}
	return NULL;
}

/* Move `handle`'s point entry to byte position `pos`, creating it first if
 * it has none.  Raises out of memory only when creating; moving an
 * existing entry cannot fail. */
static void lisp_point_write(FeContext *ctx, struct editor_buffer *b,
    struct kg_buffer_handle handle, size_t pos)
{
	struct kg_lisp_point_entry *e = lisp_point_find(handle);

	if (e != NULL) {
		(void)kg_marker_set_position(e->point, pos);
		return;
	}
	e = lisp_point_free_slot();
	if (e == NULL) {
		FeHandleError(ctx, "too many buffers with runtime point");
	}
	e->point = kg_marker_create(b, pos, KG_MARKER_GRAV_RIGHT);
	if (e->point.id == 0) {
		FeHandleError(ctx, "out of memory");
	}
	e->buffer = handle;
	e->active = true;
}

/* `handle`'s point entry, creating one at buffer start (point-min) if this
 * is the first time anything has selected `b` -- neither a frame entry nor
 * an earlier set-buffer has touched it yet.  Never moves an existing
 * entry: selecting a buffer is not supposed to move point in it. */
static void lisp_point_ensure(
    FeContext *ctx, struct editor_buffer *b, struct kg_buffer_handle handle)
{
	if (lisp_point_find(handle) != NULL) {
		return;
	}
	lisp_point_write(ctx, b, handle, 0);
}

struct kg_marker_handle lisp_exec_point_marker(void)
{
	struct kg_lisp_point_entry *e = lisp_point_find(state.exec.buffer);

	return e != NULL ? e->point
			 : (struct kg_marker_handle) { { -1, 0, 0 }, 0, 0 };
}

/* ---- Runtime execution context: frame lifecycle ----------------------- */

void lisp_exec_enter(FeContext *ctx)
{
	struct editor_window *w = wcur();
	struct editor_buffer *b = win_buffer(w);
	int row, col;

	if (b == NULL) {
		b = bcur();
	}
	state.exec.buffer = buf_handle_of(b);
	if (state.exec.buffer.slot < 0) {
		/* No current buffer (test harness, or a detached window with
		 * nothing behind it): leave the context empty.  The first
		 * native that needs a buffer raises "current buffer is
		 * dead". */
		memset(&state.exec, 0, sizeof(state.exec));
		return;
	}
	/* The window is the authority for the buffer it displays: the user
	 * may have moved point since the last frame, so this always
	 * overwrites the entry rather than reusing whatever was there. */
	row = editor_current_filerow();
	col = editor_current_filecol();
	lisp_point_write(
	    ctx, b, state.exec.buffer, buffer_row_col_to_position(b, row, col));
}

void lisp_exec_leave(int sync)
{
	if (sync && state.exec.buffer.slot >= 0) {
		struct editor_window *w = wcur();
		struct editor_buffer *b = win_buffer(w);
		struct kg_lisp_point_entry *e
		    = lisp_point_find(state.exec.buffer);
		size_t pos;
		int row, col;

		/* Only the exec buffer's point is synced, and only while the
		 * active window still shows it: hidden work must never move
		 * a displayed window, even the two-window case where the
		 * active window is not the only one showing this buffer. */
		if (b != NULL && e != NULL
		    && win_shows_buffer(w, state.exec.buffer)
		    && kg_marker_resolve(e->point, &pos) == KG_MARKER_OK) {
			buffer_position_to_row_col(b, pos, &row, &col);
			editor_cursor_goto(row, col);
		}
	}
	memset(&state.exec, 0, sizeof(state.exec));
}

void lisp_exec_set_buffer(FeContext *ctx, struct editor_buffer *b)
{
	struct kg_buffer_handle handle = buf_handle_of(b);

	if (lisp_buffer_handle_eq(handle, state.exec.buffer)) {
		return;
	}
	lisp_point_ensure(ctx, b, handle);
	state.exec.buffer = handle;
}

/* ---- Buffer object natives ------------------------------------------- */

FeObject *native_current_buffer(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return lisp_buffer_object(context, state.exec.buffer);
}

/* (buffer-list): the live buffers, current first, the rest in slot order,
 * as Emacs lists the current buffer first. */
FeObject *native_buffer_list(FeContext *context, FeObject *arguments)
{
	FeObject *list = FeNil(context);
	int current = state.exec.buffer.slot;
	int i;

	FeRequireNoArguments(context, arguments);
	for (i = MAX_BUFFERS - 1; i >= 0; i--) {
		if (i == current) {
			continue;
		}
		if (buflist[i].active) {
			list = FeCons(context,
			    lisp_buffer_object(context, buf_handle(i)), list);
		}
	}
	if (current >= 0 && current < MAX_BUFFERS && buflist[current].active) {
		list = FeCons(context,
		    lisp_buffer_object(context, buf_handle(current)), list);
	}
	return list;
}

/* The name a buffer answers to: its display name, the same string
 * (buffer-name) returns. */
static int lisp_buffer_slot_by_name(const char *name)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		char display[PATH_MAX];

		if (!buflist[i].active) {
			continue;
		}
		buf_display_name(i, display, sizeof(display));
		if (strcmp(display, name) == 0) {
			return i;
		}
	}
	return -1;
}

static char *lisp_name_argument(
    FeContext *context, FeObject *object, char *out, size_t outsize)
{
	char *text;
	size_t length;

	text = copy_fe_string(context, object, &length);
	if (length == 0 || length >= outsize) {
		free(text);
		FeHandleError(context, "invalid buffer name");
	}
	memcpy(out, text, length + 1);
	free(text);
	return out;
}

FeObject *native_get_buffer(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char name[PATH_MAX];
	int slot;

	FeRequireNoArguments(context, arguments);
	lisp_name_argument(context, object, name, sizeof(name));
	slot = lisp_buffer_slot_by_name(name);
	return slot >= 0 ? lisp_buffer_object(context, buf_handle(slot))
			 : FeNil(context);
}

FeObject *native_get_buffer_create(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char name[PATH_MAX];
	struct kg_buffer_handle handle;
	int slot;

	FeRequireNoArguments(context, arguments);
	lisp_name_argument(context, object, name, sizeof(name));
	slot = lisp_buffer_slot_by_name(name);
	if (slot >= 0) {
		return lisp_buffer_object(context, buf_handle(slot));
	}
	handle = buf_create_named(name);
	if (handle.slot < 0) {
		FeHandleError(context, "too many open buffers");
	}
	return lisp_buffer_object(context, handle);
}

FeObject *native_buffer_live_p(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);

	FeRequireNoArguments(context, arguments);
	return FeMakeBool(
	    context, lisp_object_buffer_handle(context, object).slot >= 0);
}

FeObject *native_set_buffer(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b;

	FeRequireNoArguments(context, arguments);
	b = lisp_buffer_resolve(context, object, "set-buffer");
	lisp_exec_set_buffer(context, b);
	return FeNil(context);
}

/* (kill-buffer [buf]): refuse modified unsaved buffers without prompting.
 * Killing the current exec buffer leaves its handle stale, so the rest of
 * the frame's operations on it raise "buffer is dead"; the window that
 * showed it was detached by the kill. */
FeObject *native_kill_buffer(FeContext *context, FeObject *arguments)
{
	struct kg_buffer_handle handle;
	struct editor_buffer *b;
	FeObject *object = NULL;

	if (!FeIsNil(arguments)) {
		object = FeGetNextArgument(context, &arguments);
	}
	FeRequireNoArguments(context, arguments);
	if (object == NULL) {
		b = lisp_exec_buffer(context);
		handle = buf_handle_of(b);
	} else {
		b = lisp_buffer_resolve(context, object, "kill-buffer");
		handle = buf_handle_of(b);
	}
	if (b->dirty) {
		FeHandleError(context, "kill-buffer: buffer is modified");
	}
	if (!buf_kill_buffer(handle)) {
		FeHandleError(context, "kill-buffer: cannot kill buffer");
	}
	return FeNil(context);
}
