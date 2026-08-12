/* The debugger's commands.  src/dap_commands.h has the contract.
 *
 * Two halves, as src/xref.c has: the real one, and the one a WITH_DAP=0
 * editor links, which answers every command with the same sentence rather
 * than pretending a feature `kg -V` denies is there.
 */

#include "dap_commands.h"

#ifdef KG_USE_DAP

#include "bufhandle.h"
#include "bufmgr.h"
#include "compile.h"
#include "dap_breakpoint.h"
#include "dap_client.h"
#include "dap_config.h"
#include "dap_decor.h"
#include "dap_exec.h"
#include "dap_session.h"
#include "def.h"
#include "json.h"
#include "visit.h"
#include "winmgr.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DAP_WHO "dap-debug"

/* The debuggee's transcript, bounded.  The UI subplan owns the real one --
 * append-only, CRLF normalisation as streaming state, a visible truncation
 * marker, killing the buffer starting a fresh one -- and this is the honest
 * minimum under it: the bytes arrive, they are bounded, and past the bound
 * kg says so once and keeps draining, because an adapter must never block
 * on its pipe. */
#define DAP_OUTPUT_MAX_BYTES (256u * 1024u)

/* What the user last chose, so `dap-debug` can be repeated and
 * `dap-restart` can relaunch the same thing without asking again. */
static struct {
	char filename[PATH_MAX];
	char config_name[DAP_CONFIG_NAME_MAX];
	char program[PATH_MAX];
	bool have_program;
	bool valid;
} choice;

/* The gdb-many-windows layout.  `wanted` is the user's toggle and survives
 * a session; `active` is whether the grid is on screen right now. */
static struct {
	struct kg_window_configuration saved;
	bool wanted;
	bool active;
} layout;

static struct {
	size_t bytes;
	bool truncated;
} transcript;

static unsigned build_generation;
static bool build_running;

static void dap_launch(bool run_build);

/* ------------------------------- helpers ------------------------------ */

static int current_row(void) { return editor_current_filerow(); }

/* The file the current buffer visits, or false for one that visits none.
 * Every command that names a source asks this, so "you cannot do that in a
 * scratch buffer" is one sentence in one place. */
static bool current_file(char *out, size_t out_size)
{
	if (bcur()->no_file || !bcur()->filename
	    || bcur()->filename[0] == '\0') {
		return false;
	}
	snprintf(out, out_size, "%s", bcur()->filename);
	return true;
}

static void report(bool error, const char *text)
{
	(void)error;
	editor_set_status_message("%s", text);
}

/* ------------------------------ the panes ----------------------------- */

/* The six panes the layout arranges.  Their CONTENTS are subplan 02's --
 * the row metadata, the model-derived replacements, the RET dispatch -- and
 * what stage 6 owns is that they exist, that they are read-only, and that
 * the layout can put them somewhere.  Creating one is therefore
 * find-or-create and never re-prepare: `buf_prepare_special_text()` clears
 * what it finds, and clearing the output transcript every time the layout
 * is toggled would be a debugger that forgets what the program printed. */
static const char *const pane_names[] = {
	"*dap-stack*",
	"*dap-breakpoints*",
	"*dap-locals*",
	"*dap-output*",
	"*dap-repl*",
};

#define DAP_PANE_COUNT (sizeof(pane_names) / sizeof(*pane_names))

static int pane_slot(const char *name)
{
	int slot = buf_find_open(name);

	if (slot >= 0) {
		return slot;
	}
	slot = buf_prepare_special_text(name, &text_syntax, 1);
	if (slot >= 0) {
		(void)buf_append_special_text(slot,
		    "(the debugger fills this pane)\n",
		    strlen("(the debugger fills this pane)\n"));
	}
	return slot;
}

/* stdout/stderr/console, appended as BYTES: debugpy splits one print across
 * two events mid-line, so an event is not a line [M-12].  Telemetry never
 * reaches here -- the client drops it. */
static void on_output(
    void *ctx, const char *category, const char *text, size_t len)
{
	int slot;

	(void)ctx;
	(void)category;
	if (transcript.bytes >= DAP_OUTPUT_MAX_BYTES) {
		return;
	}
	slot = pane_slot("*dap-output*");
	if (slot < 0) {
		return;
	}
	if (transcript.bytes + len > DAP_OUTPUT_MAX_BYTES) {
		len = DAP_OUTPUT_MAX_BYTES - transcript.bytes;
		transcript.truncated = true;
	}
	transcript.bytes += len;
	(void)buf_append_special_text(slot, text, len);
	if (transcript.truncated) {
		(void)buf_append_special_text(slot, "\n[output truncated]\n",
		    strlen("\n[output truncated]\n"));
		transcript.bytes = DAP_OUTPUT_MAX_BYTES;
	}
}

/* -------------------------------- layout ------------------------------ */

/* Two columns of three: the source, the stack and the breakpoints on the
 * left; locals, output and the REPL on the right.  Six windows of the
 * eight a frame can hold, which is gdb-many-windows' shape with kg's own
 * equal-width reflow. */
static void layout_apply(void)
{
	struct kg_buffer_handle buffers[1 + DAP_PANE_COUNT];
	int rows_per_column[2] = { 3, 3 };
	size_t i;
	int slot;

	if (layout.active) {
		return;
	}
	buffers[0] = buf_handle(buf_current);
	for (i = 0; i < DAP_PANE_COUNT; i++) {
		slot = pane_slot(pane_names[i]);
		if (slot < 0) {
			editor_set_status_message(
			    "dap-many-windows: no room for the panes");
			return;
		}
		buffers[i + 1] = buf_handle(slot);
	}
	/* Saved BEFORE the grid, and restored on toggle-off or on session
	 * end: a debugger that leaves a user's windows rearranged is one
	 * they stop using. */
	win_configuration_save(&layout.saved);
	if (win_arrange_grid(
		2, rows_per_column, buffers, (int)(1 + DAP_PANE_COUNT), 0)
	    != KG_WINDOW_LAYOUT_OK) {
		win_configuration_clear(&layout.saved);
		editor_set_status_message(
		    "dap-many-windows: the debug layout does not fit");
		return;
	}
	layout.active = true;
}

static void layout_restore(void)
{
	if (!layout.active) {
		return;
	}
	layout.active = false;
	(void)win_configuration_restore(&layout.saved);
	win_configuration_clear(&layout.saved);
}

void editor_dap_many_windows(int fd)
{
	(void)fd;
	layout.wanted = !layout.active;
	if (layout.wanted) {
		layout_apply();
		layout.wanted = layout.active;
	} else {
		layout_restore();
	}
	editor_set_status_message(
	    "Debug layout %s", layout.active ? "on" : "off");
}

/* ------------------------------ session news -------------------------- */

static void on_session_changed(void *ctx, struct dap_session *session)
{
	(void)ctx;
	(void)session;
	dap_decor_refresh();
}

static void on_session_report(void *ctx, bool error, const char *text)
{
	(void)ctx;
	report(error, text);
}

/* Where the program stopped.  `focus` is false when the stop carried
 * `preserveFocusHint`: the decoration moves, the user's window does not. */
static void on_location(void *ctx, const char *path, int line, bool focus)
{
	struct kg_buffer_handle none = { 0 };

	(void)ctx;
	if (focus) {
		(void)editor_visit_file_position("dap", path, line, 1, none);
	}
	dap_decor_refresh();
}

static void on_exec_changed(void *ctx)
{
	(void)ctx;
	if (!dap_session_current() && layout.active) {
		layout_restore();
	}
	dap_decor_refresh();
}

static void on_relaunch(void *ctx)
{
	(void)ctx;
	/* The configuration, the breakpoint table and the layout are all
	 * still here; only the adapter is new.  The build step is not re-run
	 * -- a restart restarts the program, and rebuilding it is what
	 * `dap-debug` is for. */
	dap_launch(false);
	if (layout.wanted) {
		layout_apply();
	}
}

/* ------------------------------- starting ----------------------------- */

static const struct dap_launch_config *find_config(
    const struct dap_config_set *set, const char *name)
{
	size_t i;

	for (i = 0; i < set->count; i++) {
		if (strcmp(set->configs[i].name, name) == 0) {
			return &set->configs[i];
		}
	}
	return NULL;
}

/* The one configuration lldb-dap's built-in row does not carry: a program
 * to debug.  Written as a MERGE through the JSON layer rather than as a
 * splice into the expanded bytes -- a path with a quote or a backslash in
 * it is a value, never syntax (src/dap_config.h says the same about
 * substitution). */
static bool merge_program(char **arguments, size_t *len, const char *program)
{
	struct kg_json *doc = kg_json_parse(*arguments, *len, NULL);
	const struct kg_json_value *root = kg_json_root(doc);
	struct kg_jsonw w;
	char *merged = NULL;
	size_t merged_len = 0, i, n;

	if (!doc) {
		return false;
	}
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "program");
	kg_jsonw_string(&w, program);
	n = kg_json_len(root);
	for (i = 0; i < n; i++) {
		size_t key_len = 0;
		const char *key = kg_json_key_at(root, i, &key_len);

		if (!key || strcmp(key, "program") == 0) {
			continue;
		}
		kg_jsonw_keyn(&w, key, key_len);
		kg_jsonw_value(&w, kg_json_at(root, i));
	}
	kg_jsonw_end_object(&w);
	kg_json_free(doc);
	if (kg_jsonw_finish(&w, &merged, &merged_len) != 0) {
		return false;
	}
	free(*arguments);
	*arguments = merged;
	*len = merged_len;
	return true;
}

static bool start_session(const struct dap_launch_config *cfg,
    const struct dap_adapter_spec *spec, const char *arguments, size_t len)
{
	struct dap_session_hooks hooks = {
		.changed = on_session_changed,
		.report = on_session_report,
		.output = on_output,
		.event = dap_exec_event,
		.sources = dap_breakpoint_session_sources,
		.breakpoints_answered = dap_breakpoint_session_answered,
	};
	struct dap_session_request request
	    = { spec, cfg->request, arguments, len, cfg->name, &hooks };
	char error[DAP_CONFIG_MESSAGE_MAX] = "";

	if (!dap_session_start(&request, error, sizeof(error))) {
		editor_set_status_message(DAP_WHO ": %s", error);
		return false;
	}
	dap_exec_session_started();
	transcript.bytes = 0;
	transcript.truncated = false;
	dap_decor_refresh();
	editor_set_status_message("Debugging: %s", cfg->name);
	return true;
}

static void launch_resolved(
    const struct dap_config_set *set, const struct dap_launch_config *cfg)
{
	struct dap_config_context_store store;
	struct dap_config_context ctx;
	struct dap_adapter_spec spec;
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	char message[DAP_CONFIG_MESSAGE_MAX];
	char *arguments = NULL;
	size_t len = 0;

	dap_config_context_for(choice.filename, set, &store, &ctx);
	if (dap_config_resolve_adapter(cfg, &ctx, &spec, &err) != 0
	    || dap_config_expand_arguments(cfg, &ctx, &arguments, &len, &err)
		!= 0) {
		dap_config_error_format(&err, message, sizeof(message));
		editor_set_status_message(DAP_WHO ": %s", message);
		free(arguments);
		return;
	}
	if (choice.have_program
	    && !merge_program(&arguments, &len, choice.program)) {
		editor_set_status_message(
		    DAP_WHO ": could not add the program to the arguments");
		free(arguments);
		return;
	}
	(void)start_session(cfg, &spec, arguments, len);
	free(arguments);
}

static void on_build_done(const struct compilation_result *result, void *ctx)
{
	(void)ctx;
	build_running = false;
	if (result->status != COMPILATION_DONE_EXITED
	    || result->exit_code != 0) {
		editor_set_status_message(
		    DAP_WHO ": the build step failed; not launching");
		return;
	}
	/* Only ever from compilation_deliver_completion(), which is a top
	 * level: starting a session here cannot land underneath a prompt. */
	dap_launch(false);
}

static bool start_build(const struct dap_launch_config *cfg)
{
	if (compilation_start_programmatic(cfg->build_command, cfg->build_cwd,
		buf_handle(buf_current), on_build_done, NULL, &build_generation)
	    != COMPILATION_ACCEPTED) {
		editor_set_status_message(
		    DAP_WHO ": a compilation is already running");
		return false;
	}
	build_running = true;
	editor_set_status_message("Building: %s", cfg->build_command);
	return true;
}

/* Load, find, and either build or start.  Re-derived from the file each
 * time rather than cached, so an edited `.kg-dap.json` takes effect at the
 * next `dap-debug` and a relaunch reads the same file the first launch
 * did. */
static void dap_launch(bool run_build)
{
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	char message[DAP_CONFIG_MESSAGE_MAX];
	struct dap_config_set *set;
	const struct dap_launch_config *cfg;

	if (!choice.valid) {
		editor_set_status_message(DAP_WHO ": nothing chosen yet");
		return;
	}
	set = dap_config_load(choice.filename, &err);
	if (!set) {
		dap_config_error_format(&err, message, sizeof(message));
		editor_set_status_message(DAP_WHO ": %s", message);
		return;
	}
	cfg = find_config(set, choice.config_name);
	if (!cfg) {
		editor_set_status_message(
		    DAP_WHO ": no configuration named %s", choice.config_name);
	} else if (run_build && cfg->has_build) {
		(void)start_build(cfg);
	} else {
		launch_resolved(set, cfg);
	}
	dap_config_free(set);
}

/* ------------------------------- dap-debug ---------------------------- */

/* The chooser.  One configuration runs without a question; several ask, and
 * the last successful choice is the default, which is what makes the
 * command repeatable. */
static bool choose_config(int fd, const struct dap_config_set *set)
{
	char name[DAP_CONFIG_NAME_MAX];
	size_t i;

	if (set->count == 1) {
		snprintf(choice.config_name, sizeof(choice.config_name), "%s",
		    set->configs[0].name);
		return true;
	}
	snprintf(name, sizeof(name), "%s",
	    find_config(set, choice.config_name) ? choice.config_name
						 : set->configs[0].name);
	if (editor_read_line(fd, "Debug configuration: ", name, sizeof(name))
	    != MINIBUF_ACCEPTED) {
		return false;
	}
	for (i = 0; i < set->count; i++) {
		if (strcmp(set->configs[i].name, name) == 0) {
			snprintf(choice.config_name, sizeof(choice.config_name),
			    "%s", name);
			return true;
		}
	}
	editor_set_status_message(DAP_WHO ": no configuration named %s", name);
	return false;
}

/* lldb-dap has no program until a user names one, and
 * `${workspaceRoot}/a.out` would be a debugger that silently debugs the
 * wrong thing (src/dap_config.h). */
static bool ask_program(int fd, const struct dap_launch_config *cfg)
{
	choice.have_program = false;
	if (!cfg->needs_program) {
		return true;
	}
	choice.program[0] = '\0';
	if (editor_read_line_path(fd, "Program to debug: ", choice.program,
		sizeof(choice.program))
		!= MINIBUF_ACCEPTED
	    || choice.program[0] == '\0') {
		editor_set_status_message(DAP_WHO ": cancelled");
		return false;
	}
	choice.have_program = true;
	return true;
}

void editor_dap_debug(int fd)
{
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	char message[DAP_CONFIG_MESSAGE_MAX];
	struct dap_config_set *set;
	const struct dap_launch_config *cfg;

	if (dap_session_current()) {
		editor_set_status_message(
		    DAP_WHO ": a debug session is already running");
		return;
	}
	if (build_running) {
		editor_set_status_message(DAP_WHO ": the build is still going");
		return;
	}
	(void)current_file(choice.filename, sizeof(choice.filename));
	set = dap_config_load(choice.filename, &err);
	if (!set) {
		dap_config_error_format(&err, message, sizeof(message));
		editor_set_status_message(DAP_WHO ": %s", message);
		return;
	}
	if (set->count > 0 && choose_config(fd, set)) {
		cfg = find_config(set, choice.config_name);
		choice.valid = cfg && ask_program(fd, cfg);
	}
	dap_config_free(set);
	if (choice.valid) {
		dap_launch(true);
	}
}

/* ------------------------------- execution ---------------------------- */

void editor_dap_continue(int fd)
{
	(void)fd;
	(void)dap_exec_resume(DAP_EXEC_CONTINUE);
}

void editor_dap_next(int fd)
{
	(void)fd;
	(void)dap_exec_resume(DAP_EXEC_NEXT);
}

void editor_dap_step_in(int fd)
{
	(void)fd;
	(void)dap_exec_resume(DAP_EXEC_STEP_IN);
}

void editor_dap_step_out(int fd)
{
	(void)fd;
	(void)dap_exec_resume(DAP_EXEC_STEP_OUT);
}

void editor_dap_step_instruction(int fd)
{
	(void)fd;
	(void)dap_exec_resume(DAP_EXEC_STEP_INSTRUCTION);
}

void editor_dap_pause(int fd)
{
	(void)fd;
	(void)dap_exec_pause();
}

void editor_dap_restart(int fd)
{
	(void)fd;
	(void)dap_exec_restart();
}

void editor_dap_frame_up(int fd)
{
	(void)fd;
	(void)dap_exec_frame_select(1);
}

void editor_dap_frame_down(int fd)
{
	(void)fd;
	(void)dap_exec_frame_select(-1);
}

void editor_dap_disconnect(int fd)
{
	(void)fd;
	if (!dap_session_current()) {
		editor_set_status_message("No debug session");
		return;
	}
	/* Detaching is the point: the debuggee keeps running [M-8]. */
	dap_session_end(dap_session_current(), DAP_END_DISCONNECT);
}

void editor_dap_terminate(int fd)
{
	(void)fd;
	if (!dap_session_current()) {
		editor_set_status_message("No debug session");
		return;
	}
	dap_session_end(dap_session_current(), DAP_END_TERMINATE);
}

/* ------------------------------ breakpoints --------------------------- */

static void breakpoint_report(
    enum dap_breakpoint_result result, int line, const char *what)
{
	if (result == DAP_BREAKPOINT_OK) {
		dap_decor_refresh();
		editor_set_status_message("%s on line %d", what, line);
	} else {
		editor_set_status_message(
		    "%s", dap_breakpoint_result_text(result));
	}
}

void editor_dap_breakpoint_toggle(int fd)
{
	int row = current_row();
	bool added = false;
	enum dap_breakpoint_result result;

	(void)fd;
	result = dap_breakpoint_toggle(bcur(), row, &added);
	breakpoint_report(
	    result, row + 1, added ? "Breakpoint" : "Breakpoint removed");
}

void editor_dap_breakpoint_temporary(int fd)
{
	struct dap_breakpoint_options opts = { .temporary = true };
	int row = current_row();

	(void)fd;
	breakpoint_report(dap_breakpoint_add(bcur(), row, &opts), row + 1,
	    "Temporary breakpoint");
}

/* `dap-until`: the stage 5 temporary-breakpoint protocol, whose whole point
 * is that the `continue` happens from the ARMED callback -- after that
 * source's newest `setBreakpoints` answered, and only if it verified.  A
 * breakpoint that did not verify has already been removed and resynced by
 * the time this runs, so there is nothing to undo and nothing to continue
 * to. */
static void on_armed(void *ctx, bool verified, const char *text)
{
	(void)ctx;
	if (!verified) {
		editor_set_status_message("dap-until: %s", text);
		return;
	}
	dap_decor_refresh();
	(void)dap_exec_resume(DAP_EXEC_CONTINUE);
}

void editor_dap_until(int fd)
{
	char path[PATH_MAX];
	int row = current_row();
	enum dap_breakpoint_result result;
	int thread_id;

	(void)fd;
	if (!current_file(path, sizeof(path))) {
		editor_set_status_message(
		    "%s", dap_breakpoint_result_text(DAP_BREAKPOINT_NO_FILE));
		return;
	}
	if (!dap_exec_stopped_thread(&thread_id)) {
		editor_set_status_message("%s", dap_exec_no_thread_text());
		return;
	}
	result = dap_breakpoint_arm_temporary(path, row + 1, on_armed, NULL);
	if (result != DAP_BREAKPOINT_OK) {
		editor_set_status_message(
		    "%s", dap_breakpoint_result_text(result));
	}
}

/* `dap-goto`.  With one target the jump happens on the first invocation;
 * with several the targets are retained and a second invocation asks which
 * -- prompting from inside the `gotoTargets` callback is not allowed, since
 * that callback runs from the idle poll and may already be underneath a
 * minibuffer prompt (src/dap_exec.h). */
static void goto_choose(int fd)
{
	char answer[32] = "1";
	size_t count = dap_exec_goto_target_count();
	char prompt[DAP_EXEC_TEXT_MAX];
	long index;

	snprintf(prompt, sizeof(prompt), "Jump target 1-%zu (%s ...): ", count,
	    dap_exec_goto_target_name(0));
	if (editor_read_line(fd, prompt, answer, sizeof(answer))
	    != MINIBUF_ACCEPTED) {
		return;
	}
	index = strtol(answer, NULL, 10);
	if (index < 1 || (size_t)index > count
	    || !dap_exec_goto_chosen((size_t)index - 1)) {
		editor_set_status_message("dap-goto: no such target");
	}
}

void editor_dap_goto(int fd)
{
	char path[PATH_MAX];

	if (dap_exec_goto_target_count() > 1) {
		goto_choose(fd);
		return;
	}
	if (!current_file(path, sizeof(path))) {
		editor_set_status_message(
		    "dap-goto: this buffer visits no file");
		return;
	}
	(void)dap_exec_goto(path, current_row() + 1);
}

/* ---------------------------- evaluate and REPL ----------------------- */

void editor_dap_evaluate(int fd)
{
	char expression[256] = "";

	if (editor_read_line(fd, "Evaluate: ", expression, sizeof(expression))
		!= MINIBUF_ACCEPTED
	    || expression[0] == '\0') {
		return;
	}
	/* `hover`, not `repl`: on lldb-dap the repl context answers
	 * "(int) $0 = 21551" and leaves a `$0` behind, where hover answers
	 * the bare value an echo-area line wants [M-14]. */
	(void)dap_exec_evaluate(expression, "hover");
}

void editor_dap_repl(int fd)
{
	int slot = pane_slot("*dap-repl*");

	(void)fd;
	if (slot < 0) {
		editor_set_status_message("dap-repl: no room for the buffer");
		return;
	}
	win_display_buffer_other_window(slot);
}

/* ------------------------------- lifecycle ---------------------------- */

void dap_commands_init(void)
{
	struct dap_exec_hooks hooks = {
		.location = on_location,
		.report = on_session_report,
		.changed = on_exec_changed,
		.relaunch = on_relaunch,
	};

	dap_exec_set_hooks(&hooks);
	dap_breakpoint_set_report_hook(on_session_report, NULL);
	dap_decor_init();
}

void dap_commands_shutdown(void)
{
	if (build_running) {
		compilation_cancel_programmatic(build_generation);
		build_running = false;
	}
	dap_exec_set_hooks(NULL);
	dap_breakpoint_set_report_hook(NULL, NULL);
	dap_decor_reset();
	memset(&choice, 0, sizeof(choice));
	memset(&layout, 0, sizeof(layout));
	memset(&transcript, 0, sizeof(transcript));
}

#else /* !KG_USE_DAP */

#include "def.h"

/* One sentence, and the same one every time.  A row that vanished with the
 * build would leave `M-x dap-continue` reporting an unknown command, which
 * tells a user nothing about why. */
static void dap_absent(void)
{
	editor_set_status_message("kg was built without DAP support");
}

void editor_dap_debug(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_disconnect(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_terminate(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_continue(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_pause(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_next(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_step_in(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_step_instruction(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_step_out(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_breakpoint_toggle(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_breakpoint_temporary(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_until(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_goto(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_restart(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_evaluate(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_repl(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_frame_up(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_frame_down(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_many_windows(int fd)
{
	(void)fd;
	dap_absent();
}

void dap_commands_init(void) { }

void dap_commands_shutdown(void) { }

#endif /* KG_USE_DAP */
