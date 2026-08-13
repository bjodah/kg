#ifndef KG_DAP_BREAKPOINT_H
#define KG_DAP_BREAKPOINT_H

#include <stddef.h>

/* The breakpoint table: where a breakpoint LIVES, which is the editor and
 * not a session.
 *
 * This is the dap-mode lesson that matters most (dap-mode.el:325-331 keeps
 * sessions inside lsp-mode's workspace, and breakpoints with them): a
 * breakpoint is a place in a file the user marked, so it exists with no
 * adapter running, it survives the session that verified it, and a session
 * starting is an occasion to TELL an adapter about the table rather than an
 * occasion to own it.  Nothing below needs a session; everything below
 * notices one when there is one.
 *
 * WHERE A BREAKPOINT IS, is a sum type [M-9, M-11].  In a live buffer it is
 * a RIGHT-gravity marker at the first byte of the line, so an insertion at
 * exactly that byte -- typing a newline above the statement -- pushes the
 * marker past the inserted text and the breakpoint stays with the code it
 * was set on.  For a file nobody has open it is `(path, line)`, a plain
 * number.  The two cross over on the buffer lifecycle events: KILLING
 * demotes a marker to a line, OPENED re-anchors a line to a marker
 * (dape.el:3482-3543's overlay-to-cons round trip).  Deleting the whole
 * line a breakpoint sits on is the one edit with no obviously right answer,
 * and kg's decided, tested policy is to RETAIN it at the resulting line --
 * the marker is already at the start of what follows, and dropping a
 * breakpoint because a line moved out from under it is the behaviour users
 * report as lost breakpoints.
 *
 * The key is the `realpath()`ed absolute path, because that is what
 * `setBreakpoints` names a source with [M-11] and what makes two spellings
 * of one file one row rather than two.  The path the user typed is retained
 * beside it for display and for nothing else.  A buffer that visits no
 * existing file -- a scratch buffer, a listing, a file not yet written --
 * refuses a breakpoint and says why: there is no source for an adapter to
 * name.
 *
 * AT MOST ONE `setBreakpoints` IN FLIGHT PER SOURCE, which is the whole of
 * how this module talks to an adapter.  Each source carries
 * `desired_generation` (bumped by every mutation), `sent_generation`,
 * `request_in_flight` and `dirty`.  A snapshot goes out only when nothing
 * is in flight; a mutation during a flight sets `dirty` and nothing else; a
 * response applies to ITS OWN SNAPSHOT and never by indexing the live
 * vector (dape.el:1760-1762's hazard); and a dirty source re-sends the
 * newest FULL snapshot the moment the flight ends.  Removing the last
 * breakpoint sends the empty array, with the source's path resolved before
 * the last marker dies, because `setBreakpoints` replaces a source's whole
 * set and the empty set is the only way to say "none".
 *
 * IDS ARE RE-KEYED AFTER EVERY SUCCESSFUL SET.  Both measured adapters
 * hand out fresh ids on a re-send, debugpy even for an unchanged line, so
 * an id is a per-set fact and never an identity; it is stored as
 * `has_id + id` because debugpy's first id is 0.  The RETURNED line is what
 * renders -- debugpy silently relocates an out-of-range line and calls it
 * verified -- and a response element that is missing or malformed leaves
 * its snapshot entry unverified and is reported rather than guessed at.
 *
 * `breakpoint` EVENTS update the local projection by id and are
 * idempotent (lldb-dap fires `changed` right after every set response);
 * they NEVER trigger a `setBreakpoints` by themselves, or an adapter that
 * fires changed-after-every-set and a client that re-sends on every change
 * make an id-churn loop between them.
 */

/* The same two bounds the session's configuration join carries
 * (DAP_SESSION_MAX_SOURCES / DAP_SESSION_MAX_SOURCE_LINES), because a table
 * that could hold more than the join can send would be a table whose extra
 * rows are silently never set.  src/dap_breakpoint.c asserts they agree. */
#define DAP_BREAKPOINT_MAX_SOURCES 32
#define DAP_BREAKPOINT_MAX_PER_SOURCE 64

/* One wire string as kg keeps it: a condition, a hit condition, a log
 * message, or the adapter's own sentence about why a breakpoint is not
 * verified.  Bounded for the same reason struct dap_error's text is. */
#define DAP_BREAKPOINT_TEXT_MAX 128

struct editor_buffer;
struct kg_json_value; /* src/json.h */
struct dap_session_source; /* src/dap_session.h */

/* Why a breakpoint was refused.  Every one of them is a sentence a command
 * can put in the echo area unchanged (dap_breakpoint_result_text()), which
 * is what keeps the wording in one place instead of at each call site. */
enum dap_breakpoint_result {
	DAP_BREAKPOINT_OK = 0,
	/* The buffer visits no existing file: nothing an adapter can name. */
	DAP_BREAKPOINT_NO_FILE,
	DAP_BREAKPOINT_BAD_LINE,
	DAP_BREAKPOINT_FULL,
	DAP_BREAKPOINT_NO_MEMORY,
};

const char *dap_breakpoint_result_text(enum dap_breakpoint_result result);

/* What a breakpoint carries on the wire beyond its line.  All three are
 * capability-gated at send (`supportsConditionalBreakpoints`,
 * `supportsHitConditionalBreakpoints`, `supportsLogPoints`): an adapter
 * that cannot do one is not asked, rather than being sent a member it will
 * either ignore or refuse the whole source over.  Their UI is a follow-on;
 * the fields ship now so the table never has to grow a column later.
 *
 * `temporary` is the one with an algorithm rather than a member: see
 * dap_breakpoint_arm_temporary(). */
struct dap_breakpoint_options {
	bool temporary;
	const char *condition; /* NULL or "" for none */
	const char *hit_condition;
	const char *log_message;
};

/* A breakpoint, copied out.  A caller never holds a pointer into the table:
 * the next mutation may move a row, and the next poll may re-key every id.
 *
 * `line` is what RENDERS -- the adapter's returned line when it answered
 * one for the line kg last sent, and the anchor's own line otherwise, so an
 * edit that moves the marker takes the display back to the marker until the
 * next round trip agrees.  `requested_line` is what kg asked for. */
struct dap_breakpoint_info {
	int line;
	int requested_line;
	bool verified;
	bool has_id;
	int id;
	bool temporary;
	/* False for a breakpoint kg keeps and the adapter is not told
	 * about: `setBreakpoints` replaces the whole set per source, so
	 * leaving it out of the array is the whole of "disabled". */
	bool enabled;
	bool anchored; /* a live marker, rather than a remembered line */
	char condition[DAP_BREAKPOINT_TEXT_MAX];
	char hit_condition[DAP_BREAKPOINT_TEXT_MAX];
	char log_message[DAP_BREAKPOINT_TEXT_MAX];
	char message[DAP_BREAKPOINT_TEXT_MAX]; /* the adapter's own reason */
};

/* Subscribe to the buffer lifecycle.  Called from dap_init() before the
 * init file is read, and by any test that wants the wiring back after a
 * kg_event_queue_init(); idempotent, in lsp_sync_install()'s own shape. */
void dap_breakpoint_init(void);

/* Where this module's own sentences go.  Unset, they go to the echo area
 * like every other one; a test installs a hook because "was it reported"
 * is half of what several of the rules here promise. */
void dap_breakpoint_set_report_hook(
    void (*hook)(void *ctx, bool error, const char *text), void *ctx);

/* Drop the whole table and unsubscribe: editor shutdown, and every test's
 * setup.  Markers are deleted from whatever buffers still hold them. */
void dap_breakpoint_reset(void);

/* Set a breakpoint on `row` (0-based, the editor's own row space) of a
 * buffer, or on `line` (1-based, the protocol's) of a path.  `opts` may be
 * NULL for an ordinary one.  Setting one where one already is updates its
 * options rather than making a second.
 *
 * The path form does not need the file open, but does anchor it to a marker
 * if some buffer is visiting that file: one file is one source however the
 * user got there. */
enum dap_breakpoint_result dap_breakpoint_add(struct editor_buffer *b, int row,
    const struct dap_breakpoint_options *opts);
enum dap_breakpoint_result dap_breakpoint_add_at(
    const char *path, int line, const struct dap_breakpoint_options *opts);

/* Remove one, answering whether there was one.  The source's empty array
 * goes out from here while the path is still resolvable. */
bool dap_breakpoint_remove(struct editor_buffer *b, int row);
bool dap_breakpoint_remove_at(const char *path, int line);

/* `dap-breakpoint-toggle`'s half that is not the command: `*added` says
 * which way it went, and is only written on DAP_BREAKPOINT_OK. */
enum dap_breakpoint_result dap_breakpoint_toggle(
    struct editor_buffer *b, int row, bool *added);

/* The table, for the panes and the decorations of later stages, and for
 * every assertion here.  A source index is stable only until the next
 * mutation. */
size_t dap_breakpoint_count(void);
size_t dap_breakpoint_source_count(void);
const char *dap_breakpoint_source_path(size_t source);
const char *dap_breakpoint_source_display_path(size_t source);
size_t dap_breakpoint_source_length(size_t source);
bool dap_breakpoint_get(
    size_t source, size_t index, struct dap_breakpoint_info *out);
bool dap_breakpoint_find(
    const char *path, int line, struct dap_breakpoint_info *out);
/* `D` in the breakpoint pane.  False when there is no breakpoint there;
 * true (and a re-send of that source) otherwise, idempotently. */
bool dap_breakpoint_set_enabled(const char *path, int line, bool enabled);

/* ------------------------- the session's side ------------------------- */

/* The configuration join's source provider, with struct
 * dap_session_hooks::sources' signature exactly (`ctx` is unused: there is
 * one table).  Asked once per session, when configuration starts; it takes
 * each source's snapshot and marks it in flight, so the join's answers are
 * this module's answers and no second `setBreakpoints` is sent behind the
 * join's back.
 *
 * The join sends lines only.  A source carrying conditions, hit conditions
 * or log messages is therefore left DIRTY by this call, and the full set
 * with those members goes out through the ordinary machinery the moment the
 * join's answer lands -- one extra round trip for the rare source, and both
 * adapters answer requests in the order they arrive, so it is queued ahead
 * of `configurationDone` either way. */
size_t dap_breakpoint_session_sources(
    void *ctx, struct dap_session_source *out, size_t max);

/* One source's `setBreakpoints` answer, with struct
 * dap_session_hooks::breakpoints_answered's signature.  `body` is the
 * response body, borrowed for the call. */
void dap_breakpoint_session_answered(void *ctx, const char *path, bool success,
    const struct kg_json_value *body);

/* A `breakpoint` event's body.  Unknown ids with a reason other than
 * "removed" create a client-side breakpoint, but only when the event
 * carries a usable source path and a positive line -- a `sourceReference`
 * without a path is listed non-navigable in v1 and is not somewhere kg can
 * put a marker.
 *
 * A breakpoint the adapter MOVED re-anchors at the new line, and merges
 * with any breakpoint already there: the one already at the target line
 * survives and keeps its own condition, hit condition and log message, and
 * takes the moved one's adapter id, verdict and `temporary` flag.
 * src/dap_breakpoint.c's bp_merge_into() states the whole rule. */
void dap_breakpoint_event(const struct kg_json_value *body);

/* Push every source that has drifted from what the adapter was told.  A
 * no-op without a session that has started configuring: before that the
 * join's snapshot is what carries the table, and sending behind it would be
 * the second in-flight request per source this module exists to prevent. */
void dap_breakpoint_sync(void);

/* Whether an EDIT has moved a breakpoint's anchor since the adapter was
 * last told, and the debounce that makes a burst of typing one request has
 * expired.  dap_poll() asks this every tick and calls dap_breakpoint_sync()
 * when it says yes, which is the whole of "the adapter hears about edits":
 * without it an adapter keeps stopping at the line a breakpoint USED to be
 * on for the rest of the session.  Idle it is one comparison, and
 * dap_breakpoint_sync() is what disarms it. */
bool dap_breakpoint_resync_due(void);

/* The session is over (dead, cancelled, or about to be).  Every temporary
 * breakpoint is removed -- they belong to one run -- every id and
 * verification is forgotten, since they were that adapter's, and nothing is
 * sent: there is nobody to tell. */
void dap_breakpoint_session_ended(void);

/* --------------------------- temporary ones --------------------------- */

/* Told once, when the temporary breakpoint's own source has answered the
 * newest `setBreakpoints` that carried it: `verified` is the adapter's
 * verdict, and `text` its reason when it refused.  A false verdict has
 * already removed the breakpoint and resynced by the time this runs, so the
 * caller's only job is to NOT continue and to say why.
 *
 * This is the seam stage 6 owns: `dap-until` and `dap-breakpoint-temporary`
 * arm one here and send `continue` from this callback, which is what makes
 * "continue only if it verified" a sequence rather than a race. */
typedef void (*dap_breakpoint_armed_fn)(
    void *ctx, bool verified, const char *text);

enum dap_breakpoint_result dap_breakpoint_arm_temporary(
    const char *path, int line, dap_breakpoint_armed_fn cb, void *ctx);

/* A `stopped` event's body, for the temporary breakpoints only.  Returns
 * true when one of them was hit and removed, which is what tells stage 6 the
 * stop it is about to paint was kg's own doing.
 *
 * `hitBreakpointIds` is the reliable half and is used when it is there: an
 * id that matches a temporary removes it, and an id that matches something
 * else -- or an exception stop that happens to intervene -- removes
 * nothing.  Adapters that omit hit ids (measured on debugpy and nbcode)
 * leave the question open until the stack is known, which is what
 * dap_breakpoint_stopped_at() below answers.  A stop with hit ids CLOSES
 * the question either way, so a later location match cannot remove a
 * temporary the adapter has already told kg was not hit. */
bool dap_breakpoint_stopped(const struct kg_json_value *body);

/* Where the stop actually was, from stage 6's `stackTrace` answer for frame
 * zero.  Removes a temporary breakpoint at that source and line, and only
 * when the `stopped` event that opened this stop carried no hit ids.
 * Returns true when one was removed. */
bool dap_breakpoint_stopped_at(const char *path, int line);

/* How many temporary breakpoints the table holds, for a caller (and a
 * test) that wants to know without walking every source. */
size_t dap_breakpoint_temporary_count(void);

#endif /* KG_DAP_BREAKPOINT_H */
