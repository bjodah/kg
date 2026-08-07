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

#include <stddef.h>

#include "bufhandle.h"
#include "marker.h"
#include "prochandle.h"

struct editor_buffer;
struct FeContext;
struct FeObject;

/* Kinds of adapter-owned objects.  One per editor object family. */
enum kg_lisp_object_kind {
	KG_LISP_OBJECT_BUFFER = 0,
	KG_LISP_OBJECT_MARKER = 1,
	KG_LISP_OBJECT_PROCESS = 2,
};

/* One exposed editor object.  The Fe-visible value is a FeTFex0 wrapper
 * whose pointer is this record; the record is active exactly while that
 * wrapper is reachable, and the GC callback frees it when Fe collects the
 * wrapper.
 *
 * `buffer` is meaningful for KG_LISP_OBJECT_BUFFER; `marker` for
 * KG_LISP_OBJECT_MARKER; `process` for KG_LISP_OBJECT_PROCESS.  Unlike a
 * buffer object's handle, which never changes once minted, a marker
 * object's `marker` is mutable: set-marker moves the same Lisp-visible
 * marker, possibly to a different buffer's kg_marker_store, so the record
 * -- not just what it points at -- is what (eq m1 m2) has to keep meaning
 * the same marker.  A process object's handle never changes once minted,
 * like a buffer object's. */
struct kg_lisp_object {
	enum kg_lisp_object_kind kind;
	struct kg_buffer_handle buffer;
	struct kg_marker_handle marker;
	struct kg_process_handle process;
	struct FeObject *wrapper; /* the Fe value; non-null iff active */
	bool active;
};

/* How many objects the pool can hold at once.  Records are deduplicated per
 * buffer handle and released when their wrappers die, so with MAX_BUFFERS
 * live buffers the pool can only fill through held stale wrappers; the
 * bound is a defensive ceiling, not a size that user code can reach.
 *
 * "At once" is load-bearing, and the pool has no back-pressure to hold it
 * up: `find_free_record()` raises when every record is taken, and nothing
 * here can ask fe to collect (fe publishes no collect-now entry point), so
 * a workload that allocates no arena never provokes the sweep that would
 * free the wrappers it has dropped.  An adapter object minted for the
 * adapter's own private use therefore has to be released when its owner
 * knows it is dead rather than when the collector gets to it -- see
 * lisp_marker_release() and src/lisp_buffer.c's excursion restore. */
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

/* Whether `obj` is a live process object of this module's making. */
bool lisp_object_is_process(struct FeContext *ctx, struct FeObject *obj);

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

/* Release marker object `obj`'s pool record NOW, without waiting for the
 * collector to sweep its wrapper: delete the underlying marker and hand
 * the record back to the pool.  Only for a marker the adapter minted for
 * its own private use and knows is dead -- an excursion's saved state
 * after its restore -- never for one a user's variable might still name,
 * since `obj` keeps pointing at a record that is now free to be reused.
 * That is safe but not silent: `lisp_object_peek()`'s wrapper-identity
 * check answers "not a live object" for the released wrapper afterwards,
 * whether the record has been reused or not.  Does nothing when `obj` is
 * not a live marker object -- calling it twice is not an error.
 *
 * Deterministic release is what keeps the pool bound a bound on how many
 * objects are live AT ONCE rather than on how many a run may create: a
 * loop that saves an excursion 5000 times holds one record at a time. */
void lisp_marker_release(struct FeContext *ctx, struct FeObject *obj);

/* ---- Process objects: the pool's third kind ---------------------------
 * Deduplicated like a buffer object -- (eq p1 p2) for two asks naming the
 * same table slot and generation -- since a process, like a buffer, is a
 * single long-lived thing a caller looks up repeatedly rather than
 * something fresh minted every call the way a marker is. */

/* The process object for `handle`, deduplicated so two asks for the same
 * table entry answer with the same (eq) value.  Raises when the pool is
 * exhausted. */
struct FeObject *lisp_process_object(
    struct FeContext *ctx, struct kg_process_handle handle);

/* The process handle `obj` names, raising when `obj` is not a process
 * object at all.  A handle naming a finished or released process is not
 * itself an error -- src/process_table.h's own resolution answers that,
 * matching lisp_marker_resolve()'s detached-marker precedent. */
struct kg_process_handle lisp_process_resolve(
    struct FeContext *ctx, struct FeObject *obj, const char *what);

/* The buffer handle BUFFER-OR-NAME names, for start-process's and
 * start-shell-command's BUFFER argument: a live buffer object resolves
 * directly; a string is get-buffer-create's own name resolution (an
 * existing buffer of that name, else a fresh one); nil answers a handle
 * naming nothing, which is what a process spawned with no target buffer
 * means -- its output is discarded, same as a killed process-buffer's.
 * Raises when `object` is anything else, or buffer creation fails. */
struct kg_buffer_handle lisp_buffer_or_name_or_nil(
    struct FeContext *ctx, struct FeObject *object, const char *what);

#endif /* KG_LISP_OBJ_H */
