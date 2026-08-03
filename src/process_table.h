#ifndef KG_PROCESS_TABLE_H
#define KG_PROCESS_TABLE_H

/* A bounded table of child processes kg is tracking beyond compile.c's and
 * shell.c's own single-process state -- the backend for a Lisp
 * `start-process`/`start-shell-command` (a later slice; nothing here is
 * gated on KG_USE_LISP, since a process outlives no Lisp value more than it
 * outlives a C one).
 *
 * The whole point of splitting this from src/event.h's queue is the split
 * kg's main loop already has between an unsafe poll point and a handful of
 * safe ones (see src/main.c's loop and src/tty.c's read_key_byte()):
 *
 *   kg_process_table_poll()   fd -> this table's per-process bounded queue.
 *                              Non-blocking reads and reaps only.  No
 *                              buffer write, no Lisp, no allocation beyond
 *                              the fixed queue every entry already owns --
 *                              safe to call from anywhere, including a
 *                              point that is not a safe point.  Publishes
 *                              KG_EVENT_PROCESS_OUTPUT/_EXIT; never
 *                              delivers them.
 *
 *   the drain subscriber      queue -> the process's target buffer, via
 *   this module registers     kg_buffer_replace() with KG_EDIT_INTERNAL, at
 *   with kg_event_subscribe() the end of the buffer.  Runs only from
 *                              kg_event_drain_safe(), i.e. only at
 *                              src/main.c's three named safe points.
 *
 * Never expose a pid as identity outside this module: struct
 * kg_process_handle (src/prochandle.h) is the only identity token a caller
 * gets back, generation-checked the same way a buffer or window handle is,
 * so a stale handle to a released slot never resolves to whatever process
 * that slot holds next. */

#include "bufhandle.h"
#include "prochandle.h"
#include <stddef.h>

struct kg_spawn_request;

/* Live processes this table tracks at once.  A finished process keeps its
 * slot -- and stays queryable -- until kg_process_table_release() (or, once
 * a later slice adds it, delete-process) frees it, or until a spawn with
 * the table full reclaims the oldest finished, already-published entry;
 * see kg_process_table_spawn(). */
#define KG_PROCESS_TABLE_MAX 8

/* Bytes of not-yet-delivered output one process may hold.  Reached only
 * when a process outruns the drain -- output is committed to its buffer at
 * every safe point.  Overflow drops the *oldest* queued bytes and sets a
 * truncation flag; the drain subscriber turns that into one
 * "kg: output truncated" marker line per overflow run, immediately before
 * the bytes it precedes, never one line per dropped chunk. */
#define KG_PROCESS_OUTPUT_MAX (256 * 1024)

/* What became of a process, decoded the way struct kg_process_status is but
 * collapsed to the three states a caller (eventually `process-status`)
 * actually asks about: still running, or finished one of the two ways a
 * child can finish. */
enum kg_process_run_status {
	KG_PROCESS_RUNNING,
	KG_PROCESS_EXITED,
	KG_PROCESS_SIGNALED,
};

/* `code` is the exit code when `status` is KG_PROCESS_EXITED, the signal
 * number when it is KG_PROCESS_SIGNALED, and meaningless (left at 0) while
 * still KG_PROCESS_RUNNING. */
struct kg_process_table_info {
	enum kg_process_run_status status;
	int code;
};

/* Claim a slot and spawn `req` into it, targeting `buffer` for its output.
 * Returns a live handle, or a zeroed one (kg_process_table_resolves() false)
 * when: the table already holds KG_PROCESS_TABLE_MAX processes with none of
 * them a finished, already-published entry to reclaim; kg_process_spawn()
 * itself refuses `req` (see process.h, including the argv-and-command
 * case); or the queue's own allocation fails, in which case the child that
 * was already forked is killed and reaped rather than left untracked. */
struct kg_process_handle kg_process_table_spawn(
    const struct kg_spawn_request *req, struct kg_buffer_handle buffer);

/* Move bytes fd -> queue for every active entry, and reap whichever have
 * finished.  Publishes KG_EVENT_PROCESS_OUTPUT when an entry's poll read
 * anything, and KG_EVENT_PROCESS_EXIT once when an entry is reaped --
 * retried on every later call until the reservation succeeds, since an
 * exit event is lifecycle and must never be silently dropped.  Touches no
 * buffer and calls no Lisp, so it is safe from a point that is not one of
 * src/event.h's safe points. */
void kg_process_table_poll(void);

/* Whether `handle` still names a live table entry -- running, or finished
 * and not yet released.  What src/event.c's process resolution arm asks. */
bool kg_process_table_resolves(struct kg_process_handle handle);

/* `handle`'s status.  Returns false (and leaves `*out` untouched) once the
 * handle no longer resolves; `out` may be NULL to just ask whether it
 * still does. */
bool kg_process_table_query(
    struct kg_process_handle handle, struct kg_process_table_info *out);

/* Free `handle`'s slot: its output queue and its output descriptor (already
 * closed by the reap that finished it, but this is what makes that not
 * load-bearing).  Refused (returns false, nothing freed) for a handle that
 * does not resolve, or names a process still running -- kill it first.
 * `handle` never resolves again after this, even to a later occupant of
 * the same slot: the slot's generation is not reused. */
bool kg_process_table_release(struct kg_process_handle handle);

/* delete-process's escalation: SIGTERM to the process group, a bounded wait
 * for the reap, SIGKILL and a second bounded wait if the first did not
 * finish it, then kg_process_table_release().  Never blocks the editor
 * indefinitely -- each wait is a fixed number of 1 ms ticks -- so a child
 * that ignores both signals leaves the slot occupied (running) rather than
 * hanging the caller; that shows up as a false return.  A handle that
 * already names a finished, unreleased entry just releases it, matching
 * kg_process_table_release() with no signal sent.  Refused (false, nothing
 * changed) for a handle that does not resolve at all. */
bool kg_process_table_terminate_and_release(struct kg_process_handle handle);

/* Mark `handle`'s entry as filter-owned (or not).  While true, the drain
 * subscriber this module registers leaves the queued output alone instead
 * of auto-appending it to the target buffer -- src/lisp_process.c's own
 * subscriber is the one that delivers it, to whatever Lisp filter function
 * is set.  Refused (false) for a handle that does not resolve. */
bool kg_process_table_set_has_filter(
    struct kg_process_handle handle, bool has_filter);

/* `handle`'s target buffer handle (process-buffer's answer, before
 * resolving it to a live buffer or an object), or a handle naming nothing
 * for one that does not resolve. */
struct kg_buffer_handle kg_process_table_buffer(
    struct kg_process_handle handle);

/* Hand back `handle`'s queued output as an owned, NUL-terminated buffer
 * (malloc'd; the caller frees it) and clear the queue -- the same
 * "kg: output truncated" marker the auto-append path would write is
 * prefixed here exactly once when the drop-oldest policy owes one.  `*len`
 * gets the byte length, excluding the added NUL (the content itself may
 * still embed one, same limitation FeMakeString(3) already has throughout
 * the adapter).  Returns NULL, `*len` left at 0, and the queue untouched
 * when nothing is queued, `handle` does not resolve, or allocation failed
 * (so a later delivery gets another chance rather than losing the bytes).
 * For a filter to consume queued output; the auto-append drain subscriber
 * in this file does not use it, to keep its existing success/failure
 * handling exactly as it was before this native-facing accessor existed. */
char *kg_process_table_take_output(
    struct kg_process_handle handle, size_t *len);

/* Kill every tracked process's group, reap what a bounded wait can, and
 * release every slot regardless of whether it finished on its own.  Wired
 * into the same teardown compilation_shutdown() already runs from
 * (src/bufmgr.c's editor_cleanup()); no child of kg's may outlive it. */
void kg_process_table_shutdown(void);

/* Arm the fd invariant every slot needs before it is ever touched (an
 * unclaimed slot's descriptor reads as -1, never the zero a static
 * initializer would leave it at, which is stdin) and register the drain
 * subscriber that commits queued output to its target buffer.  Idempotent;
 * called once from init_editor(). */
void kg_process_table_init(void);

/* Test-only seam: append `bytes` to `handle`'s output queue exactly as a
 * real fd read would, exercising the drop-oldest/truncation-flag policy
 * without needing a real child to produce KG_PROCESS_OUTPUT_MAX bytes of
 * output.  A no-op for a handle that does not resolve.  Nothing outside
 * test/ calls this. */
void kg_process_table_test_append_output(
    struct kg_process_handle handle, const char *bytes, size_t len);

#endif /* KG_PROCESS_TABLE_H */
