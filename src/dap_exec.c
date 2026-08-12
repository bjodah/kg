/* The stopped-state model and the requests that end it.
 *
 * src/dap_exec.h has the contract; this file has the waterfall, the three
 * counters that keep a late reply from painting, and the one oracle every
 * execution command asks.  It holds no editor state: everything a user
 * would see leaves through struct dap_exec_hooks.
 *
 * Every continuation below is a heap-allocated struct exec_req handed to
 * dap_client_request() as its context.  That is deliberate: the client's
 * contract is that a callback runs EXACTLY ONCE whatever happens -- answer,
 * refusal, deadline, death, a send that never left -- so freeing the
 * context in the callback is a complete ownership story with no path that
 * leaks and no path that frees twice.
 */

#include "dap_exec.h"

#include "dap_breakpoint.h"
#include "dap_client.h"
#include "dap_session.h"
#include "json.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* How long `dap-restart` waits for the old session to end before it stops
 * waiting.  A restart the user asked for is not allowed to hang on an
 * adapter that will not go: the session's own teardown ladder is bounded
 * already, and this is the backstop above it. */
#define DAP_EXEC_RESTART_GRACE_MS 2000u

#define DAP_EXEC_MAX_GOTO_TARGETS 16

struct exec_goto_target {
	int id;
	char label[DAP_EXEC_NAME_MAX];
};

/* One outstanding request's tags.  Which members mean anything depends on
 * the request, and each callback reads only its own. */
struct exec_req {
	unsigned epoch;
	unsigned selection;
	unsigned serial; /* `threads` only */
	int thread_id;
	int scope;
	int parent;
	int depth;
	/* `stackTrace`: 1 for the stop's first frame, DAP_EXEC_STACK_PAGE
	 * for the page, and 0 on a `threads` refresh that must NOT continue
	 * the waterfall. */
	int levels;
	enum dap_exec_step step;
};

static struct {
	unsigned epoch;
	unsigned selection;
	unsigned threads_serial;
	/* The epoch a resume has already been accounted for.  A `continued`
	 * arriving under a different one is unsolicited and bumps the epoch
	 * itself [M-4]; one arriving under this one is the answer to a
	 * resume kg sent, and is idempotent. */
	unsigned resumed_epoch;

	char reason[DAP_EXEC_TEXT_MAX];
	bool preserve_focus;
	int selected_thread;
	bool stop_all_threads;

	struct dap_exec_thread threads[DAP_EXEC_MAX_THREADS];
	size_t thread_count;

	struct dap_exec_frame *frames;
	size_t frame_count;
	size_t frame_capacity;
	size_t selected_frame;
	bool frames_paged;
	int pending_frame_delta;

	struct dap_exec_scope scopes[DAP_EXEC_MAX_SCOPES];
	size_t scope_count;

	struct dap_exec_variable *variables;
	size_t variable_count;
	size_t variable_capacity;

	struct exec_goto_target goto_targets[DAP_EXEC_MAX_GOTO_TARGETS];
	size_t goto_count;
	unsigned goto_selection;
	char goto_path[PATH_MAX];
	int goto_line;

	/* The debounced `thread` re-request. */
	bool threads_due;
	unsigned long long threads_due_ms;

	/* A client-side restart in progress, and when kg stops waiting. */
	bool relaunching;
	unsigned long long relaunch_deadline_ms;

	struct dap_exec_hooks hooks;
} g;

/* ------------------------------- plumbing ----------------------------- */

static unsigned long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ull
	    + (unsigned long long)(ts.tv_nsec / 1000000);
}

static struct dap_client *exec_client(void)
{
	return dap_session_client(dap_session_current());
}

static bool exec_capable(enum dap_capability cap)
{
	struct dap_client *c = exec_client();

	return c && dap_capable(c, cap);
}

static void exec_changed(void)
{
	if (g.hooks.changed) {
		g.hooks.changed(g.hooks.ctx);
	}
}

/* One line, bounded, with the C0 bytes turned into spaces.  An adapter's
 * words reach a user through here and nowhere else, and a message kg puts
 * in front of somebody is one line of text -- an adapter does not get to
 * spell a newline or an escape into it.  What makes the bytes safe to PAINT
 * is still display_glyph_at() at the other end. */
static void exec_copy_line(char *out, size_t out_size, const char *in)
{
	size_t i;

	snprintf(out, out_size, "%s", in ? in : "");
	for (i = 0; out[i]; i++) {
		if ((unsigned char)out[i] < 0x20 || out[i] == 0x7f) {
			out[i] = ' ';
		}
	}
}

static void exec_report(bool error, const char *fmt, ...)
{
	char text[DAP_EXEC_TEXT_MAX];
	va_list ap;

	if (!g.hooks.report) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	g.hooks.report(g.hooks.ctx, error, text);
}

/* Protocol ids are range-checked int32 [M-10]: the spec pins
 * `variablesReference` to (0, 2^31) and every id all four measured adapters
 * emitted fits, lldb-dap's OS-tid thread ids included.  Out of range is a
 * value kg refuses rather than a wider type. */
static bool exec_id(const struct kg_json_value *v, int *out)
{
	long long raw = kg_json_int(v, LLONG_MIN);

	if (raw < INT32_MIN || raw > INT32_MAX) {
		return false;
	}
	*out = (int)raw;
	return true;
}

static struct exec_req *exec_req_new(void)
{
	struct exec_req *req = calloc(1, sizeof(*req));

	if (req) {
		req->epoch = g.epoch;
		req->selection = g.selection;
		req->parent = -1;
		req->scope = -1;
	}
	return req;
}

/* Send `command`, or fail without leaking the context.  A request the
 * client refuses fails its own callback synchronously (src/dap_client.h),
 * which is what frees `req` on that path. */
static bool exec_send(const char *command, struct kg_jsonw *w,
    dap_response_fn cb, struct exec_req *req)
{
	struct dap_client *c = exec_client();
	char *body = NULL;
	size_t len = 0;

	if (!req) {
		kg_jsonw_free(w);
		return false;
	}
	if (!c) {
		kg_jsonw_free(w);
		free(req);
		return false;
	}
	if (kg_jsonw_finish(w, &body, &len) != 0) {
		free(req);
		return false;
	}
	(void)dap_client_request(c, command, body, len, cb, req);
	free(body);
	return true;
}

/* Whether a reply still describes the world it was asked about.  The epoch
 * is [M-4]'s stale-handle guard and the selection generation is the
 * same-stop one; a request that carries no selection (a `threads` refresh)
 * passes `false`. */
static bool exec_current(const struct exec_req *req, bool selection_matters)
{
	if (req->epoch != g.epoch) {
		return false;
	}
	return !selection_matters || req->selection == g.selection;
}

static const char *exec_error_text(const struct dap_response *r)
{
	return r->error ? r->error->message : "no answer";
}

/* ------------------------------ the model ----------------------------- */

static void free_frames(void)
{
	size_t i;

	for (i = 0; i < g.frame_count; i++) {
		free(g.frames[i].path);
	}
	free(g.frames);
	g.frames = NULL;
	g.frame_count = 0;
	g.frame_capacity = 0;
}

/* Everything a `variablesReference` or a `frameId` names.  Dropped whenever
 * the epoch moves, because after a resume those handles are answered with
 * success and wrong data by both measured adapters. */
static void clear_stop_view(void)
{
	free_frames();
	free(g.variables);
	g.variables = NULL;
	g.variable_count = 0;
	g.variable_capacity = 0;
	g.scope_count = 0;
	g.selected_frame = 0;
	g.frames_paged = false;
	g.pending_frame_delta = 0;
	g.goto_count = 0;
}

static void mark_threads_running(int thread_id, bool all)
{
	size_t i;

	for (i = 0; i < g.thread_count; i++) {
		if (all || g.threads[i].id == thread_id) {
			g.threads[i].stopped = false;
		}
	}
}

/* The spec's defaults, and they are not symmetric.  An absent or false
 * `allThreadsStopped` marks only the NAMED thread -- nbcode's minimal
 * stopped body must not mark every thread stopped -- while an absent
 * `allThreadsContinued` means every thread continued [M-10]. */
static void mark_threads_stopped(void)
{
	size_t i;

	for (i = 0; i < g.thread_count; i++) {
		g.threads[i].stopped = g.stop_all_threads
		    || g.threads[i].id == g.selected_thread;
	}
}

static void pick_thread(void)
{
	size_t i;

	for (i = 0; i < g.thread_count; i++) {
		if (g.threads[i].stopped) {
			g.selected_thread = g.threads[i].id;
			return;
		}
	}
	if (g.thread_count > 0) {
		g.selected_thread = g.threads[0].id;
		g.threads[0].stopped = true;
	}
}

/* ------------------------------- requests ----------------------------- */

static void on_threads(
    struct dap_client *c, const struct dap_response *r, void *ctx);
static void on_stack(
    struct dap_client *c, const struct dap_response *r, void *ctx);
static void on_scopes(
    struct dap_client *c, const struct dap_response *r, void *ctx);
static void on_variables(
    struct dap_client *c, const struct dap_response *r, void *ctx);

/* `levels` is 0 for a plain refresh and 1 for the stop's own request, which
 * is what tells the reply whether to continue the waterfall. */
static void request_threads(int levels)
{
	struct exec_req *req = exec_req_new();
	struct kg_jsonw w;

	if (req) {
		req->serial = ++g.threads_serial;
		req->levels = levels;
	}
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_end_object(&w);
	(void)exec_send("threads", &w, on_threads, req);
}

static void request_stack(int levels)
{
	struct exec_req *req = exec_req_new();
	struct kg_jsonw w;

	if (req) {
		req->levels = levels;
		req->thread_id = g.selected_thread;
	}
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "threadId");
	kg_jsonw_int(&w, g.selected_thread);
	kg_jsonw_key(&w, "startFrame");
	kg_jsonw_int(&w, 0);
	kg_jsonw_key(&w, "levels");
	kg_jsonw_int(&w, levels);
	kg_jsonw_end_object(&w);
	(void)exec_send("stackTrace", &w, on_stack, req);
}

static void request_scopes(void)
{
	struct exec_req *req;
	struct kg_jsonw w;

	if (g.selected_frame >= g.frame_count) {
		return;
	}
	req = exec_req_new();
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "frameId");
	kg_jsonw_int(&w, g.frames[g.selected_frame].id);
	kg_jsonw_end_object(&w);
	(void)exec_send("scopes", &w, on_scopes, req);
}

static void request_variables(int reference, int scope, int parent, int depth)
{
	struct exec_req *req = exec_req_new();
	struct kg_jsonw w;

	if (req) {
		req->scope = scope;
		req->parent = parent;
		req->depth = depth;
	}
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "variablesReference");
	kg_jsonw_int(&w, reference);
	kg_jsonw_end_object(&w);
	(void)exec_send("variables", &w, on_variables, req);
}

/* ------------------------------- threads ------------------------------ */

static void fill_threads(const struct kg_json_value *body)
{
	const struct kg_json_value *list = kg_json_get(body, "threads");
	size_t n = kg_json_len(list);
	size_t i;

	g.thread_count = 0;
	for (i = 0; i < n && g.thread_count < DAP_EXEC_MAX_THREADS; i++) {
		const struct kg_json_value *item = kg_json_at(list, i);
		struct dap_exec_thread *t = &g.threads[g.thread_count];

		if (!exec_id(kg_json_get(item, "id"), &t->id)) {
			continue;
		}
		exec_copy_line(t->name, sizeof(t->name),
		    kg_json_str(kg_json_get(item, "name"), NULL));
		t->stopped = false;
		g.thread_count++;
	}
}

static void on_threads(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct exec_req *req = ctx;

	(void)c;
	/* Only the NEWEST request's answer is taken: a second stop while
	 * this one was in flight has already moved the serial on, and its
	 * own answer is the one that describes the world. */
	if (req->serial == g.threads_serial && exec_current(req, false)
	    && r->success) {
		fill_threads(r->body);
		mark_threads_stopped();
		if (g.selected_thread == 0) {
			pick_thread();
		}
		if (req->levels > 0 && g.selected_thread != 0) {
			request_stack(1);
		}
		exec_changed();
	}
	free(req);
}

/* -------------------------------- frames ------------------------------ */

/* [M-11]: a frame is navigable only when it names an absolute path that is
 * there, as a regular file.  Relative paths (lldb-dap's `csu/libc-start.c`),
 * paths that do not exist, and `sourceReference`-only frames are listed and
 * left alone -- passing one to file visiting is how a debugger opens an
 * empty buffer named after a file nobody has. */
static bool frame_path_navigable(const char *path)
{
	struct stat st;

	if (!path || path[0] != '/') {
		return false;
	}
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool frames_reserve(size_t want)
{
	struct dap_exec_frame *grown;

	if (want <= g.frame_capacity) {
		return true;
	}
	grown = realloc(g.frames, want * sizeof(*grown));
	if (!grown) {
		return false;
	}
	memset(grown + g.frame_capacity, 0,
	    (want - g.frame_capacity) * sizeof(*grown));
	g.frames = grown;
	g.frame_capacity = want;
	return true;
}

static void fill_one_frame(
    struct dap_exec_frame *f, const struct kg_json_value *item)
{
	const struct kg_json_value *source = kg_json_get(item, "source");
	const char *path = kg_json_str(kg_json_get(source, "path"), NULL);

	exec_copy_line(f->name, sizeof(f->name),
	    kg_json_str(kg_json_get(item, "name"), NULL));
	f->line = (int)kg_json_int(kg_json_get(item, "line"), 0);
	f->column = (int)kg_json_int(kg_json_get(item, "column"), 0);
	f->path = path ? strdup(path) : NULL;
	f->navigable = f->line > 0 && frame_path_navigable(f->path);
}

static void fill_frames(const struct kg_json_value *body)
{
	const struct kg_json_value *list = kg_json_get(body, "stackFrames");
	size_t n = kg_json_len(list);
	size_t i;

	free_frames();
	if (n > DAP_EXEC_MAX_FRAMES) {
		n = DAP_EXEC_MAX_FRAMES;
	}
	if (n == 0 || !frames_reserve(n)) {
		return;
	}
	for (i = 0; i < n; i++) {
		const struct kg_json_value *item = kg_json_at(list, i);
		struct dap_exec_frame *f = &g.frames[g.frame_count];

		if (!exec_id(kg_json_get(item, "id"), &f->id)) {
			continue;
		}
		fill_one_frame(f, item);
		g.frame_count++;
	}
}

static size_t first_navigable_frame(void)
{
	size_t i;

	for (i = 0; i < g.frame_count; i++) {
		if (g.frames[i].navigable) {
			return i;
		}
	}
	return g.frame_count;
}

/* Take `index` as the selected frame: tell the caller where the program is,
 * and ask for that frame's scopes.  `user` is a selection change WITHIN one
 * stop, which is what the selection generation exists to guard. */
static void select_frame(size_t index, bool user)
{
	const struct dap_exec_frame *f;

	if (index >= g.frame_count) {
		return;
	}
	if (user) {
		g.selection++;
	}
	g.selected_frame = index;
	f = &g.frames[index];
	if (f->navigable) {
		/* The stop's own location is what the temporary-breakpoint
		 * protocol needs for adapters that omit `hitBreakpointIds`
		 * (measured on debugpy and nbcode) -- and it is the STOP's,
		 * not a frame the user walked to afterwards, or walking the
		 * stack over a `dap-until` line would remove a breakpoint
		 * nothing hit. */
		if (!user) {
			(void)dap_breakpoint_stopped_at(f->path, f->line);
		}
		if (g.hooks.location) {
			g.hooks.location(
			    g.hooks.ctx, f->path, f->line, !g.preserve_focus);
		}
	}
	request_scopes();
	exec_changed();
}

/* The stop's first answer: one frame.  Navigable, and that is where the
 * program is; otherwise a bounded page, because one frame alone cannot
 * "prefer the topmost navigable frame". */
static void stack_first_frame(void)
{
	if (g.frame_count == 0) {
		exec_report(true, "stopped, but the adapter reported no stack");
		exec_changed();
		return;
	}
	if (g.frames[0].navigable) {
		select_frame(0, false);
		return;
	}
	request_stack(DAP_EXEC_STACK_PAGE);
}

static void stack_page(void)
{
	int delta = g.pending_frame_delta;
	size_t index;

	g.frames_paged = true;
	g.pending_frame_delta = 0;
	if (delta != 0) {
		/* The page a stack walk asked for: apply the walk rather
		 * than re-deciding where the stop was. */
		(void)dap_exec_frame_select(delta);
		return;
	}
	index = first_navigable_frame();
	if (index >= g.frame_count) {
		/* Frame zero was not openable and neither is anything under
		 * it: the frames stay listed, source focus stays where the
		 * user left it, and kg says why.  The waterfall still runs
		 * -- a stop with no source kg can show is still a stop whose
		 * locals a user can read -- so frame zero is selected
		 * anyway, and select_frame() is what declines to navigate
		 * to it. */
		exec_report(false,
		    "Stopped where kg has no source: %zu frames, none openable",
		    g.frame_count);
		index = 0;
	}
	select_frame(index, false);
}

static void on_stack(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct exec_req *req = ctx;

	(void)c;
	if (!exec_current(req, true)) {
		free(req);
		return;
	}
	if (!r->success) {
		exec_report(true, "stackTrace failed: %s", exec_error_text(r));
		free(req);
		return;
	}
	fill_frames(r->body);
	if (req->levels == 1) {
		stack_first_frame();
	} else {
		stack_page();
	}
	free(req);
}

/* -------------------------- scopes and variables ---------------------- */

static void on_scopes(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct exec_req *req = ctx;
	const struct kg_json_value *list;
	size_t i, n;

	(void)c;
	if (!exec_current(req, true) || !r->success) {
		free(req);
		return;
	}
	list = kg_json_get(r->body, "scopes");
	n = kg_json_len(list);
	g.scope_count = 0;
	for (i = 0; i < n && g.scope_count < DAP_EXEC_MAX_SCOPES; i++) {
		const struct kg_json_value *item = kg_json_at(list, i);
		struct dap_exec_scope *s = &g.scopes[g.scope_count];

		if (!exec_id(kg_json_get(item, "variablesReference"),
			&s->variables_reference)) {
			continue;
		}
		exec_copy_line(s->name, sizeof(s->name),
		    kg_json_str(kg_json_get(item, "name"), NULL));
		s->expensive
		    = kg_json_bool(kg_json_get(item, "expensive"), false);
		g.scope_count++;
	}
	for (i = 0; i < g.scope_count; i++) {
		if (!g.scopes[i].expensive
		    && g.scopes[i].variables_reference > 0) {
			request_variables(
			    g.scopes[i].variables_reference, (int)i, -1, 0);
		}
	}
	free(req);
	exec_changed();
}

static bool variables_reserve(size_t want)
{
	struct dap_exec_variable *grown;
	size_t capacity = g.variable_capacity ? g.variable_capacity : 32;

	if (want <= g.variable_capacity) {
		return true;
	}
	while (capacity < want) {
		capacity *= 2;
	}
	if (capacity > DAP_EXEC_MAX_VARIABLES) {
		capacity = DAP_EXEC_MAX_VARIABLES;
	}
	grown = realloc(g.variables, capacity * sizeof(*grown));
	if (!grown) {
		return false;
	}
	g.variables = grown;
	g.variable_capacity = capacity;
	return true;
}

static void add_variable(
    const struct kg_json_value *item, const struct exec_req *req)
{
	struct dap_exec_variable *v;

	if (g.variable_count >= DAP_EXEC_MAX_VARIABLES
	    || !variables_reserve(g.variable_count + 1)) {
		return;
	}
	v = &g.variables[g.variable_count];
	memset(v, 0, sizeof(*v));
	exec_copy_line(v->name, sizeof(v->name),
	    kg_json_str(kg_json_get(item, "name"), NULL));
	exec_copy_line(v->value, sizeof(v->value),
	    kg_json_str(kg_json_get(item, "value"), NULL));
	exec_copy_line(v->type, sizeof(v->type),
	    kg_json_str(kg_json_get(item, "type"), NULL));
	if (!exec_id(kg_json_get(item, "variablesReference"),
		&v->variables_reference)) {
		v->variables_reference = 0;
	}
	v->scope = req->scope;
	v->parent = req->parent;
	v->depth = req->depth;
	g.variable_count++;
}

static void on_variables(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct exec_req *req = ctx;
	const struct kg_json_value *list;
	size_t i, n;

	(void)c;
	if (!exec_current(req, true) || !r->success) {
		free(req);
		return;
	}
	list = kg_json_get(r->body, "variables");
	n = kg_json_len(list);
	for (i = 0; i < n; i++) {
		add_variable(kg_json_at(list, i), req);
	}
	free(req);
	exec_changed();
}

/* An adapter that hands back a reference already in a node's own ancestry
 * is describing a cycle; following it is how a variables pane becomes
 * infinite. */
static bool reference_in_ancestry(size_t index, int reference)
{
	int at = (int)index;
	int guard = DAP_EXEC_MAX_DEPTH + 1;

	while (at >= 0 && guard-- > 0) {
		if (g.variables[at].variables_reference == reference) {
			return true;
		}
		at = g.variables[at].parent;
	}
	return false;
}

bool dap_exec_expand_variable(size_t index)
{
	const struct dap_exec_variable *v;

	if (index >= g.variable_count) {
		return false;
	}
	v = &g.variables[index];
	if (v->variables_reference <= 0 || v->depth + 1 >= DAP_EXEC_MAX_DEPTH) {
		return false;
	}
	if (v->parent >= 0
	    && reference_in_ancestry(
		(size_t)v->parent, v->variables_reference)) {
		return false;
	}
	request_variables(
	    v->variables_reference, v->scope, (int)index, v->depth + 1);
	return true;
}

/* -------------------------------- stopping ---------------------------- */

static void on_stopped(const struct kg_json_value *body)
{
	const struct kg_json_value *tid = kg_json_get(body, "threadId");
	int thread_id = 0;

	/* An accepted stop bumps the epoch [M-4] and opens a new selection
	 * generation: nothing asked for under the previous stop may paint. */
	g.epoch++;
	g.selection++;
	g.resumed_epoch = 0;
	clear_stop_view();
	/* An OPEN string, never a switch: lldb-dap's stopOnEntry says
	 * "exception" and "signal SIGSTOP", nbcode's step says "pause". */
	exec_copy_line(g.reason, sizeof(g.reason),
	    kg_json_str(kg_json_get(body, "reason"), NULL));
	g.preserve_focus
	    = kg_json_bool(kg_json_get(body, "preserveFocusHint"), false);
	g.stop_all_threads
	    = kg_json_bool(kg_json_get(body, "allThreadsStopped"), false);
	/* `threadId` is optional.  With none, the selection waits for
	 * `threads` -- a `stackTrace` with a guessed thread is a debugger
	 * showing somebody else's stack. */
	g.selected_thread = (kg_json_kind_of(tid) == KG_JSON_NUMBER
				&& exec_id(tid, &thread_id))
	    ? thread_id
	    : 0;
	mark_threads_stopped();
	(void)dap_breakpoint_stopped(body);
	request_threads(1);
	exec_changed();
}

/* Idempotent for all three orderings a `continued` arrives in: before the
 * response (delve), after it (lldb-dap), or never [M-10]. */
static void apply_continued(int thread_id, bool all)
{
	if (g.resumed_epoch != g.epoch) {
		/* Unsolicited: the program is running and kg did not ask,
		 * so every handle it holds is stale from now [M-4]. */
		g.epoch++;
		g.resumed_epoch = g.epoch;
	}
	mark_threads_running(thread_id, all);
	clear_stop_view();
	g.reason[0] = '\0';
	dap_session_note_resumed(dap_session_current());
	exec_changed();
}

static void on_continued(const struct kg_json_value *body)
{
	int thread_id = 0;

	(void)exec_id(kg_json_get(body, "threadId"), &thread_id);
	/* The deliberate asymmetry: an ABSENT `allThreadsContinued` means
	 * ALL threads continued, where an absent `allThreadsStopped` means
	 * only the named one stopped [M-10]. */
	apply_continued(thread_id,
	    kg_json_bool(kg_json_get(body, "allThreadsContinued"), true));
}

/* Three `thread` events in 70 ms were measured from one `t.start()`.  The
 * event opens a quiet period and nothing else; dap_exec_poll() is what
 * eventually asks. */
static void on_thread_event(void)
{
	g.threads_due = true;
	g.threads_due_ms = now_ms() + DAP_EXEC_THREAD_DEBOUNCE_MS;
}

void dap_exec_event(
    void *ctx, const char *event, const struct kg_json_value *body)
{
	(void)ctx;
	if (strcmp(event, "stopped") == 0) {
		on_stopped(body);
	} else if (strcmp(event, "continued") == 0) {
		on_continued(body);
	} else if (strcmp(event, "thread") == 0) {
		on_thread_event();
	} else if (strcmp(event, "breakpoint") == 0) {
		dap_breakpoint_event(body);
		exec_changed();
	}
}

/* ------------------------------- execution ---------------------------- */

const char *dap_exec_no_thread_text(void)
{
	return "No stopped thread: the program is not suspended";
}

bool dap_exec_stopped_thread(int *out)
{
	struct dap_session *s = dap_session_current();
	struct dap_session_state st;
	size_t i;

	if (!s) {
		return false;
	}
	dap_session_get_state(s, &st);
	if (st.phase != DAP_PHASE_STOPPED) {
		return false;
	}
	if (g.selected_thread != 0) {
		*out = g.selected_thread;
		return true;
	}
	for (i = 0; i < g.thread_count; i++) {
		if (g.threads[i].stopped) {
			/* The oracle is also where the selection settles: a
			 * stop with no `threadId` leaves it open until the
			 * thread list arrives, and every later request needs
			 * one answer rather than a fresh search. */
			g.selected_thread = g.threads[i].id;
			*out = g.selected_thread;
			return true;
		}
	}
	return false;
}

static const char *step_command(enum dap_exec_step step)
{
	switch (step) {
	case DAP_EXEC_CONTINUE:
		return "continue";
	case DAP_EXEC_STEP_IN:
		return "stepIn";
	case DAP_EXEC_STEP_OUT:
		return "stepOut";
	case DAP_EXEC_NEXT:
	case DAP_EXEC_STEP_INSTRUCTION:
		break;
	}
	return "next";
}

static void on_resumed(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct exec_req *req = ctx;
	bool all;

	(void)c;
	if (!exec_current(req, false)) {
		free(req);
		return;
	}
	if (!r->success) {
		/* Back to STOPPED and refetch: the epoch already moved when
		 * the request went out, so the frame handles were
		 * conservatively invalidated and have to be asked for
		 * again. */
		exec_report(true, "%s failed: %s", step_command(req->step),
		    exec_error_text(r));
		dap_session_note_stopped(dap_session_current());
		g.selection++;
		request_stack(1);
		free(req);
		return;
	}
	/* Synthesised locally on every success, because an adapter may send
	 * the real one before this response, after it, or never [M-10]. */
	all = req->step == DAP_EXEC_CONTINUE
	    && kg_json_bool(kg_json_get(r->body, "allThreadsContinued"), true);
	apply_continued(req->thread_id, all);
	free(req);
}

bool dap_exec_resume(enum dap_exec_step step)
{
	struct exec_req *req;
	struct kg_jsonw w;
	int thread_id;

	if (!dap_exec_stopped_thread(&thread_id)) {
		exec_report(true, "%s", dap_exec_no_thread_text());
		return false;
	}
	if (!dap_session_begin_resume(dap_session_current())) {
		exec_report(true, "The program is already running");
		return false;
	}
	/* BEFORE the send, never only on the next stop [M-4]. */
	g.epoch++;
	g.resumed_epoch = g.epoch;
	clear_stop_view();
	req = exec_req_new();
	if (req) {
		req->thread_id = thread_id;
		req->step = step;
	}
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "threadId");
	kg_jsonw_int(&w, thread_id);
	/* `granularity` only if the adapter said it understands one. */
	if (step == DAP_EXEC_STEP_INSTRUCTION
	    && exec_capable(DAP_CAP_STEPPING_GRANULARITY)) {
		kg_jsonw_key(&w, "granularity");
		kg_jsonw_string(&w, "instruction");
	}
	kg_jsonw_end_object(&w);
	exec_changed();
	return exec_send(step_command(step), &w, on_resumed, req);
}

static void on_simple(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct exec_req *req = ctx;

	(void)c;
	if (!r->success) {
		exec_report(
		    true, "%s failed: %s", r->command, exec_error_text(r));
	}
	free(req);
}

bool dap_exec_pause(void)
{
	struct dap_session *s = dap_session_current();
	struct dap_session_state st;
	struct kg_jsonw w;

	if (!s) {
		exec_report(true, "No debug session");
		return false;
	}
	dap_session_get_state(s, &st);
	if (st.phase == DAP_PHASE_STOPPED) {
		exec_report(false, "The program is already stopped");
		return false;
	}
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "threadId");
	kg_jsonw_int(&w, g.selected_thread ? g.selected_thread : 1);
	kg_jsonw_end_object(&w);
	return exec_send("pause", &w, on_simple, exec_req_new());
}

/* --------------------------------- goto ------------------------------- */

static void send_goto(int target_id)
{
	struct exec_req *req;
	struct kg_jsonw w;
	int thread_id;

	if (!dap_exec_stopped_thread(&thread_id)) {
		exec_report(true, "%s", dap_exec_no_thread_text());
		return;
	}
	/* Resume-implying: the adapter answers with a fresh `stopped`, and
	 * every handle kg holds is about the old one [M-4]. */
	g.epoch++;
	clear_stop_view();
	(void)dap_session_begin_resume(dap_session_current());
	req = exec_req_new();
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "threadId");
	kg_jsonw_int(&w, thread_id);
	kg_jsonw_key(&w, "targetId");
	kg_jsonw_int(&w, target_id);
	kg_jsonw_end_object(&w);
	(void)exec_send("goto", &w, on_simple, req);
}

static void fill_goto_targets(const struct kg_json_value *body)
{
	const struct kg_json_value *list = kg_json_get(body, "targets");
	size_t n = kg_json_len(list);
	size_t i;

	g.goto_count = 0;
	for (i = 0; i < n && g.goto_count < DAP_EXEC_MAX_GOTO_TARGETS; i++) {
		const struct kg_json_value *item = kg_json_at(list, i);
		struct exec_goto_target *t = &g.goto_targets[g.goto_count];

		if (!exec_id(kg_json_get(item, "id"), &t->id)) {
			continue;
		}
		exec_copy_line(t->label, sizeof(t->label),
		    kg_json_str(kg_json_get(item, "label"), NULL));
		g.goto_count++;
	}
}

static void on_goto_targets(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct exec_req *req = ctx;

	(void)c;
	if (!exec_current(req, true)) {
		free(req);
		return;
	}
	if (!r->success) {
		exec_report(true, "gotoTargets failed: %s", exec_error_text(r));
		free(req);
		return;
	}
	fill_goto_targets(r->body);
	g.goto_selection = g.selection;
	if (g.goto_count == 0) {
		exec_report(true, "No jump target on line %d", g.goto_line);
	} else if (g.goto_count == 1) {
		send_goto(g.goto_targets[0].id);
	} else {
		exec_report(false,
		    "%zu jump targets on line %d: dap-goto again to choose",
		    g.goto_count, g.goto_line);
	}
	free(req);
}

bool dap_exec_goto(const char *path, int line)
{
	struct exec_req *req;
	struct kg_jsonw w;
	int thread_id;

	if (!exec_capable(DAP_CAP_GOTO_TARGETS)) {
		exec_report(true, "This adapter cannot jump to a line");
		return false;
	}
	if (!dap_exec_stopped_thread(&thread_id)) {
		exec_report(true, "%s", dap_exec_no_thread_text());
		return false;
	}
	snprintf(g.goto_path, sizeof(g.goto_path), "%s", path ? path : "");
	g.goto_line = line;
	g.goto_count = 0;
	req = exec_req_new();
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "source");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "path");
	kg_jsonw_string(&w, g.goto_path);
	kg_jsonw_end_object(&w);
	kg_jsonw_key(&w, "line");
	kg_jsonw_int(&w, line);
	/* Column 1: kg navigates by line, and a DAP column is not a byte
	 * offset into a row (doc/coordinates.md). */
	kg_jsonw_key(&w, "column");
	kg_jsonw_int(&w, 1);
	kg_jsonw_end_object(&w);
	return exec_send("gotoTargets", &w, on_goto_targets, req);
}

size_t dap_exec_goto_target_count(void)
{
	return g.goto_selection == g.selection ? g.goto_count : 0;
}

const char *dap_exec_goto_target_name(size_t index)
{
	if (index >= dap_exec_goto_target_count()) {
		return NULL;
	}
	return g.goto_targets[index].label;
}

bool dap_exec_goto_chosen(size_t index)
{
	if (index >= dap_exec_goto_target_count()) {
		return false;
	}
	send_goto(g.goto_targets[index].id);
	return true;
}

/* -------------------------------- restart ----------------------------- */

bool dap_exec_restart(void)
{
	struct dap_session *s = dap_session_current();
	struct kg_jsonw w;

	if (!s) {
		exec_report(true, "No debug session");
		return false;
	}
	g.epoch++;
	clear_stop_view();
	if (exec_capable(DAP_CAP_RESTART_REQUEST)) {
		/* The spec's stated preference, and the reason the
		 * capability is read live: lldb-dap advertises `restart`
		 * through the capabilities event rather than at initialize
		 * [M-7]. */
		kg_jsonw_init(&w);
		kg_jsonw_begin_object(&w);
		kg_jsonw_end_object(&w);
		return exec_send("restart", &w, on_simple, exec_req_new());
	}
	/* Client-side: end this session and relaunch the same configuration.
	 * The breakpoint table and the window layout are editor property and
	 * are not touched by either half. */
	g.relaunching = true;
	g.relaunch_deadline_ms = now_ms() + DAP_EXEC_RESTART_GRACE_MS;
	dap_session_end(s, DAP_END_TERMINATE);
	return true;
}

/* ------------------------------- evaluate ----------------------------- */

static void on_evaluate(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct exec_req *req = ctx;
	char text[DAP_EXEC_TEXT_MAX];

	(void)c;
	if (!exec_current(req, true)) {
		free(req);
		return;
	}
	if (!r->success) {
		/* A failed evaluate renders in place; it is not a session
		 * error [M-14]. */
		exec_report(true, "%s", exec_error_text(r));
		free(req);
		return;
	}
	exec_copy_line(text, sizeof(text),
	    kg_json_str(kg_json_get(r->body, "result"), NULL));
	exec_report(false, "%s", text);
	free(req);
}

bool dap_exec_evaluate(const char *expression, const char *context)
{
	struct kg_jsonw w;

	if (!exec_client()) {
		exec_report(true, "No debug session");
		return false;
	}
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "expression");
	kg_jsonw_string(&w, expression ? expression : "");
	kg_jsonw_key(&w, "context");
	kg_jsonw_string(&w, context ? context : "hover");
	if (g.selected_frame < g.frame_count) {
		kg_jsonw_key(&w, "frameId");
		kg_jsonw_int(&w, g.frames[g.selected_frame].id);
	}
	kg_jsonw_end_object(&w);
	return exec_send("evaluate", &w, on_evaluate, exec_req_new());
}

/* ----------------------------- walking frames ------------------------- */

bool dap_exec_frame_select(int delta)
{
	size_t want;
	int thread_id;

	if (!dap_exec_stopped_thread(&thread_id)) {
		exec_report(true, "%s", dap_exec_no_thread_text());
		return false;
	}
	if (!g.frames_paged) {
		/* The stop fetched one frame; the page is what a stack walk
		 * needs, and the selection is applied when it lands. */
		g.pending_frame_delta = delta;
		request_stack(DAP_EXEC_STACK_PAGE);
		return true;
	}
	if (delta < 0 && g.selected_frame == 0) {
		exec_report(false, "Already at the innermost frame");
		return false;
	}
	want = (size_t)((long)g.selected_frame + delta);
	if (want >= g.frame_count) {
		exec_report(false, "Already at the outermost frame");
		return false;
	}
	select_frame(want, true);
	return true;
}

/* -------------------------------- lifecycle --------------------------- */

void dap_exec_set_hooks(const struct dap_exec_hooks *hooks)
{
	if (hooks) {
		g.hooks = *hooks;
	} else {
		memset(&g.hooks, 0, sizeof(g.hooks));
	}
}

void dap_exec_session_started(void)
{
	g.epoch++;
	g.selection++;
	g.resumed_epoch = 0;
	g.selected_thread = 0;
	g.thread_count = 0;
	g.threads_due = false;
	g.reason[0] = '\0';
	clear_stop_view();
}

void dap_exec_session_ended(void)
{
	/* On session end too [M-4]: a reply that outlives its adapter must
	 * not paint either. */
	g.epoch++;
	g.selection++;
	g.resumed_epoch = 0;
	g.selected_thread = 0;
	g.thread_count = 0;
	g.threads_due = false;
	g.reason[0] = '\0';
	clear_stop_view();
	dap_breakpoint_session_ended();
	exec_changed();
}

/* The one place a dead session is reaped, and the one place a client-side
 * restart's relaunch is started -- both at the top level, never from inside
 * a response callback. */
static int session_tick(void)
{
	struct dap_session *s = dap_session_current();
	struct dap_session_state st;
	bool overdue;

	if (s) {
		dap_session_get_state(s, &st);
		overdue = g.relaunching && now_ms() >= g.relaunch_deadline_ms;
		if (st.phase != DAP_PHASE_DEAD && !overdue) {
			return 0;
		}
		dap_session_close(s);
		dap_exec_session_ended();
	}
	if (!g.relaunching) {
		return s != NULL;
	}
	g.relaunching = false;
	if (g.hooks.relaunch) {
		g.hooks.relaunch(g.hooks.ctx);
	}
	return 1;
}

int dap_exec_poll(void)
{
	int changed = session_tick();

	if (g.threads_due && now_ms() >= g.threads_due_ms) {
		g.threads_due = false;
		request_threads(0);
		changed = 1;
	}
	return changed;
}

/* ------------------------------- accessors ---------------------------- */

unsigned dap_exec_epoch(void) { return g.epoch; }

unsigned dap_exec_selection(void) { return g.selection; }

const char *dap_exec_stop_reason(void) { return g.reason; }

int dap_exec_selected_thread(void) { return g.selected_thread; }

size_t dap_exec_thread_count(void) { return g.thread_count; }

bool dap_exec_thread(size_t index, struct dap_exec_thread *out)
{
	if (index >= g.thread_count) {
		return false;
	}
	*out = g.threads[index];
	return true;
}

size_t dap_exec_frame_count(void) { return g.frame_count; }

bool dap_exec_frame(size_t index, struct dap_exec_frame *out)
{
	if (index >= g.frame_count) {
		return false;
	}
	*out = g.frames[index];
	return true;
}

size_t dap_exec_selected_frame(void) { return g.selected_frame; }

size_t dap_exec_scope_count(void) { return g.scope_count; }

bool dap_exec_scope(size_t index, struct dap_exec_scope *out)
{
	if (index >= g.scope_count) {
		return false;
	}
	*out = g.scopes[index];
	return true;
}

size_t dap_exec_variable_count(void) { return g.variable_count; }

bool dap_exec_variable(size_t index, struct dap_exec_variable *out)
{
	if (index >= g.variable_count) {
		return false;
	}
	*out = g.variables[index];
	return true;
}

bool dap_exec_current_location(char *path, size_t path_size, int *line)
{
	const struct dap_exec_frame *f;

	if (g.selected_frame >= g.frame_count) {
		return false;
	}
	f = &g.frames[g.selected_frame];
	if (!f->navigable) {
		return false;
	}
	snprintf(path, path_size, "%s", f->path);
	*line = f->line;
	return true;
}
