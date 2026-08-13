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
#include "dap_config.h"
#include "dap_decor.h"
#include "dap_exec.h"
#include "dap_java.h"
#include "dap_session.h"
#include "dap_ui.h"
#include "def.h"
#include "json.h"
#include "localvars.h"
#include "visit.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dap_session; /* src/dap_session.h */

#define DAP_WHO "dap-debug"

/* What the user last chose, so `dap-debug` can be repeated and
 * `dap-restart` can relaunch the same thing without asking again.
 *
 * `dap-debug` builds a CANDIDATE of this and commits it only once every
 * question has been answered.  Writing the parts as they arrive is how a
 * C-g at the chooser used to launch the previous configuration: the
 * filename had already been overwritten, `valid` was still last time's, and
 * nothing between the cancel and the launch said otherwise. */
struct dap_choice {
	char filename[PATH_MAX];
	char config_name[DAP_CONFIG_NAME_MAX];
	char program[PATH_MAX];
	bool have_program;
	bool valid;
};

static struct dap_choice choice;

/* The REPL's own history, so that walking it does not walk somebody else's
 * shell commands, and `dap-evaluate`'s, which is a different question asked
 * in a different context [M-14]. */
static struct minibuf_history repl_history;
static struct minibuf_history evaluate_history;

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
	/* buf_visits_file() rather than `no_file` alone (src/def.h says so):
	 * kg's own *special* buffers -- `*scratch*`, a debugger pane -- carry
	 * a NAME in `filename` and no file behind it, and a debugger asking
	 * an adapter to run `*scratch*` is the failure this predicate is
	 * here to prevent. */
	if (!buf_visits_file(bcur()) || !bcur()->filename
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

/* Everything about a pane -- the six buffers, the row metadata, the two
 * transcripts, the layout and the info map's commands -- is src/dap_ui.c's.
 * What is left here is the command layer: the rows cmd.c reaches, the
 * prompts (this is the one debugger file that prompts) and the hooks that
 * turn the session's and the stop model's news into dap_ui_changed(). */

static void on_output(
    void *ctx, const char *category, const char *text, size_t len)
{
	(void)ctx;
	dap_ui_output(category, text, len);
}

/* The launch was refused and the adapter had already explained why.  For an
 * adapter that builds what it debugs that explanation is compiler
 * diagnostics, so it goes where every other compiler's does -- *compilation*
 * and the next-error store -- rather than only into the debugger's own
 * transcript, where C-x ` cannot reach it.
 *
 * A compilation the user started owns that buffer: if one is running the
 * feed is refused, the output stays in *dap-output*, and the echo area says
 * so rather than silently losing the diagnostics. */
static void on_build_failed(void *ctx, const char *label, const char *directory,
    const char *bytes, size_t len, bool truncated)
{
	char where[PATH_MAX];
	char note[160];

	(void)ctx;
	if (directory && directory[0]) {
		snprintf(where, sizeof(where), "%s", directory);
	} else if (compilation_resolve_directory(NULL, where, sizeof(where))
	    != 0) {
		return;
	}
	if (!compilation_feed_begin(label, where)) {
		editor_set_status_message(DAP_WHO
		    ": a compilation is running; the launch output is in "
		    "*dap-output*");
		return;
	}
	compilation_feed_bytes(bytes, len);
	snprintf(note, sizeof(note), "Launch failed: %s%s", label,
	    truncated ? " (output truncated)" : "");
	compilation_feed_finish(note);
}

void editor_dap_many_windows(int fd)
{
	(void)fd;
	dap_ui_layout_toggle();
}

/* ------------------------------ session news -------------------------- */

/* THE ONE NOTIFICATION POINT, from this side: everything that can change
 * what a pane or a mark says arrives here and calls exactly these two. */
static void ui_changed(void)
{
	dap_decor_refresh();
	dap_ui_changed();
}

static void on_session_changed(void *ctx, struct dap_session *session)
{
	(void)ctx;
	(void)session;
	ui_changed();
}

/* The breakpoint table changed shape.  It reports its own sentences but
 * repaints nothing, and one of its changes -- a source released, which
 * compacts every index after it -- is not otherwise visible to anybody:
 * without this the breakpoint pane could keep showing rows built before
 * the compaction (src/dap_breakpoint.h). */
static void on_breakpoints_changed(void *ctx)
{
	(void)ctx;
	ui_changed();
}

static void on_session_report(void *ctx, bool error, const char *text)
{
	(void)ctx;
	report(error, text);
}

/* Where the program stopped.  `focus` is false when the stop carried
 * `preserveFocusHint`: the panes and the decoration move, the user's window
 * does not -- and neither does it when the user is reading a `*dap-*` pane,
 * because taking the window they are in to show them a source line is the
 * one thing a pane is there to prevent.  The column is a 1-BASED BYTE
 * column (doc/coordinates.md) and a DAP column is not one, so kg passes 1.
 */
static void on_location(void *ctx, const char *path, int line, bool focus)
{
	struct kg_buffer_handle none = { 0 };

	(void)ctx;
	if (focus && !dap_ui_current_buffer_is_pane()) {
		(void)editor_visit_file_position("dap", path, line, 1, none);
	}
	ui_changed();
}

static void on_exec_changed(void *ctx)
{
	(void)ctx;
	if (!dap_session_current()) {
		dap_ui_layout_session_ended();
	}
	ui_changed();
}

/* An `evaluate` answer.  The REPL's goes into its transcript, tagged with
 * what was asked; everything else is one line of echo area.  A reply whose
 * epoch or selection has moved on is dropped without a word: the model has
 * already refused it once, and a transcript is append-only, so a stale line
 * in it is a line a user cannot tell from a fresh one. */
static void on_evaluated(void *ctx, const struct dap_exec_answer *answer)
{
	(void)ctx;
	if (answer->epoch != dap_exec_epoch()
	    || answer->selection != dap_exec_selection()) {
		return;
	}
	if (strcmp(answer->context, "repl") == 0) {
		dap_ui_repl_answer(
		    answer->expression, answer->error, answer->text);
		return;
	}
	editor_set_status_message("%s", answer->text);
}

static void on_relaunch(void *ctx)
{
	(void)ctx;
	/* The configuration, the breakpoint table and the layout are all
	 * still here; only the adapter is new.  The build step is not re-run
	 * -- a restart restarts the program, and rebuilding it is what
	 * `dap-debug` is for.
	 *
	 * The layout is NOT put back up from here: both paths that produce a
	 * session already do it themselves -- start_session() for an ordinary
	 * adapter, and dap_java_set_session_hook() for the one whose session
	 * exists only once a language server has answered -- so a call here
	 * would only ever fire on the path that produced NO session, which is
	 * six panes around a debugger that is not running. */
	dap_launch(false);
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
		/* The Java adapter's events go through its own layer FIRST,
		 * which forwards every one of them to the model unchanged and
		 * then forms nbcode's opinion about them (src/dap_java.h).
		 * Nothing else installs anything but the model itself. */
		.event = dap_java_owns(spec) ? dap_java_event : dap_exec_event,
		.sources = dap_breakpoint_session_sources,
		.breakpoints_answered = dap_breakpoint_session_answered,
		.build_failed = on_build_failed,
	};
	struct dap_session_request request
	    = { spec, cfg->request, arguments, len, cfg->name, &hooks };
	char error[DAP_CONFIG_MESSAGE_MAX] = "";

	/* A Java session does not exist yet when this returns: its adapter
	 * is an endpoint the language server has to be asked for, which
	 * takes as long as starting a JVM.  What starts here is the WAIT,
	 * and the panes are opened once there is a session to show. */
	if (dap_java_owns(spec)) {
		if (dap_java_start(
			&request, choice.filename, error, sizeof(error))
		    != 0) {
			editor_set_status_message(DAP_WHO ": %s", error);
			return false;
		}
		ui_changed();
		return true;
	}
	if (!dap_session_start(&request, error, sizeof(error))) {
		editor_set_status_message(DAP_WHO ": %s", error);
		return false;
	}
	dap_exec_session_started();
	dap_ui_layout_session_started();
	ui_changed();
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

/* The build step's own substitutions.  They are stored unexpanded, like
 * every other string in a configuration, and expanding them is this
 * caller's job: an unexpanded `${workspaceRoot}` is a compiler run in a
 * directory named after the placeholder, which fails in a way that reads
 * like a broken toolchain rather than like a configuration kg did not
 * finish reading. */
static bool start_build(
    const struct dap_config_set *set, const struct dap_launch_config *cfg)
{
	struct dap_config_context_store store;
	struct dap_config_context ctx;
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	char message[DAP_CONFIG_MESSAGE_MAX];
	char command[KG_COMPILE_COMMAND_MAX];
	char directory[PATH_MAX];

	dap_config_context_for(choice.filename, set, &store, &ctx);
	if (dap_config_expand_string(
		cfg->build_command, &ctx, command, sizeof(command), &err)
		!= 0
	    || dap_config_expand_string(
		   cfg->build_cwd, &ctx, directory, sizeof(directory), &err)
		!= 0) {
		dap_config_error_format(&err, message, sizeof(message));
		editor_set_status_message(DAP_WHO ": %s", message);
		return false;
	}
	if (compilation_start_programmatic(command, directory,
		buf_handle(buf_current), on_build_done, NULL, &build_generation)
	    != COMPILATION_ACCEPTED) {
		editor_set_status_message(
		    DAP_WHO ": a compilation is already running");
		return false;
	}
	build_running = true;
	editor_set_status_message("Building: %s", command);
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
		(void)start_build(set, cfg);
	} else {
		launch_resolved(set, cfg);
	}
	dap_config_free(set);
}

/* ------------------------------- dap-debug ---------------------------- */

/* The chooser.  One configuration runs without a question; several ask, and
 * the last successful choice is the default, which is what makes the
 * command repeatable. */
static bool choose_config(
    int fd, const struct dap_config_set *set, struct dap_choice *pick)
{
	char name[DAP_CONFIG_NAME_MAX];
	size_t i;

	if (set->count == 1) {
		snprintf(pick->config_name, sizeof(pick->config_name), "%s",
		    set->configs[0].name);
		return true;
	}
	/* The DEFAULT comes from the last committed choice -- that is what
	 * makes the command repeatable -- and the answer goes to the
	 * candidate, which is what makes a refusal cost nothing. */
	snprintf(name, sizeof(name), "%s",
	    find_config(set, choice.config_name) ? choice.config_name
						 : set->configs[0].name);
	if (editor_read_line(fd, "Debug configuration: ", name, sizeof(name))
	    != MINIBUF_ACCEPTED) {
		return false;
	}
	for (i = 0; i < set->count; i++) {
		if (strcmp(set->configs[i].name, name) == 0) {
			snprintf(pick->config_name, sizeof(pick->config_name),
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
static bool ask_program(
    int fd, const struct dap_launch_config *cfg, struct dap_choice *pick)
{
	pick->have_program = false;
	if (!cfg->needs_program) {
		return true;
	}
	pick->program[0] = '\0';
	if (editor_read_line_path(
		fd, "Program to debug: ", pick->program, sizeof(pick->program))
		!= MINIBUF_ACCEPTED
	    || pick->program[0] == '\0') {
		editor_set_status_message(DAP_WHO ": cancelled");
		return false;
	}
	pick->have_program = true;
	return true;
}

void editor_dap_debug(int fd)
{
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	char message[DAP_CONFIG_MESSAGE_MAX];
	struct dap_choice pick = { 0 };
	struct dap_config_set *set;
	const struct dap_launch_config *cfg;
	bool chosen;

	if (dap_session_current()) {
		editor_set_status_message(
		    DAP_WHO ": a debug session is already running");
		return;
	}
	if (build_running) {
		editor_set_status_message(DAP_WHO ": the build is still going");
		return;
	}
	/* Asked FIRST and refused here: `${file}` and the walk for
	 * `.kg-dap.json` both start at the buffer's own file, and a buffer
	 * that visits none used to leave the previous project's filename in
	 * place -- which is a debugger silently debugging the last thing. */
	if (!current_file(pick.filename, sizeof(pick.filename))) {
		editor_set_status_message(
		    DAP_WHO ": this buffer visits no file");
		return;
	}
	set = dap_config_load(pick.filename, &err);
	if (!set) {
		dap_config_error_format(&err, message, sizeof(message));
		editor_set_status_message(DAP_WHO ": %s", message);
		return;
	}
	if (set->count == 0) {
		dap_config_free(set);
		editor_set_status_message(
		    DAP_WHO ": no configurations to choose from");
		return;
	}
	chosen = choose_config(fd, set, &pick);
	if (chosen) {
		cfg = find_config(set, pick.config_name);
		chosen = cfg && ask_program(fd, cfg, &pick);
	}
	dap_config_free(set);
	/* Every question answered, and only now: a cancelled chooser, a name
	 * nothing answers to and a cancelled program prompt all leave the
	 * previous choice exactly as it was -- which is what `dap-restart`
	 * relaunches -- and launch nothing. */
	if (!chosen) {
		return;
	}
	pick.valid = true;
	choice = pick;
	dap_launch(true);
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
	/* A Java session that has not connected yet is a WAIT rather than a
	 * session, and this is what ends one: the language server it is
	 * waiting for may be importing a large project, and a user has to be
	 * able to stop waiting for it (src/dap_java.h). */
	if (dap_java_step() == DAP_JAVA_RESOLVING) {
		dap_java_cancel();
		return;
	}
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
		ui_changed();
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
	ui_changed();
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
	char expression[DAP_EXEC_TEXT_MAX] = "";

	if (editor_read_line_with_history(fd, "Evaluate: ", expression,
		sizeof(expression), &evaluate_history)
		!= MINIBUF_ACCEPTED
	    || expression[0] == '\0') {
		return;
	}
	/* `hover`, not `repl`: on lldb-dap the repl context answers
	 * "(int) $0 = 21551" and leaves a `$0` behind, where hover answers
	 * the bare value an echo-area line wants [M-14]. */
	(void)dap_exec_evaluate(expression, "hover");
}

/* The REPL, whose input is the MINIBUFFER.  A prompt line inside the buffer
 * would need an editable-tail mode kg does not have -- cursor confinement,
 * an undo policy, insertion arriving above the prompt while it is being
 * typed -- so `*dap-repl*` is a read-only transcript and this is where the
 * typing happens.  Nothing selects a window or touches a source buffer
 * while the prompt is open; the answer arrives later, through
 * on_evaluated(). */
void editor_dap_repl(int fd)
{
	char expression[DAP_EXEC_TEXT_MAX] = "";

	if (!dap_ui_show(DAP_UI_PANE_REPL)) {
		editor_set_status_message("dap-repl: no room for the buffer");
		return;
	}
	if (editor_read_line_with_history(
		fd, "DAP> ", expression, sizeof(expression), &repl_history)
		!= MINIBUF_ACCEPTED
	    || expression[0] == '\0') {
		return;
	}
	dap_ui_repl_echo(expression);
	/* `repl` context here and nowhere else [M-14]: this is the one place
	 * an adapter's own REPL formatting, side effects included, is what
	 * the user asked for. */
	(void)dap_exec_evaluate(expression, "repl");
}

/* ------------------------- the debugger's panes ----------------------- */

/* The `dap-info` map's commands.  One map for all six panes and one
 * dispatch on what the LINE is (src/dap_ui.h), rather than one map per
 * pane: the panes differ in what they list, not in what RET means. */
void editor_dap_info_select(int fd)
{
	(void)fd;
	dap_ui_info_ret();
}

void editor_dap_info_delete_breakpoint(int fd)
{
	(void)fd;
	dap_ui_info_delete();
}

void editor_dap_info_toggle_breakpoint(int fd)
{
	(void)fd;
	dap_ui_info_toggle_enable();
}

void editor_dap_info_toggle_breakpoints_threads(int fd)
{
	(void)fd;
	dap_ui_info_toggle_breakpoints_threads();
}

/* ------------------------------- lifecycle ---------------------------- */

void dap_commands_init(void)
{
	struct dap_exec_hooks hooks = {
		.location = on_location,
		.report = on_session_report,
		.changed = on_exec_changed,
		.relaunch = on_relaunch,
		.evaluated = on_evaluated,
	};

	dap_exec_set_hooks(&hooks);
	dap_breakpoint_set_report_hook(on_session_report, NULL);
	dap_breakpoint_set_changed_hook(on_breakpoints_changed, NULL);
	dap_java_set_session_hook(dap_ui_layout_session_started);
	dap_decor_init();
	dap_ui_init();
	dap_set_ui_poll(dap_ui_poll);
}

void dap_commands_shutdown(void)
{
	if (build_running) {
		compilation_cancel_programmatic(build_generation);
		build_running = false;
	}
	dap_exec_set_hooks(NULL);
	dap_breakpoint_set_report_hook(NULL, NULL);
	dap_breakpoint_set_changed_hook(NULL, NULL);
	dap_java_set_session_hook(NULL);
	dap_java_cancel();
	dap_decor_reset();
	dap_set_ui_poll(NULL);
	dap_ui_reset();
	memset(&choice, 0, sizeof(choice));
	minibuf_history_init(&repl_history);
	minibuf_history_init(&evaluate_history);
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

void editor_dap_info_select(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_info_delete_breakpoint(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_info_toggle_breakpoint(int fd)
{
	(void)fd;
	dap_absent();
}

void editor_dap_info_toggle_breakpoints_threads(int fd)
{
	(void)fd;
	dap_absent();
}

void dap_commands_init(void) { }

void dap_commands_shutdown(void) { }

#endif /* KG_USE_DAP */
