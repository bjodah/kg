#ifndef KG_LISP_OBJ_H
#define KG_LISP_OBJ_H

/* Adapter-owned editor objects exposed to Fe, and the runtime execution
 * context that says which buffer the current frame works in.
 *
 * Fe has no per-context custom type facility, so a buffer object is a
 * FeTFex0 value (created with FeMakePtr) wrapping a record in this module's
 * bounded pool.  The record holds the generation-checked buffer handle and
 * is released when Fe collects the wrapper (see lisp_object_gc).  Only the
 * adapter creates FeTFex0 values, so a buffer object cannot be forged from
 * Lisp; the strict type check plus the record/handle identity checks in
 * lisp_buffer_resolve() are what keep stale and foreign values out.
 *
 * Nothing here may include fe.h: this header is a standalone header-check
 * unit and Fe is reachable only from src/lisp_*.c implementation files.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bufhandle.h"

struct editor_buffer;
struct FeContext;
struct FeObject;

/* Kinds of adapter-owned objects.  One per editor object family; buffer is
 * the only family Phase 2 exposes. */
enum kg_lisp_object_kind {
	KG_LISP_OBJECT_BUFFER = 0,
};

/* One exposed editor object.  The Fe-visible value is a FeTFex0 wrapper
 * whose pointer is this record; the record is active exactly while that
 * wrapper is reachable, and the GC callback frees it when Fe collects the
 * wrapper. */
struct kg_lisp_object {
	enum kg_lisp_object_kind kind;
	struct kg_buffer_handle buffer;
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

/* Whether `obj` is a buffer object at all (strict type check). */
bool lisp_object_is_buffer(struct FeObject *obj);

/* The buffer `obj` names, raising on a non-buffer, a stale/foreign record
 * or a killed buffer.  `what` names the failing operation in the error. */
struct editor_buffer *lisp_buffer_resolve(
    struct FeContext *ctx, struct FeObject *obj, const char *what);

/* The handle `obj` names, or a zeroed handle when it is not a live buffer
 * object (buffer-live-p's answer).  Never raises. */
struct kg_buffer_handle lisp_object_buffer_handle(
    struct FeContext *ctx, struct FeObject *obj);

#endif /* KG_LISP_OBJ_H */
