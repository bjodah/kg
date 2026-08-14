#ifndef KG_DAP_JAVA_H
#define KG_DAP_JAVA_H

#include <stddef.h>

struct dap_adapter_spec; /* dap_config.h; only ever pointed at here */
struct dap_session_request; /* dap_session.h; likewise */
struct kg_json_value; /* json.h; likewise */

/* Everything about debugging Java that is true of NBCODE rather than of the
 * Debug Adapter Protocol, kept in one module so none of it leaks downwards.
 *
 * The generic client (src/dap_client.h) and the generic session
 * (src/dap_session.h) are what every adapter gets.  This layer sits ABOVE
 * both -- it starts sessions, it reads their events after the model has,
 * and it ends them -- and it is where two nbcode-shaped facts live that
 * must never be generalised:
 *
 * ONE.  THE ADAPTER IS NOT A COMMAND, IT IS AN ENDPOINT.  There is no
 * `nbcode --dap` to spawn.  The Java debug adapter is a second server the
 * Java LANGUAGE server's process announces beside itself, so starting a
 * session is a sequence of steps that can each take seconds and each fail
 * differently: resolve the file, find or start the language server, wait
 * for it to be `initialized`, obtain the endpoint, connect and hand over
 * the handshake secret.  Only then does an ordinary DAP session begin.
 *
 * Each step is named (dap_java_step_text()), so the status line says which
 * one a slow start is in rather than saying nothing for twenty seconds; the
 * whole thing is cancellable at any step; and none of it blocks -- the
 * machine advances from dap_java_poll(), on the editor's own poll.
 *
 * The ordering is a hard dependency and not a nicety: the debug connection
 * captures the language session's state when it is CONSTRUCTED, so a socket
 * opened before the language server is initialized gets a session that is
 * not there yet.
 *
 * kg never owns that process.  The language server started it, the language
 * server ends it, and disconnecting a debug session closes ONE SOCKET --
 * the user's Java server is still their Java server afterwards.  What this
 * layer does watch for is the opposite direction: the owner dying under a
 * live debug session, which makes the session dead because its endpoint is.
 *
 * TWO.  NBCODE NEVER SENDS `terminated`, AND NEVER SENDS `exited`.  Waited
 * out to sixty seconds past program completion (doc/plans/dap/03-java.md).
 * A generic client cannot paper over that: `thread{reason:"exited"}` in the
 * protocol means one thread of many ended, and a client that read it as
 * "the session is over" would end every session at the first worker thread
 * that finished.  So the inference is here, it is conservative, and it is
 * cancelled by any sign of life.
 *
 * See dap_java_event() for the shape of that inference and for the measured
 * JVM behaviour it has to work around.
 */

/* Where a starting session has got to.  Ordered, and every one of them can
 * be the last: cancelling and failing are both possible at each. */
enum dap_java_step {
	DAP_JAVA_IDLE = 0,
	/* Asking src/lsp.h for the sibling endpoint, which starts the
	 * language server if nothing is running for this file's workspace.
	 * The step covers the wait for `initialized` and the wait for the
	 * announce, because they are one question asked repeatedly. */
	DAP_JAVA_RESOLVING,
	/* An ordinary DAP session exists and is doing its own handshake. */
	DAP_JAVA_RUNNING,
};

/* How long after the last live debuggee thread disappears kg waits before
 * concluding that a session nbcode will never say `terminated` about is
 * over.
 *
 * Two seconds because it is a budget for LATE OUTPUT rather than for the
 * program: measured, the debuggee's own stdout keeps arriving after the
 * `thread{exited}` that reports its main thread ending, and an inference
 * that disconnected the instant the thread went would truncate the last
 * lines the user wanted to read.  Anything that proves the session is alive
 * -- a new thread, a stop -- cancels it outright rather than extending it. */
#define DAP_JAVA_TEARDOWN_GRACE_MS 2000u

/* Whether `spec` is one this module has to start, i.e. whether starting it
 * means asking the LSP layer for an endpoint rather than spawning a
 * command.  The command layer asks this instead of testing the transport
 * kind itself, so the two never drift. */
bool dap_java_owns(const struct dap_adapter_spec *spec);

/* Begin the sequence for `request`, whose adapter dap_java_owns().
 * `filename` is the buffer's own path, which is what resolves the workspace
 * root -- taken as a path rather than read from a buffer because this
 * module, like everything below it, links without the editor.  A relative
 * one is made absolute against kg's working directory, which is what such
 * a path means; an empty one is a buffer visiting no file and is refused.
 *
 * Returns 0 when the sequence has begun and -1 with `error` filled in when
 * it could not, which is the same contract dap_session_start() has, so the
 * command layer's failure path is one path.  Nothing is spawned, connected
 * or blocked on here; the first real work happens in dap_java_poll().
 *
 * The `request` and its hooks are COPIED, arguments included: the caller
 * frees its expanded arguments as soon as this returns, exactly as it does
 * for dap_session_start(). */
int dap_java_start(const struct dap_session_request *request,
    const char *filename, char *error, size_t error_size);

/* Advance a starting sequence, and run the teardown inference for a
 * running one.  Called from dap_poll(); nonzero means a repaint is wanted,
 * lsp_poll()'s convention. */
int dap_java_poll(void);

/* Abandon a sequence that has not produced a session yet.  C-g, and the
 * reason every step is individually cancellable: a language server that is
 * importing a large project can leave RESOLVING for a long time, and a user
 * must be able to stop waiting.  Harmless when nothing is starting. */
void dap_java_cancel(void);

/* Every event the session forwarded, AFTER the execution model has had it.
 * Installed as the session's event hook for a Java session only, with
 * dap_exec_event() called from inside it -- so the generic model sees
 * exactly what it always saw, and this layer sees it too. */
void dap_java_event(
    void *ctx, const char *event, const struct kg_json_value *body);

/* The session ended, however it ended: drop the inference state so the
 * next session starts from nothing. */
void dap_java_session_ended(void);

/* What the sequence is doing, as a sentence for the status line, or NULL
 * when nothing is starting. */
const char *dap_java_step_text(void);
enum dap_java_step dap_java_step(void);

/* The teardown grace, overridable so a test does not wait one out.  Zero
 * means "already expired", which is what the tests use. */
void dap_java_set_grace_ms(unsigned grace_ms);

/* The sibling endpoint, from the caller rather than from the language
 * server: `host` and `port` something is listening on and the handshake
 * `secret` it wants, or a NULL `host` to go back to asking src/lsp.h.
 * `secret` is a string, and is only read when `host` is not NULL.
 *
 * TEST ONLY, and it exists for one reason: the teardown inference lives in
 * the RUNNING step, and the only other road to that step is a real nbcode
 * with a real JVM under it.  A fixture that set this points the ordinary
 * start sequence -- the same session, the same secret, the same socket --
 * at a fake adapter instead.  Every ask is answered OK with one unchanging
 * generation, so the owner check in dap_java_poll() finds the owner still
 * there and the case is about the inference and nothing else. */
void dap_java_set_endpoint_for_test(
    const char *host, unsigned short port, const char *secret);

/* Told when the wait finally produced a session, because the two things
 * that have to happen then -- the panes opening and the layout being
 * arranged -- are the editor's and this module links without one.  It is a
 * pointer for the link reason src/dap_core.c's ui_poll is: the command
 * layer sets it, and a suite with no editor in it leaves it NULL and sees
 * no panes, which is exactly what such a suite wants. */
void dap_java_set_session_hook(void (*fn)(void));

#endif /* KG_DAP_JAVA_H */
