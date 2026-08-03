#ifndef KG_LISP_OBJ_H
#define KG_LISP_OBJ_H

/* Adapter-owned editor objects exposed to Fe, and the runtime execution
 * context that says which buffer the current frame works in.
 *
 * Fe has no per-context custom type facility, so a buffer or marker object
 * is a FeTFex0 value (created with FeMakePtr) wrapping a record in this
 * module's bounded pool.  The record holds the generation-checked handle
 * and is released when Fe collects the wrapper (see lisp_object_gc).  Only
 * the adapter creates FeTFex0 values, so an object cannot be forged from
 * Lisp; the strict type check plus the record/handle identity checks in
 * lisp_buffer_resolve()/lisp_marker_resolve() are what keep stale, foreign
 * and wrong-kind values out -- a marker object handed to a buffer-only
 * native is exactly as much a type error as a string would be.
 *
 * Nothing here may include fe.h: this header is a standalone header-check
 * unit and Fe is reachable only from src/lisp_*.c implementation files.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bufhandle.h"
#include "marker.h"

struct editor_buffer;
struct FeContext;
struct FeObject;

/* Kinds of adapter-owned objects.  One per editor object family. */
enum kg_lisp_object_kind {
	KG_LISP_OBJECT_BUFFER = 0,
	KG_LISP_OBJECT_MARKER = 1,
};

/* One exposed editor object.  The Fe-visible value is a FeTFex0 wrapper
 * whose pointer is this record; the record is active exactly while that
 * wrapper is reachable, and the GC callback frees it when Fe collects the
 * wrapper.
 *
 * `buffer` is meaningful for KG_LISP_OBJECT_BUFFER; `marker` for
 * KG_LISP_OBJECT_MARKER.  Unlike a buffer object's handle, which never
 * changes once minted, a marker object's `marker` is mutable: set-marker
 * moves the same Lisp-visible marker, possibly to a different buffer's
 * kg_marker_store, so the record -- not just what it points at -- is what
 * (eq m1 m2) has to keep meaning the same marker. */
struct kg_lisp_object {
	enum kg_lisp_object_kind kind;
	struct kg_buffer_handle buffer;
	struct kg_marker_handle marker;
	struct FeObject *wrapper; /* the Fe value; non-null iff active */
	bool active;
};

/* How many objects the pool can hold at once.  Records are deduplicated per
 * buffer handle and released when their wrappers die, so with MAX_BUFFERS
 * live buffers the pool can only fill through held stale wrappers; the
 * bound is a defensive ceiling, not a size that user code can reach. */
#define LISP_MAX_OBJECTS 64

struct lisp_object_pool {
	struct kg_lisp_object objects[LISP_MAX_OBJECTS];
};

/* Fe-side GC callback (registered with FeSetGCFn): release a pool record
 * when its FeTFex0 wrapper is collected.  Ignores every other type. */
struct FeObject *lisp_object_gc(struct FeContext *ctx, struct FeObject *obj);

/* The buffer object for `handle`, deduplicated so two asks for the same
 * live buffer answer with the same value (eq-identical).  Raises when the
 * pool is exhausted. */
struct FeObject *lisp_buffer_object(
    struct FeContext *ctx, struct kg_buffer_handle handle);

/* Whether `obj` is a live buffer object of this module's making (strict
 * type and kind check together -- a marker object answers false here,
 * exactly as a string or a cons would). */
bool lisp_object_is_buffer(struct FeContext *ctx, struct FeObject *obj);

/* Whether `obj` is a live marker object of this module's making. */
bool lisp_object_is_marker(struct FeContext *ctx, struct FeObject *obj);

/* The buffer `obj` names, raising on a non-buffer, a stale/foreign record
 * or a killed buffer.  `what` names the failing operation in the error. */
struct editor_buffer *lisp_buffer_resolve(
    struct FeContext *ctx, struct FeObject *obj, const char *what);

/* The handle `obj` names, or a zeroed handle when it is not a live buffer
 * object (buffer-live-p's answer).  Never raises. */
struct kg_buffer_handle lisp_object_buffer_handle(
    struct FeContext *ctx, struct FeObject *obj);

/* A fresh marker object at byte position `pos` of `b`, with the given
 * gravity.  Unlike buffer objects, never deduplicated: (eq (make-marker)
 * (make-marker)) is nil, exactly as two calls to Emacs' make-marker are.
 * Raises when the pool is exhausted or the underlying marker cannot be
 * created (out of memory). */
struct FeObject *lisp_marker_object(struct FeContext *ctx,
    struct editor_buffer *b, size_t pos, enum kg_marker_gravity gravity);

/* The marker handle `obj` names, raising when `obj` is not a marker
 * object at all.  A marker that points nowhere -- never set, detached, or
 * its buffer has died -- is not itself an error: it answers with a handle
 * kg_marker_resolve() reports as KG_MARKER_GONE, which is what
 * (marker-position obj)/(marker-buffer obj) read to answer nil, exactly
 * as Emacs' own detached markers do. */
struct kg_marker_handle lisp_marker_resolve(
    struct FeContext *ctx, struct FeObject *obj, const char *what);

/* Point marker object `obj` at byte position `pos` of `b`, replacing
 * whatever it pointed at before -- including a different buffer's marker
 * store.  Keeps `obj`'s current gravity when it has one (a live or
 * previously-live marker), and defaults to left gravity for a marker that
 * has never been attached, matching make-marker's default insertion
 * type.  Raises when `obj` is not a marker object or moving it runs out
 * of memory. */
void lisp_marker_set(struct FeContext *ctx, struct FeObject *obj,
    struct editor_buffer *b, size_t pos);

/* Detach marker object `obj`: it points nowhere until set-marker moves it
 * again.  Raises when `obj` is not a marker object. */
void lisp_marker_detach(struct FeContext *ctx, struct FeObject *obj);

#endif /* KG_LISP_OBJ_H */
