#ifndef KG_ASYNC_H
#define KG_ASYNC_H

/* The editor-facing aggregate of asynchronous protocol work.
 *
 * The two operations stay deliberately small: wait_fds() says what may
 * wake the input wait, and poll() services every subsystem once.  They do
 * not drain the editor event queue.  In particular, a caller inside a
 * minibuffer prompt must not turn this into a hidden safe point; actions
 * that cannot run under a prompt remain queued until the top-level loop.
 *
 * KG_ASYNC_WAIT_FDS_MAX is the exact compile-time sum of the subsystem
 * bounds (checked in async.c).  A destination smaller than that could make
 * a live subsystem disappear from the wait, so it is a programmer error:
 * editor_async_wait_fds() returns -1 without writing anything when `fds`
 * is NULL or `max` is smaller than this bound (including a negative max).
 * A valid full-sized destination and no live subsystem returns zero, and a
 * valid full-sized destination never returns -1: a subsystem that broke
 * its own bound is an assertion inside, because the alternative is a wait
 * with no subsystem descriptor in it and nothing said about why.
 *
 * Wait descriptors are readable only.  A protocol connection waiting for
 * writability -- a nonblocking connect in progress or a backed-up outbox --
 * therefore advances on the editor's 100 ms idle tick rather than waking
 * the wait itself.  Generalising the seam to carry poll events is a later
 * change, not a hidden exception here.
 */
#define KG_ASYNC_WAIT_FDS_MAX 14

int editor_async_wait_fds(int *fds, int max);

/* Service each asynchronous subsystem once.  Nonzero means something
 * user-visible changed and the caller should repaint. */
int editor_async_poll(void);

#endif /* KG_ASYNC_H */
