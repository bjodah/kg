/* ======================= the editor's breakpoint table ==================
 *
 * See src/dap_breakpoint.h for the contract: the sum-type location, the
 * canonical key, the one-request-in-flight-per-source protocol, and why a
 * `breakpoint` event may never send anything.  Stage 5 of
 * doc/plans/dap/01-protocol.md.
 *
 * The table is editor state, so this is the first DAP module that reaches
 * for a buffer at all -- and it reaches for exactly three things: a marker,
 * a row's byte offset, and the two lifecycle events that turn one into the
 * other.  It READS rows.  It never writes one, writes no undo record and
 * touches no row's text fields (`make gateway-check`).
 */

#include "dap_breakpoint.h"

#include "bufhandle.h"
#include "dap_client.h"
#include "dap_session.h"
#include "def.h"
#include "event.h"
#include "json.h"
#include "marker.h"

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct dap_client; /* src/dap_client.h */

/* How long an edit that moved a breakpoint waits before the source goes out
 * again.  The panes' number and its reasoning (DAP_UI_DEBOUNCE_MS): a burst
 * of typing is one `setBreakpoints`, not one per keystroke -- and every
 * successful set re-keys every adapter id, so the burst matters. */
#define DAP_BREAKPOINT_RESYNC_DEBOUNCE_MS 100u

/* A table that could hold more than the configuration join can send would
 * be a table whose extra rows are silently never set. */
static_assert(DAP_BREAKPOINT_MAX_SOURCES == DAP_SESSION_MAX_SOURCES,
    "the table's source bound and the join's must agree");
static_assert(DAP_BREAKPOINT_MAX_PER_SOURCE == DAP_SESSION_MAX_SOURCE_LINES,
    "the table's per-source bound and the join's must agree");

/* One breakpoint.
 *
 * `line` is the anchor's own line, refreshed from the marker whenever the
 * table is read or written; `sent_line` is what the last request carried
 * and `adapter_line` what the adapter answered for it.  That pair is what
 * makes "the returned line is what renders" survive an edit: once the
 * marker moves, `line` no longer equals `sent_line`, and the display goes
 * back to the marker until the next round trip agrees again.
 *
 * `armed_cb` is the temporary-breakpoint seam.  It runs exactly once: on
 * the response to the newest set that carried this breakpoint, or on the
 * session ending, whichever happens first. */
struct dap_bp {
	uint32_t local_id;
	bool anchored;
	struct kg_marker_handle marker;
	int line;
	int sent_line;
	int adapter_line;
	bool verified;
	bool has_id;
	int id;
	bool temporary;
	/* Whether a user put a breakpoint HERE, as opposed to this object
	 * existing only because a run-to-here needed somewhere to stop.  One
	 * line is one object, so `dap-until` over a breakpoint the user set
	 * lands on that object; this is what tells the two apart again when
	 * the temporary half is over -- see temporary_hit(). */
	bool user_set;
	/* Spelled as the NEGATIVE so that zero is an ordinary breakpoint:
	 * every one of the two creation sites below memsets its slot, and a
	 * flag that had to be set to true afterwards is a flag one of them
	 * would eventually forget. */
	bool disabled;
	dap_breakpoint_armed_fn armed_cb;
	void *armed_ctx;
	char condition[DAP_BREAKPOINT_TEXT_MAX];
	char hit_condition[DAP_BREAKPOINT_TEXT_MAX];
	char log_message[DAP_BREAKPOINT_TEXT_MAX];
	char message[DAP_BREAKPOINT_TEXT_MAX];
};

/* One entry of the snapshot a request was built from: which breakpoint, at
 * which line, in which array position.  A response is zipped against THIS,
 * and each entry's breakpoint is looked up by `local_id` -- so an element
 * can never land on a breakpoint the request did not carry, and a
 * breakpoint removed while the request was in flight simply has no entry
 * left to find. */
struct dap_bp_snap {
	uint32_t local_id;
	int line;
};

/* One source: the canonical key, the ordered vector, and the four fields
 * the in-flight protocol is. */
struct dap_bp_source {
	char path[PATH_MAX];
	char display[PATH_MAX];
	/* Unique per source INSTANCE, so a response that outlives its source
	 * -- or a later source at the same array position -- finds nothing
	 * rather than somebody else's vector. */
	uint32_t epoch;
	struct dap_bp items[DAP_BREAKPOINT_MAX_PER_SOURCE];
	size_t count;
	uint32_t desired_generation;
	uint32_t sent_generation;
	bool request_in_flight;
	bool dirty;
	struct dap_bp_snap snap[DAP_BREAKPOINT_MAX_PER_SOURCE];
	size_t snap_count;
};

/* Compacted: `source_count` live sources at the front, so a public index is
 * a position here and a released source leaves no hole.  Requests in flight
 * survive the compaction because their context names an epoch, never a
 * position. */
static struct dap_bp_source *sources[DAP_BREAKPOINT_MAX_SOURCES];
static size_t source_count;
static uint32_t next_local_id = 1;
static uint32_t next_epoch = 1;

/* The context a `setBreakpoints` kg sent itself carries: one slot per
 * possible in-flight request, holding the epoch of the source it belongs to
 * and zero when free.  A pointer into this array is a context that is
 * always safe to dereference, whatever became of the source. */
static uint32_t flights[DAP_BREAKPOINT_MAX_SOURCES];

static struct kg_event_subscriber_token event_token;

/* Whether the `stopped` kg is inside left the "was a temporary breakpoint
 * hit" question open -- which is what an adapter that omits
 * `hitBreakpointIds` does (measured on debugpy and nbcode).  A stop that
 * named hit ids has ANSWERED the question, so a later location match may
 * not reopen it and remove a temporary the adapter said was not hit. */
static bool stop_question_open;

/* An edit has moved an anchor since the adapter was last told, and when the
 * re-send is due.  Armed by the change subscriber, disarmed by
 * dap_breakpoint_sync(); idle it is one bool nobody writes. */
static bool resync_due;
static unsigned long long resync_due_ms;

static void (*report_hook)(void *ctx, bool error, const char *text);
static void *report_hook_ctx;

/* Where "the set changed" goes.  Separate from the report hook because it
 * carries no words: a repaint is not news for the echo area. */
static void (*changed_hook)(void *ctx);
static void *changed_hook_ctx;

/* ------------------------------ small parts --------------------------- */

static unsigned long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ull
	    + (unsigned long long)(ts.tv_nsec / 1000000);
}

const char *dap_breakpoint_result_text(enum dap_breakpoint_result result)
{
	switch (result) {
	case DAP_BREAKPOINT_OK:
		return "";
	case DAP_BREAKPOINT_NO_FILE:
		return "Breakpoints need a buffer visiting a file on disk";
	case DAP_BREAKPOINT_BAD_LINE:
		return "No such line";
	case DAP_BREAKPOINT_FULL:
		return "Too many breakpoints";
	case DAP_BREAKPOINT_NO_MEMORY:
		return "Out of memory";
	}
	return "";
}

void dap_breakpoint_set_report_hook(
    void (*hook)(void *ctx, bool error, const char *text), void *ctx)
{
	report_hook = hook;
	report_hook_ctx = ctx;
}

void dap_breakpoint_set_changed_hook(void (*hook)(void *ctx), void *ctx)
{
	changed_hook = hook;
	changed_hook_ctx = ctx;
}

/* One notification, cheap and safe from inside a response callback: it
 * calls a hook that sets a flag (src/dap_ui.h) and touches no table. */
static void note_changed(void)
{
	if (changed_hook) {
		changed_hook(changed_hook_ctx);
	}
}

/* One line of news.  Adapter text reaches it (a refusal, a relocation
 * message), so it is bounded and goes where every other bounded sentence
 * goes; what makes those bytes safe to PAINT is display_glyph_at() at the
 * other end, as for every byte kg reads. */
static void report(bool error, const char *fmt, ...)
{
	char text[2 * DAP_BREAKPOINT_TEXT_MAX];
	va_list ap;

	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	if (report_hook) {
		report_hook(report_hook_ctx, error, text);
		return;
	}
	editor_set_status_message("%s", text);
}

static void copy_text(char *dst, size_t size, const char *src)
{
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, size, "%s", src);
}

/* The canonical key.  realpath() is what makes two spellings of one file
 * one source [M-11]; a path that does not resolve names a file that does
 * not exist, which is a breakpoint no adapter could place. */
static bool canonical_path(const char *path, char out[PATH_MAX])
{
	char resolved[PATH_MAX];

	if (!path || !path[0] || !realpath(path, resolved)) {
		return false;
	}
	memcpy(out, resolved, strlen(resolved) + 1);
	return true;
}

static bool buffer_file(const struct editor_buffer *b, char out[PATH_MAX])
{
	if (!b || b->no_file || !b->filename) {
		return false;
	}
	return canonical_path(b->filename, out);
}

/* The buffer visiting `path`, or NULL.  One file is one source however the
 * user reached it, so this asks by canonical path and not by the string a
 * buffer happens to hold. */
static struct editor_buffer *buffer_visiting(const char *path)
{
	char resolved[PATH_MAX];
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active) {
			continue;
		}
		if (buffer_file(&buflist[i], resolved)
		    && strcmp(resolved, path) == 0) {
			return &buflist[i];
		}
	}
	return NULL;
}

static bool same_buffer(struct kg_buffer_handle a, struct kg_buffer_handle b)
{
	return a.slot == b.slot && a.id == b.id && a.generation == b.generation;
}

/* ------------------------------- anchoring ---------------------------- */

/* A RIGHT-gravity marker at the first byte of the line: an insertion there
 * -- a newline typed above the statement -- moves the marker past what was
 * inserted, so the breakpoint stays with the code rather than with the
 * blank line the user just made.  The same rule is what makes a whole-line
 * deletion retain the breakpoint at the resulting line: the marker is
 * already at the start of whatever followed. */
static void anchor_bp(struct editor_buffer *b, struct dap_bp *bp)
{
	int row = bp->line - 1;
	size_t pos;

	if (!b || b->numrows <= 0) {
		return;
	}
	if (row >= b->numrows) {
		row = b->numrows - 1;
		bp->line = row + 1;
	}
	pos = buffer_row_col_to_position(b, row, 0);
	bp->marker = kg_marker_create(b, pos, KG_MARKER_GRAV_RIGHT);
	bp->anchored = bp->marker.id != 0;
}

/* The breakpoint's line right now.  An anchored one reads its marker and
 * caches the answer; one whose marker has stopped resolving -- the buffer
 * was killed and the KILLING event has not been delivered yet, since
 * delivery waits for a safe point -- demotes itself here and keeps the last
 * line it saw. */
static int bp_refresh(struct dap_bp *bp)
{
	int row, col;

	if (!bp->anchored) {
		return bp->line;
	}
	if (!kg_marker_get_row_col(bp->marker, &row, &col)) {
		bp->anchored = false;
		return bp->line;
	}
	bp->line = row + 1;
	return bp->line;
}

/* What renders [M-9]: the adapter's answer for the line kg last sent, and
 * the anchor's own line again as soon as an edit moves it off that line. */
static int bp_display_line(struct dap_bp *bp)
{
	int line = bp_refresh(bp);

	return (bp->adapter_line > 0 && bp->sent_line == line)
	    ? bp->adapter_line
	    : line;
}

/* ------------------------------- the table ---------------------------- */

static struct dap_bp_source *source_find(const char *path, size_t *index)
{
	size_t i;

	for (i = 0; i < source_count; i++) {
		if (strcmp(sources[i]->path, path) == 0) {
			if (index) {
				*index = i;
			}
			return sources[i];
		}
	}
	return NULL;
}

static struct dap_bp_source *source_by_epoch(uint32_t epoch, size_t *index)
{
	size_t i;

	for (i = 0; epoch && i < source_count; i++) {
		if (sources[i]->epoch == epoch) {
			if (index) {
				*index = i;
			}
			return sources[i];
		}
	}
	return NULL;
}

static struct dap_bp_source *source_create(
    const char *path, const char *display, size_t *index)
{
	struct dap_bp_source *s;

	if (source_count == DAP_BREAKPOINT_MAX_SOURCES) {
		return NULL;
	}
	s = calloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}
	copy_text(s->path, sizeof(s->path), path);
	copy_text(s->display, sizeof(s->display), display ? display : path);
	s->epoch = next_epoch++;
	sources[source_count] = s;
	if (index) {
		*index = source_count;
	}
	source_count++;
	return s;
}

static struct dap_bp *bp_by_local_id(struct dap_bp_source *s, uint32_t local_id)
{
	size_t i;

	for (i = 0; i < s->count; i++) {
		if (s->items[i].local_id == local_id) {
			return &s->items[i];
		}
	}
	return NULL;
}

/* The breakpoint on `line`, by what it RENDERS as -- an adapter that
 * relocated one put it where the user now sees it, and that is the line a
 * toggle names -- and by its anchor as well, so a breakpoint whose adapter
 * line has drifted is still found where it actually is. */
static bool bp_find_line(struct dap_bp_source *s, int line, size_t *index)
{
	size_t i;

	for (i = 0; i < s->count; i++) {
		if (bp_display_line(&s->items[i]) == line
		    || s->items[i].line == line) {
			*index = i;
			return true;
		}
	}
	return false;
}

/* Drop the breakpoint at `i`, marker and all.  Nothing is sent from here:
 * the caller decides whether this was a mutation the adapter must hear
 * about or a projection of something it just told kg. */
static void bp_forget(struct dap_bp_source *s, size_t i)
{
	if (s->items[i].anchored) {
		kg_marker_delete(s->items[i].marker);
	}
	memmove(&s->items[i], &s->items[i + 1],
	    (s->count - i - 1) * sizeof(s->items[0]));
	s->count--;
	memset(&s->items[s->count], 0, sizeof(s->items[0]));
}

/* A source with nothing left in it and nothing outstanding is gone: the
 * empty array has already been sent -- that is what the removal did -- and
 * a remembered empty source would send one again at every session start. */
static void source_release(size_t index)
{
	struct dap_bp_source *s = sources[index];

	if (!s || s->count > 0 || s->request_in_flight) {
		return;
	}
	free(s);
	memmove(&sources[index], &sources[index + 1],
	    (source_count - index - 1) * sizeof(sources[0]));
	source_count--;
	sources[source_count] = NULL;
	/* Every index at or after `index` now names a different source.  A
	 * projection built before this call has to hear about it, whether the
	 * release came from a user's own removal or from an ordinary
	 * `setBreakpoints` response nothing else reports. */
	note_changed();
}

/* ---------------------------- talking to an adapter ------------------- */

/* The client to send on, or NULL for every honest reason there might not be
 * one: no session, a dead one, or one that has not started configuring --
 * before that the join's own snapshot is what carries the table, and
 * sending behind it would be the second in-flight request per source this
 * module exists to prevent. */
static struct dap_client *sendable_client(void)
{
	struct dap_session *session = dap_session_current();
	struct dap_session_state st;
	struct dap_client *c;

	if (!session) {
		return NULL;
	}
	c = dap_session_client(session);
	if (!c) {
		return NULL;
	}
	dap_session_get_state(session, &st);
	return st.configuration_started ? c : NULL;
}

/* Whether an answer can still be expected at all -- which is a different
 * question from whether kg may send right now: a session that has not
 * started configuring will carry the table in its join. */
static bool adapter_reachable(void)
{
	struct dap_session *session = dap_session_current();

	return session && dap_session_client(session) != NULL;
}

/* The wire fields, each sent only if the adapter said it can do that one:
 * an adapter without `supportsConditionalBreakpoints` is not asked, rather
 * than being handed a member it will either ignore or refuse the whole
 * source over. */
static void write_bp_object(
    struct kg_jsonw *w, struct dap_client *c, struct dap_bp *bp, int line)
{
	kg_jsonw_begin_object(w);
	kg_jsonw_key(w, "line");
	kg_jsonw_int(w, line);
	if (bp->condition[0]
	    && dap_capable(c, DAP_CAP_CONDITIONAL_BREAKPOINTS)) {
		kg_jsonw_key(w, "condition");
		kg_jsonw_string(w, bp->condition);
	}
	if (bp->hit_condition[0]
	    && dap_capable(c, DAP_CAP_HIT_CONDITIONAL_BREAKPOINTS)) {
		kg_jsonw_key(w, "hitCondition");
		kg_jsonw_string(w, bp->hit_condition);
	}
	if (bp->log_message[0] && dap_capable(c, DAP_CAP_LOG_POINTS)) {
		kg_jsonw_key(w, "logMessage");
		kg_jsonw_string(w, bp->log_message);
	}
	kg_jsonw_end_object(w);
}

/* Whether this breakpoint carries anything the configuration join's own
 * request cannot express (it sends lines and nothing else). */
static bool bp_has_wire_fields(const struct dap_bp *bp)
{
	return bp->condition[0] || bp->hit_condition[0] || bp->log_message[0];
}

/* Take the snapshot.  The previous answer's line is dropped here rather
 * than kept: it was about the previous request, and a re-key is on its way
 * for this one.  The previous VERDICT stays until the answer replaces it,
 * so a source being re-sent does not blink through unverified. */
static void snap_take(struct dap_bp_source *s, size_t i, int line)
{
	s->snap[s->snap_count].local_id = s->items[i].local_id;
	s->snap[s->snap_count].line = line;
	s->snap_count++;
	s->items[i].sent_line = line;
	s->items[i].adapter_line = 0;
}

static int build_request(
    struct dap_bp_source *s, struct dap_client *c, char **out, size_t *len)
{
	struct kg_jsonw w;
	size_t i;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "source");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "path");
	kg_jsonw_string(&w, s->path);
	kg_jsonw_end_object(&w);
	kg_jsonw_key(&w, "breakpoints");
	kg_jsonw_begin_array(&w);
	s->snap_count = 0;
	for (i = 0; i < s->count; i++) {
		int line = bp_refresh(&s->items[i]);

		/* A disabled breakpoint is one kg keeps and the adapter is
		 * never told about: `setBreakpoints` replaces the whole set
		 * per source [M-9], so leaving it out of the array IS the
		 * disable.  The snapshot skips it in step, which is what
		 * keeps the response's positional zip aligned. */
		if (s->items[i].disabled) {
			continue;
		}
		snap_take(s, i, line);
		write_bp_object(&w, c, &s->items[i], line);
	}
	kg_jsonw_end_array(&w);
	kg_jsonw_key(&w, "sourceModified");
	kg_jsonw_bool(&w, false);
	kg_jsonw_end_object(&w);
	return kg_jsonw_finish(&w, out, len);
}

static uint32_t *flight_claim(uint32_t epoch)
{
	size_t i;

	for (i = 0; i < DAP_BREAKPOINT_MAX_SOURCES; i++) {
		if (flights[i] == 0) {
			flights[i] = epoch;
			return &flights[i];
		}
	}
	return NULL;
}

static void on_set_breakpoints(
    struct dap_client *c, const struct dap_response *r, void *ctx);

/* Send this source's whole set, if there is anybody to send it to and
 * nothing already in flight for it.  Everything else the caller wanted --
 * the generation bump, the projection, the removal -- has already happened;
 * this is only the wire. */
static void source_sync(size_t index)
{
	struct dap_bp_source *s = sources[index];
	struct dap_client *c = sendable_client();
	uint32_t *flight;
	char *body = NULL;
	size_t len = 0;

	if (s->request_in_flight && !adapter_reachable()) {
		/* Whatever was in flight died with the session, and a request
		 * nobody will ever answer does not get to hold a source
		 * hostage.  dap_breakpoint_session_ended() is the orderly way
		 * here; this is the backstop for every other way. */
		s->request_in_flight = false;
	}
	if (s->request_in_flight) {
		s->dirty = true;
		return;
	}
	if (!c) {
		s->dirty = false;
		source_release(index);
		return;
	}
	flight = flight_claim(s->epoch);
	if (!flight) {
		return;
	}
	if (build_request(s, c, &body, &len) != 0) {
		*flight = 0;
		return;
	}
	/* Set before the send: a request that cannot even be handed over
	 * fails its callback synchronously (src/dap_client.h), and that
	 * callback is the one that clears these again. */
	s->request_in_flight = true;
	s->sent_generation = s->desired_generation;
	s->dirty = false;
	(void)dap_client_request(
	    c, "setBreakpoints", body, len, on_set_breakpoints, flight);
	free(body);
}

/* One mutation of the live vector: the desired generation moves, and the
 * source is either sent now or marked dirty for whenever the flight ends. */
static void source_mutated(size_t index)
{
	sources[index]->desired_generation++;
	note_changed();
	source_sync(index);
}

/* A change kg LEARNED from the adapter rather than made: the projection
 * moves and nothing is sent.  An adapter that fires `changed` after every
 * set response and a client that re-sent on every change would keep each
 * other re-keying ids forever, which is the loop the plan names.  The
 * generation still moves, so the next real mutation carries this along. */
static void source_projected(size_t index)
{
	sources[index]->desired_generation++;
	note_changed();
}

/* ----------------------------- the response --------------------------- */

static void note_unverified(struct dap_bp *bp, const char *why)
{
	bp->verified = false;
	bp->has_id = false;
	bp->adapter_line = 0;
	copy_text(bp->message, sizeof(bp->message), why);
}

/* One `Breakpoint` from the response array, onto the snapshot entry's own
 * breakpoint.  Ids are re-keyed on EVERY successful set -- both measured
 * adapters reissue them, debugpy even for an unchanged line -- and are
 * stored as has_id + id, because debugpy's first id is 0 [M-9]. */
static void apply_element(struct dap_bp *bp, const struct kg_json_value *elem)
{
	const struct kg_json_value *id = kg_json_get(elem, "id");
	long long line = kg_json_int(kg_json_get(elem, "line"), 0);
	long long value = kg_json_int(id, 0);

	bp->has_id = kg_json_kind_of(id) == KG_JSON_NUMBER && value >= INT32_MIN
	    && value <= INT32_MAX;
	bp->id = bp->has_id ? (int)value : 0;
	bp->verified = kg_json_bool(kg_json_get(elem, "verified"), false);
	/* The returned line is what renders: debugpy silently relocates an
	 * out-of-range line and calls it verified, and `line` may be absent
	 * altogether on an unverified one. */
	bp->adapter_line = (line > 0 && line <= INT32_MAX) ? (int)line : 0;
	copy_text(bp->message, sizeof(bp->message),
	    kg_json_str(kg_json_get(elem, "message"), NULL));
}

/* Zip the answer against the snapshot it belongs to, never against the live
 * vector (dape.el:1760-1762's hazard).  Returns how many entries the
 * adapter left unaccounted for, which the caller reports. */
static size_t apply_response(
    struct dap_bp_source *s, bool success, const struct kg_json_value *body)
{
	const struct kg_json_value *list = kg_json_get(body, "breakpoints");
	size_t missing = 0;
	size_t i;

	for (i = 0; i < s->snap_count; i++) {
		struct dap_bp *bp = bp_by_local_id(s, s->snap[i].local_id);
		const struct kg_json_value *elem
		    = success ? kg_json_at(list, i) : NULL;

		if (!bp) {
			continue; /* removed while this was in flight */
		}
		if (kg_json_kind_of(elem) != KG_JSON_OBJECT) {
			note_unverified(bp,
			    success ? "the adapter answered nothing for it"
				    : "the adapter refused this source");
			missing++;
			continue;
		}
		apply_element(bp, elem);
	}
	return missing;
}

/* One armed temporary breakpoint, settled.  An unverified one is removed
 * before its callback runs, so the caller never sees a table still
 * promising a stop that will not come; the removal is a mutation like any
 * other and resyncs the source.  Returns false once none is left. */
static bool settle_one_armed(uint32_t epoch)
{
	char text[DAP_BREAKPOINT_TEXT_MAX];
	dap_breakpoint_armed_fn cb;
	struct dap_bp_source *s;
	size_t index = 0;
	void *ctx;
	bool verified;
	size_t i;

	s = source_by_epoch(epoch, &index);
	for (i = 0; s && i < s->count; i++) {
		if (!s->items[i].armed_cb) {
			continue;
		}
		cb = s->items[i].armed_cb;
		ctx = s->items[i].armed_ctx;
		verified = s->items[i].verified;
		copy_text(text, sizeof(text), s->items[i].message);
		s->items[i].armed_cb = NULL;
		if (!verified) {
			bp_forget(s, i);
			source_mutated(index);
		}
		cb(ctx, verified,
		    verified ? "" : (text[0] ? text : "not verified"));
		return true;
	}
	return false;
}

/* The same, anywhere in the table and for a reason that is not a response:
 * the session ended, so nobody is going to answer. */
static bool settle_any_armed(const char *why)
{
	size_t i, j;

	for (i = 0; i < source_count; i++) {
		for (j = 0; j < sources[i]->count; j++) {
			struct dap_bp *bp = &sources[i]->items[j];
			dap_breakpoint_armed_fn cb = bp->armed_cb;

			if (!cb) {
				continue;
			}
			bp->armed_cb = NULL;
			cb(bp->armed_ctx, false, why);
			return true;
		}
	}
	return false;
}

static void settle_armed(uint32_t epoch)
{
	size_t guard;

	for (guard = 0; guard < DAP_BREAKPOINT_MAX_PER_SOURCE; guard++) {
		if (!settle_one_armed(epoch)) {
			return;
		}
	}
}

/* The end of one round for one source, whichever request it was: kg's own
 * `setBreakpoints` or the session's configuration join.  A dirty source
 * re-sends the NEWEST full snapshot immediately -- that is what makes an
 * answer that is already stale cost one more round trip instead of a wrong
 * table. */
static void finish_round(uint32_t epoch, bool success, const char *error_text,
    const struct kg_json_value *body)
{
	struct dap_bp_source *s;
	size_t index = 0;
	size_t missing;

	s = source_by_epoch(epoch, &index);
	if (!s || !s->request_in_flight) {
		return;
	}
	s->request_in_flight = false;
	missing = apply_response(s, success, body);
	/* Verdicts, ids and relocated lines all moved: the pane says
	 * "pending" or "verified" per breakpoint and the line it renders is
	 * the adapter's answer, so this is a repaint even when nothing else
	 * below happens. */
	note_changed();
	if (!success && error_text) {
		report(true, "breakpoints in %s were refused: %s", s->display,
		    error_text);
	} else if (success && missing > 0) {
		report(true, "%s: the adapter answered nothing for %zu of them",
		    s->display, missing);
	}
	settle_armed(epoch);
	s = source_by_epoch(epoch, &index);
	if (!s) {
		return;
	}
	if (s->dirty || s->desired_generation != s->sent_generation) {
		source_sync(index);
		return;
	}
	source_release(index);
}

static void on_set_breakpoints(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	uint32_t *flight = ctx;
	uint32_t epoch = *flight;

	(void)c;
	*flight = 0;
	finish_round(epoch, r->success,
	    r->error ? r->error->message : "no answer", r->body);
}

/* ------------------------------- mutations ---------------------------- */

/* Ordered by line at insertion: the vector a source hands the wire reads in
 * the order a user reads the file.  Edits may later move an anchor past its
 * neighbour, which costs nothing -- the protocol does not order the array
 * and the response is zipped positionally against what was sent. */
static size_t bp_insert_position(struct dap_bp_source *s, int line)
{
	size_t i;

	for (i = 0; i < s->count; i++) {
		if (s->items[i].line > line) {
			break;
		}
	}
	return i;
}

static void bp_apply_options(
    struct dap_bp *bp, const struct dap_breakpoint_options *opts)
{
	if (!opts) {
		return;
	}
	/* A UNION, exactly as bp_merge_into()'s is and for the same reason:
	 * one line is one object, so a run-to-here over a breakpoint the user
	 * already set ADDS the temporary half to it rather than converting
	 * it.  What ends the temporary half without ending the breakpoint is
	 * `user_set` (temporary_hit()). */
	bp->temporary = bp->temporary || opts->temporary;
	/* Only the fields this add actually carries.  A temporary add's
	 * options are all NULL, and an unconditional copy of them is how a
	 * `dap-until` used to erase the condition on the breakpoint it
	 * landed on; a caller clearing one passes "" and still clears it. */
	if (opts->condition) {
		copy_text(
		    bp->condition, sizeof(bp->condition), opts->condition);
	}
	if (opts->hit_condition) {
		copy_text(bp->hit_condition, sizeof(bp->hit_condition),
		    opts->hit_condition);
	}
	if (opts->log_message) {
		copy_text(bp->log_message, sizeof(bp->log_message),
		    opts->log_message);
	}
}

/* Create or update one breakpoint, anchoring it if a buffer is visiting the
 * file, and send the source's new set.  `armed` is attached BEFORE the send
 * so the very request that carries a temporary breakpoint is the one whose
 * answer settles it. */
static enum dap_breakpoint_result bp_put(struct dap_bp_source *s, size_t index,
    int line, const struct dap_breakpoint_options *opts,
    dap_breakpoint_armed_fn armed, void *armed_ctx)
{
	struct editor_buffer *b = buffer_visiting(s->path);
	size_t at;

	if (bp_find_line(s, line, &at)) {
		bp_apply_options(&s->items[at], opts);
	} else {
		if (s->count == DAP_BREAKPOINT_MAX_PER_SOURCE) {
			return DAP_BREAKPOINT_FULL;
		}
		at = bp_insert_position(s, line);
		memmove(&s->items[at + 1], &s->items[at],
		    (s->count - at) * sizeof(s->items[0]));
		memset(&s->items[at], 0, sizeof(s->items[0]));
		s->count++;
		s->items[at].local_id = next_local_id++;
		s->items[at].line = line;
		bp_apply_options(&s->items[at], opts);
		anchor_bp(b, &s->items[at]);
	}
	/* Any add that is not a run-to-here is a user putting a breakpoint
	 * here, whether the object was made for it or was already there. */
	if (!opts || !opts->temporary) {
		s->items[at].user_set = true;
	}
	s->items[at].armed_cb = armed;
	s->items[at].armed_ctx = armed_ctx;
	source_mutated(index);
	return DAP_BREAKPOINT_OK;
}

static enum dap_breakpoint_result add_at_path(const char *canonical,
    const char *display, int line, const struct dap_breakpoint_options *opts,
    dap_breakpoint_armed_fn armed, void *armed_ctx)
{
	struct dap_bp_source *s;
	size_t index = 0;

	if (line <= 0) {
		return DAP_BREAKPOINT_BAD_LINE;
	}
	s = source_find(canonical, &index);
	if (!s) {
		s = source_create(canonical, display, &index);
	}
	if (!s) {
		return source_count == DAP_BREAKPOINT_MAX_SOURCES
		    ? DAP_BREAKPOINT_FULL
		    : DAP_BREAKPOINT_NO_MEMORY;
	}
	return bp_put(s, index, line, opts, armed, armed_ctx);
}

enum dap_breakpoint_result dap_breakpoint_add(
    struct editor_buffer *b, int row, const struct dap_breakpoint_options *opts)
{
	char path[PATH_MAX];

	if (!buffer_file(b, path)) {
		return DAP_BREAKPOINT_NO_FILE;
	}
	if (row < 0 || (b->numrows > 0 && row >= b->numrows)) {
		return DAP_BREAKPOINT_BAD_LINE;
	}
	return add_at_path(path, b->filename, row + 1, opts, NULL, NULL);
}

enum dap_breakpoint_result dap_breakpoint_add_at(
    const char *path, int line, const struct dap_breakpoint_options *opts)
{
	char canonical[PATH_MAX];

	if (!canonical_path(path, canonical)) {
		return DAP_BREAKPOINT_NO_FILE;
	}
	return add_at_path(canonical, path, line, opts, NULL, NULL);
}

static bool remove_canonical(const char *canonical, int line)
{
	struct dap_bp_source *s;
	size_t index = 0;
	size_t at;

	s = source_find(canonical, &index);
	if (!s || !bp_find_line(s, line, &at)) {
		return false;
	}
	/* The source's path was resolved when the breakpoint was made and is
	 * this module's own copy, so the empty array the removal sends still
	 * names the file after the last marker has died. */
	bp_forget(s, at);
	source_mutated(index);
	return true;
}

bool dap_breakpoint_remove(struct editor_buffer *b, int row)
{
	char path[PATH_MAX];

	return buffer_file(b, path) && remove_canonical(path, row + 1);
}

bool dap_breakpoint_remove_at(const char *path, int line)
{
	char canonical[PATH_MAX];

	return canonical_path(path, canonical)
	    && remove_canonical(canonical, line);
}

enum dap_breakpoint_result dap_breakpoint_toggle(
    struct editor_buffer *b, int row, bool *added)
{
	enum dap_breakpoint_result result;

	if (dap_breakpoint_remove(b, row)) {
		if (added) {
			*added = false;
		}
		return DAP_BREAKPOINT_OK;
	}
	result = dap_breakpoint_add(b, row, NULL);
	if (result == DAP_BREAKPOINT_OK && added) {
		*added = true;
	}
	return result;
}

/* -------------------------------- queries ----------------------------- */

size_t dap_breakpoint_source_count(void) { return source_count; }

size_t dap_breakpoint_count(void)
{
	size_t total = 0;
	size_t i;

	for (i = 0; i < source_count; i++) {
		total += sources[i]->count;
	}
	return total;
}

uint32_t dap_breakpoint_source_epoch(size_t source)
{
	return source < source_count ? sources[source]->epoch : 0;
}

bool dap_breakpoint_source_by_epoch(uint32_t epoch, size_t *index)
{
	size_t at = 0;

	if (!source_by_epoch(epoch, &at)) {
		return false;
	}
	if (index) {
		*index = at;
	}
	return true;
}

const char *dap_breakpoint_source_path(size_t source)
{
	return source < source_count ? sources[source]->path : NULL;
}

const char *dap_breakpoint_source_display_path(size_t source)
{
	return source < source_count ? sources[source]->display : NULL;
}

size_t dap_breakpoint_source_length(size_t source)
{
	return source < source_count ? sources[source]->count : 0;
}

static void bp_describe(struct dap_bp *bp, struct dap_breakpoint_info *out)
{
	memset(out, 0, sizeof(*out));
	out->line = bp_display_line(bp);
	out->requested_line = bp->line;
	out->verified = bp->verified;
	out->has_id = bp->has_id;
	out->id = bp->id;
	out->temporary = bp->temporary;
	out->enabled = !bp->disabled;
	out->anchored = bp->anchored;
	copy_text(out->condition, sizeof(out->condition), bp->condition);
	copy_text(
	    out->hit_condition, sizeof(out->hit_condition), bp->hit_condition);
	copy_text(out->log_message, sizeof(out->log_message), bp->log_message);
	copy_text(out->message, sizeof(out->message), bp->message);
}

bool dap_breakpoint_get(
    size_t source, size_t index, struct dap_breakpoint_info *out)
{
	if (source >= source_count || index >= sources[source]->count) {
		return false;
	}
	bp_describe(&sources[source]->items[index], out);
	return true;
}

/* Enable or disable one, which is a re-send of its whole source and nothing
 * else: the table keeps the breakpoint either way, and what changes is
 * whether the adapter is told about it. */
bool dap_breakpoint_set_enabled(const char *path, int line, bool enabled)
{
	char canonical[PATH_MAX];
	struct dap_bp_source *s;
	size_t index = 0;
	size_t at;

	if (!canonical_path(path, canonical)) {
		return false;
	}
	s = source_find(canonical, &index);
	if (!s || !bp_find_line(s, line, &at)) {
		return false;
	}
	if (s->items[at].disabled == !enabled) {
		return true;
	}
	s->items[at].disabled = !enabled;
	source_mutated(index);
	return true;
}

bool dap_breakpoint_find(
    const char *path, int line, struct dap_breakpoint_info *out)
{
	char canonical[PATH_MAX];
	struct dap_bp_source *s;
	size_t at;

	if (!canonical_path(path, canonical)) {
		return false;
	}
	s = source_find(canonical, NULL);
	if (!s || !bp_find_line(s, line, &at)) {
		return false;
	}
	if (out) {
		bp_describe(&s->items[at], out);
	}
	return true;
}

size_t dap_breakpoint_temporary_count(void)
{
	size_t total = 0;
	size_t i, j;

	for (i = 0; i < source_count; i++) {
		for (j = 0; j < sources[i]->count; j++) {
			total += sources[i]->items[j].temporary ? 1 : 0;
		}
	}
	return total;
}

/* --------------------------- the session's side ----------------------- */

size_t dap_breakpoint_session_sources(
    void *ctx, struct dap_session_source *out, size_t max)
{
	size_t written = 0;
	size_t i, j;

	(void)ctx;
	for (i = 0; i < source_count && written < max; i++) {
		struct dap_bp_source *s = sources[i];

		/* Nothing of THIS session can be in flight -- kg sends
		 * nothing before configuration starts -- so a flag left here
		 * belongs to a session that is already gone. */
		s->request_in_flight = false;
		if (s->count == 0) {
			continue;
		}
		copy_text(
		    out[written].path, sizeof(out[written].path), s->path);
		out[written].line_count = 0;
		s->snap_count = 0;
		s->dirty = false;
		for (j = 0; j < s->count; j++) {
			int line = bp_refresh(&s->items[j]);

			if (s->items[j].disabled) {
				continue;
			}
			out[written].lines[out[written].line_count++] = line;
			snap_take(s, j, line);
			/* The join sends lines and nothing else, so a source
			 * carrying wire fields leaves this call dirty and the
			 * full set follows the join's answer. */
			s->dirty |= bp_has_wire_fields(&s->items[j]);
		}
		s->request_in_flight = true;
		s->sent_generation = s->desired_generation;
		written++;
	}
	return written;
}

void dap_breakpoint_session_answered(
    void *ctx, const char *path, bool success, const struct kg_json_value *body)
{
	struct dap_bp_source *s;

	(void)ctx;
	/* No error text: the session's join has already reported a refused
	 * source by name, and one failure is one sentence. */
	s = path ? source_find(path, NULL) : NULL;
	if (s) {
		finish_round(s->epoch, success, NULL, body);
	}
}

bool dap_breakpoint_resync_due(void)
{
	return resync_due && now_ms() >= resync_due_ms;
}

void dap_breakpoint_sync(void)
{
	size_t i = source_count;

	/* Whatever an edit was waiting to push, this is the push. */
	resync_due = false;
	/* Downwards, because a source that turns out to have nothing left to
	 * say is released here and everything after it shifts down. */
	while (i-- > 0) {
		if (sources[i]->dirty
		    || sources[i]->desired_generation
			!= sources[i]->sent_generation) {
			source_sync(i);
		}
	}
}

void dap_breakpoint_session_ended(void)
{
	size_t guard;
	size_t i, j;

	/* Every armed callback runs exactly once, whatever happens, and the
	 * session ending is one of the things that can happen.  One at a
	 * time, re-scanning between them, because a callback is stage 6's
	 * code and may touch the table. */
	for (guard = 0; guard < DAP_BREAKPOINT_MAX_SOURCES; guard++) {
		if (!settle_any_armed("the debugging session ended")) {
			break;
		}
	}
	i = source_count;
	while (i-- > 0) {
		struct dap_bp_source *s = sources[i];

		j = s->count;
		while (j-- > 0) {
			if (!s->items[j].temporary) {
				continue;
			}
			/* The run-to-here belonged to the run; a breakpoint
			 * the user set does not, and outlives it here as it
			 * does at the stop that reaches it. */
			if (s->items[j].user_set) {
				s->items[j].temporary = false;
				s->items[j].armed_ctx = NULL;
				continue;
			}
			bp_forget(s, j);
		}
		/* The ids and the verdicts were that adapter's, and there is
		 * nobody left to tell about the removals. */
		for (j = 0; j < s->count; j++) {
			s->items[j].has_id = false;
			s->items[j].verified = false;
			s->items[j].sent_line = 0;
			s->items[j].adapter_line = 0;
			s->items[j].message[0] = '\0';
		}
		s->request_in_flight = false;
		s->dirty = false;
		s->snap_count = 0;
		s->sent_generation = s->desired_generation;
		source_release(i);
	}
	stop_question_open = false;
	/* Temporaries are gone and every verdict was that adapter's. */
	note_changed();
}

/* ------------------------------- events ------------------------------- */

static struct dap_bp *bp_by_id(int id, size_t *index)
{
	size_t i, j;

	for (i = 0; i < source_count; i++) {
		for (j = 0; j < sources[i]->count; j++) {
			if (sources[i]->items[j].has_id
			    && sources[i]->items[j].id == id) {
				*index = i;
				return &sources[i]->items[j];
			}
		}
	}
	return NULL;
}

/* A breakpoint the adapter moved.  It re-anchors at the new line, and if
 * the user already had one there the two become one, deterministically:
 *
 *   - the breakpoint ALREADY at the target line survives, since that is
 *     the one the user put there and the one they can still see;
 *   - it takes the moved one's adapter id and verdict, because the adapter
 *     has just said that id is at that line and one line is one wire
 *     breakpoint;
 *   - it KEEPS its own condition, hit condition and log message -- two
 *     conditions cannot both be true of one wire breakpoint, and the
 *     destination's is the one the user last saw there;
 *   - `temporary` is the union, so a run-to-here still ends at that line,
 *     and an armed callback moves across rather than being dropped.
 *
 * Nothing is sent: this is a projection of what the adapter just said. */
static void bp_merge_into(struct dap_bp *survivor, struct dap_bp *moved)
{
	survivor->has_id = moved->has_id;
	survivor->id = moved->id;
	survivor->verified = moved->verified;
	copy_text(survivor->message, sizeof(survivor->message), moved->message);
	survivor->temporary = survivor->temporary || moved->temporary;
	/* And so is `user_set`, or a user's breakpoint moved onto a
	 * run-to-here's object would be swept away with it. */
	survivor->user_set = survivor->user_set || moved->user_set;
	if (!survivor->armed_cb && moved->armed_cb) {
		survivor->armed_cb = moved->armed_cb;
		survivor->armed_ctx = moved->armed_ctx;
	}
}

static void bp_move(size_t index, size_t at, int line)
{
	struct dap_bp_source *s = sources[index];
	struct dap_bp *bp = &s->items[at];
	size_t existing;

	if (bp_find_line(s, line, &existing) && existing != at) {
		bp_merge_into(&s->items[existing], bp);
		report(false, "%s: the adapter moved a breakpoint onto line %d",
		    s->display, line);
		bp_forget(s, at);
		source_projected(index);
		return;
	}
	if (bp->anchored) {
		kg_marker_delete(bp->marker);
		bp->anchored = false;
	}
	bp->line = line;
	bp->sent_line = line;
	bp->adapter_line = line;
	anchor_bp(buffer_visiting(s->path), bp);
	source_projected(index);
}

static void event_update(size_t index, size_t at, const struct kg_json_value *b)
{
	struct dap_bp *bp = &sources[index]->items[at];
	long long line = kg_json_int(kg_json_get(b, "line"), 0);

	bp->verified = kg_json_bool(kg_json_get(b, "verified"), bp->verified);
	copy_text(bp->message, sizeof(bp->message),
	    kg_json_str(kg_json_get(b, "message"), NULL));
	if (line > 0 && line <= INT32_MAX && (int)line != bp_display_line(bp)) {
		bp_move(index, at, (int)line);
	}
}

/* An id kg has never seen, with a reason other than "removed" (dape 0.17).
 * It becomes a client-side breakpoint only when the event says WHERE: a
 * `sourceReference` with no path is listed non-navigable in v1 and is not
 * somewhere kg can put a marker. */
static void event_create(const struct kg_json_value *b, int id)
{
	const char *path
	    = kg_json_str(kg_json_get(kg_json_get(b, "source"), "path"), NULL);
	long long line = kg_json_int(kg_json_get(b, "line"), 0);
	char canonical[PATH_MAX];
	struct dap_bp_source *s;
	size_t index = 0;
	size_t at;

	if (line <= 0 || line > INT32_MAX || !canonical_path(path, canonical)) {
		return;
	}
	s = source_find(canonical, &index);
	if (!s) {
		s = source_create(canonical, path, &index);
	}
	if (!s || s->count == DAP_BREAKPOINT_MAX_PER_SOURCE) {
		return;
	}
	at = s->count;
	memset(&s->items[at], 0, sizeof(s->items[at]));
	s->count++;
	s->items[at].local_id = next_local_id++;
	s->items[at].line = (int)line;
	s->items[at].has_id = true;
	s->items[at].id = id;
	s->items[at].verified = kg_json_bool(kg_json_get(b, "verified"), false);
	anchor_bp(buffer_visiting(canonical), &s->items[at]);
	source_projected(index);
}

void dap_breakpoint_event(const struct kg_json_value *body)
{
	const struct kg_json_value *b = kg_json_get(body, "breakpoint");
	const struct kg_json_value *idv = kg_json_get(b, "id");
	const char *reason = kg_json_str(kg_json_get(body, "reason"), NULL);
	long long id = kg_json_int(idv, 0);
	bool removed = reason && strcmp(reason, "removed") == 0;
	size_t index = 0;
	struct dap_bp *bp;

	if (kg_json_kind_of(idv) != KG_JSON_NUMBER || id < INT32_MIN
	    || id > INT32_MAX) {
		return; /* nothing to key the projection on */
	}
	bp = bp_by_id((int)id, &index);
	if (!bp) {
		if (!removed) {
			event_create(b, (int)id);
		}
		return;
	}
	if (removed) {
		bp_forget(sources[index], (size_t)(bp - sources[index]->items));
		source_projected(index);
		/* The adapter is the one that removed it, so there is
		 * nothing left to tell it and nothing left to hold. */
		source_release(index);
		return;
	}
	event_update(index, (size_t)(bp - sources[index]->items), b);
}

/* ---------------------------- temporary ones -------------------------- */

enum dap_breakpoint_result dap_breakpoint_arm_temporary(
    const char *path, int line, dap_breakpoint_armed_fn cb, void *ctx)
{
	struct dap_breakpoint_options opts = { .temporary = true };
	enum dap_breakpoint_result result;
	char canonical[PATH_MAX];
	struct dap_bp_source *s;
	size_t index = 0;
	size_t at;

	if (!canonical_path(path, canonical)) {
		return DAP_BREAKPOINT_NO_FILE;
	}
	result = add_at_path(canonical, path, line, &opts, cb, ctx);
	if (result != DAP_BREAKPOINT_OK) {
		return result;
	}
	/* Nothing was sent and nothing ever will be, so the caller is told
	 * now rather than left waiting for an answer that has no request.
	 * A session that has not started configuring yet is NOT this case:
	 * its join is about to carry the breakpoint, and its answer is what
	 * settles it. */
	s = source_find(canonical, &index);
	if (!adapter_reachable() && s && bp_find_line(s, line, &at)
	    && s->items[at].armed_cb) {
		s->items[at].armed_cb = NULL;
		bp_forget(s, at);
		source_mutated(index);
		cb(ctx, false, "no debugging session");
	}
	return DAP_BREAKPOINT_OK;
}

/* Remove the temporary breakpoint `at` in source `index` and tell the
 * adapter, which is the "resyncing if the session lives" half of the rule.
 * A breakpoint hit before its own set response came back still has an armed
 * callback, and it is settled here rather than left waiting for an answer
 * that is now about a breakpoint the table no longer holds.
 *
 * A breakpoint the user set is not removed by a run-to-here that landed on
 * it: only the temporary half ends, and the source is re-sent so the adapter
 * hears that the breakpoint itself survives. */
static void temporary_hit(size_t index, size_t at, const char *why)
{
	struct dap_bp_source *s = sources[index];
	dap_breakpoint_armed_fn cb = s->items[at].armed_cb;
	void *ctx = s->items[at].armed_ctx;

	s->items[at].armed_cb = NULL;
	s->items[at].armed_ctx = NULL;
	if (s->items[at].user_set) {
		s->items[at].temporary = false;
	} else {
		bp_forget(s, at);
	}
	source_mutated(index);
	if (cb) {
		cb(ctx, false, why);
	}
}

static bool temporary_with_id(int id, size_t *index, size_t *at)
{
	size_t i, j;

	for (i = 0; i < source_count; i++) {
		for (j = 0; j < sources[i]->count; j++) {
			if (sources[i]->items[j].temporary
			    && sources[i]->items[j].has_id
			    && sources[i]->items[j].id == id) {
				*index = i;
				*at = j;
				return true;
			}
		}
	}
	return false;
}

bool dap_breakpoint_stopped(const struct kg_json_value *body)
{
	const struct kg_json_value *ids = kg_json_get(body, "hitBreakpointIds");
	size_t count = kg_json_len(ids);
	size_t index, at;
	size_t i;
	bool hit = false;

	if (kg_json_kind_of(ids) != KG_JSON_ARRAY || count == 0) {
		/* No hit ids: the question stays open until the stack says
		 * where this stop actually was. */
		stop_question_open = true;
		return false;
	}
	stop_question_open = false;
	for (i = 0; i < count; i++) {
		long long id = kg_json_int(kg_json_at(ids, i), -1);

		if (id < INT32_MIN || id > INT32_MAX) {
			continue;
		}
		while (temporary_with_id((int)id, &index, &at)) {
			temporary_hit(index, at, "the stop reached it first");
			hit = true;
		}
	}
	return hit;
}

bool dap_breakpoint_stopped_at(const char *path, int line)
{
	char canonical[PATH_MAX];
	struct dap_bp_source *s;
	size_t index = 0;
	size_t at;

	if (!stop_question_open || !canonical_path(path, canonical)) {
		return false;
	}
	s = source_find(canonical, &index);
	if (!s || !bp_find_line(s, line, &at) || !s->items[at].temporary) {
		return false;
	}
	stop_question_open = false;
	temporary_hit(index, at, "the stop reached it first");
	return true;
}

/* --------------------------- buffer lifecycle ------------------------- */

/* KILLING is PUBLISHED while the buffer still resolves, but DELIVERED at
 * the next safe point -- by which time src/bufmgr.c has already freed the
 * marker store.  So demotion takes the marker's line when the marker is
 * still there, and the line bp_refresh() last cached when it is not; the
 * change subscriber below is what keeps that cache honest, since it runs at
 * the safe point after the edit and before any kill. */
static void demote_buffer(struct kg_buffer_handle buffer)
{
	size_t i, j;

	for (i = 0; i < source_count; i++) {
		for (j = 0; j < sources[i]->count; j++) {
			struct dap_bp *bp = &sources[i]->items[j];

			if (!bp->anchored
			    || !same_buffer(bp->marker.buffer, buffer)) {
				continue;
			}
			(void)bp_refresh(bp);
			kg_marker_delete(bp->marker);
			bp->anchored = false;
			bp->marker = (struct kg_marker_handle) { 0 };
		}
	}
}

/* The anchors' cache, kept honest at the safe point after every edit -- and
 * where an edit becomes news for the adapter.  A marker that MOVED means the
 * line the adapter would stop at is no longer the line the user sees, so the
 * source's desired generation moves (a projection: nothing is sent from
 * here) and the debounced re-send is armed.  Ordinary typing moves no
 * marker; a newline inserted or removed above a breakpoint does. */
static void refresh_buffer(struct kg_buffer_handle buffer)
{
	size_t i, j;

	for (i = 0; i < source_count; i++) {
		bool moved = false;

		for (j = 0; j < sources[i]->count; j++) {
			struct dap_bp *bp = &sources[i]->items[j];
			int was;

			if (!bp->anchored
			    || !same_buffer(bp->marker.buffer, buffer)) {
				continue;
			}
			was = bp->line;
			moved |= bp_refresh(bp) != was;
		}
		if (moved) {
			source_projected(i);
			resync_due = true;
			resync_due_ms
			    = now_ms() + DAP_BREAKPOINT_RESYNC_DEBOUNCE_MS;
		}
	}
}

/* A file is open again, so its remembered lines become markers again.  A
 * line past the end of the file is clamped to the last one, which is a
 * change the adapter has to hear about; nothing else here is. */
static void reanchor_buffer(struct editor_buffer *b)
{
	char path[PATH_MAX];
	struct dap_bp_source *s;
	size_t index = 0;
	bool moved = false;
	size_t j;

	if (!buffer_file(b, path)) {
		return;
	}
	s = source_find(path, &index);
	if (!s) {
		return;
	}
	for (j = 0; j < s->count; j++) {
		int was = s->items[j].line;

		if (s->items[j].anchored) {
			continue;
		}
		anchor_bp(b, &s->items[j]);
		moved = moved || s->items[j].line != was;
	}
	if (moved) {
		source_mutated(index);
	}
}

static enum kg_event_cb_status on_buffer_event(
    const struct kg_event *ev, enum kg_event_resolution resolution, void *ctx)
{
	(void)ctx;
	switch (ev->kind) {
	case KG_EVENT_BUFFER_OPENED:
		if (resolution == KG_EVENT_RESOLVED_LIVE) {
			reanchor_buffer(
			    buf_resolve(ev->payload.buffer_life.buffer));
		}
		break;
	case KG_EVENT_BUFFER_KILLING:
		demote_buffer(ev->payload.buffer_life.buffer);
		break;
	case KG_EVENT_BUFFER_CHANGED:
		refresh_buffer(ev->payload.changed.buffer);
		break;
	case KG_EVENT_BUFFER_BROAD_CHANGE:
		refresh_buffer(ev->payload.broad.buffer);
		break;
	default:
		break;
	}
	return KG_EVENT_CB_CONTINUE;
}

void dap_breakpoint_init(void)
{
	/* Idempotent, in lsp_sync_install()'s own shape: unsubscribing a
	 * token that names nothing, or one whose slot a later registration
	 * reused, does nothing (src/event.h). */
	(void)kg_event_unsubscribe(event_token);
	event_token = kg_event_subscribe((1u << KG_EVENT_BUFFER_OPENED)
		| (1u << KG_EVENT_BUFFER_KILLING)
		| (1u << KG_EVENT_BUFFER_CHANGED)
		| (1u << KG_EVENT_BUFFER_BROAD_CHANGE),
	    on_buffer_event, NULL);
}

void dap_breakpoint_reset(void)
{
	size_t i, j;

	for (i = 0; i < source_count; i++) {
		for (j = 0; j < sources[i]->count; j++) {
			if (sources[i]->items[j].anchored) {
				kg_marker_delete(sources[i]->items[j].marker);
			}
		}
		free(sources[i]);
		sources[i] = NULL;
	}
	source_count = 0;
	memset(flights, 0, sizeof(flights));
	stop_question_open = false;
	resync_due = false;
	(void)kg_event_unsubscribe(event_token);
	event_token = KG_EVENT_SUBSCRIBER_NONE;
}
