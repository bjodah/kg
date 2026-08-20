/* test_cmd.c — invariants of the one command table.
 *
 * cmdtable is the single statement of what a command is allowed to do
 * in the authoritative registry: whether it edits the buffer, whether Lisp
 * may ask
 * for it, and what it is called in one line.  Nothing else in the tree
 * enforces those properties, so they are asserted here.
 *
 * This binary links every editor translation unit except main.c, the way
 * test_perf does, because reaching the table means linking cmd.o and
 * cmd.o reaches most of the editor.  It calls no handler. */

#include "../src/cmd.h"
#include "../src/cmdstate.h"
#include "../src/def.h"
#include "../src/keyevent.h"
#include "../src/lisp.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

/* THE KEPT-OFF SET.  Phase 17 audited all 153 rows and flipped every
 * command whose handler is safe under (command-execute ...), which
 * inverted what is worth pinning: the Lisp-callable set is now the
 * default and the exceptions are the decision.  These are the
 * exceptions, grouped by the reason cmdtable's own header states, and
 * cmd_invoke() must refuse exactly these and nothing else -- the point
 * of this test is unchanged, that the set is something somebody wrote
 * down rather than a flag that drifted.
 *
 * 1. Re-enters the evaluator, which a Lisp caller is already inside.
 * 2. A modal loop that owns the keyboard until it ends.  Macro replay
 *    additionally re-enters editor_process_keypress(), i.e. arbitrary
 *    command dispatch, with no depth accounting of its own.
 * 3. Its argument IS a keystroke, so a Lisp call has nothing to supply.
 * 4. Ends or suspends the editor from inside an evaluation.
 * 5. The interactive command dispatcher itself; (command-execute ...)
 *    is the same thing without the picker. */
static const char *const not_lisp_callable[] = {
	/* 1 */
	"eval-buffer",
	"eval-expression",
	"eval-last-sexp",
	"eval-print-last-sexp",
	"newline-or-eval-print-last-sexp",
	/* 2 */
	"isearch-backward",
	"isearch-backward-regexp",
	"isearch-forward",
	"isearch-forward-regexp",
	"kmacro-end-and-call-macro",
	"kmacro-end-macro",
	"kmacro-end-or-call-macro",
	"kmacro-start-macro",
	"query-replace",
	"query-replace-regexp",
	/* 3 */
	"copy-to-register",
	"describe-key",
	"insert-register",
	"jump-to-register",
	"point-to-register",
	"quoted-insert",
	"self-insert-command",
	"zap-to-char",
	/* 4 */
	"git-commit-abort",
	"git-rebase-abort",
	"save-buffers-kill-terminal",
	"server-edit",
	"suspend-editor",
	/* 5 */
	"execute-extended-command",
};

/* A command that can read the terminal is Lisp-callable but refused when
 * the activation has no descriptor to prompt on (cmd.h's
 * CMD_READS_TERMINAL, cmd_invoke()'s CMD_NO_TERMINAL).  Every shape of
 * terminal read is represented here: a minibuffer prompt, a path prompt,
 * a y/n confirmation, a raw key, and the picker. */
static const char *const reads_terminal[] = {
	"goto-line",
	"find-file",
	"revert-buffer",
	"quoted-insert",
	"switch-to-buffer",
	/* The debugger's four: the configuration chooser and the program
	 * path, the expression, the jump target when `gotoTargets` answered
	 * with several, and -- since subplan 02 -- the REPL, whose input is
	 * the minibuffer because `*dap-repl*` is a read-only transcript.
	 * The other nineteen dap-* rows read nothing, which is what makes
	 * them safe from a hook -- test/test_dap_commands.c pins all
	 * twenty-three from the other side. */
	"dap-debug",
	"dap-evaluate",
	"dap-goto",
	"dap-repl",
};

/* And the other side of the same question: commands that do NOT read the
 * terminal, so a hook or an init file may run them.  A row that silently
 * gained a prompt would be an editor that exits on a bad file
 * descriptor, which is why this list is here at all. */
static const char *const reads_nothing[] = {
	"forward-word",
	"upcase-word",
	"undo",
	"other-window",
	"list-buffers",
	"recenter-top-bottom",
};

static int table_size(void)
{
	int n = 0;

	while (cmd_descriptor_at(n)) {
		n++;
	}
	return n;
}

/* The M-x picker's two-pass ranking keeps the table's own order within
 * each rank, so the table has to be the sorted one. */
static void test_names_sorted_and_unique(void)
{
	int i, n = table_size();

	CHECK(n > 0);
	for (i = 1; i < n; i++) {
		const char *prev = cmd_descriptor_at(i - 1)->name;
		const char *cur = cmd_descriptor_at(i)->name;

		CHECKF(
		    strcmp(prev, cur) < 0, "%s must sort before %s", prev, cur);
	}
}

static void test_every_entry_has_a_handler_and_summary(void)
{
	int i, n = table_size();

	for (i = 0; i < n; i++) {
		const struct named_cmd *cmd = cmd_descriptor_at(i);

		CHECKF(cmd->fn != NULL, "%s has no handler", cmd->name);
		CHECKF(cmd->summary != NULL && cmd->summary[0] != '\0',
		    "%s has no summary", cmd->name);
		CHECKF(strlen(cmd->summary) <= 60, "%s: summary is %zu columns",
		    cmd->name, strlen(cmd->summary));
		/* A summary is a phrase, not a sentence: the generated
		 * help columns have no room for the period. */
		CHECKF(cmd->summary[strlen(cmd->summary) - 1] != '.',
		    "%s: summary ends in a period", cmd->name);
		CHECKF((cmd->flags
			   & ~(unsigned)(CMD_EDITS_BUFFER | CMD_LISP_CALLABLE
			       | CMD_REPEATS | CMD_KEEPS_GOAL_COLUMN
			       | CMD_READS_TERMINAL))
			== 0,
		    "%s has an unknown flag", cmd->name);
	}
}

static void test_lisp_callable_set_is_the_audited_one(void)
{
	size_t k,
	    expected = sizeof(not_lisp_callable) / sizeof(*not_lisp_callable);
	int i, n = table_size(), refused = 0;

	for (k = 0; k < expected; k++) {
		const struct named_cmd *cmd = cmd_lookup(not_lisp_callable[k]);

		CHECKF(cmd != NULL, "%s is gone from the table",
		    not_lisp_callable[k]);
		if (cmd) {
			CHECKF(!(cmd->flags & CMD_LISP_CALLABLE),
			    "%s gained CMD_LISP_CALLABLE without a reason",
			    cmd->name);
		}
	}
	for (i = 0; i < n; i++) {
		if (!(cmd_descriptor_at(i)->flags & CMD_LISP_CALLABLE)) {
			refused++;
		}
	}
	CHECKF(refused == (int)expected,
	    "%d commands are refused from Lisp, the audit named %zu", refused,
	    expected);
}

/* CMD_READS_TERMINAL is what makes a prompting command safe from Lisp
 * rather than fatal: cmd_invoke() refuses it when the activation has no
 * prompt descriptor, and read(-1, ...) would otherwise clear `running'.
 * A command that grows a prompt without the flag is the bug this
 * catches, so both sides of the classification are pinned. */
static void test_terminal_reading_classification(void)
{
	size_t i;

	for (i = 0; i < sizeof(reads_terminal) / sizeof(*reads_terminal); i++) {
		const struct named_cmd *cmd = cmd_lookup(reads_terminal[i]);

		CHECKF(cmd != NULL, "%s is gone from the table",
		    reads_terminal[i]);
		if (cmd) {
			CHECKF(cmd->flags & CMD_READS_TERMINAL,
			    "%s reads the terminal but does not say so",
			    cmd->name);
		}
	}
	for (i = 0; i < sizeof(reads_nothing) / sizeof(*reads_nothing); i++) {
		const struct named_cmd *cmd = cmd_lookup(reads_nothing[i]);

		CHECKF(
		    cmd != NULL, "%s is gone from the table", reads_nothing[i]);
		if (cmd) {
			CHECKF(!(cmd->flags & CMD_READS_TERMINAL),
			    "%s claims to read the terminal", cmd->name);
		}
	}
}

/* The old allow-list carried its own `mutates` bit.  Every entry agreed
 * with CMD_EDITS_BUFFER, which is why deleting it was safe; this pins
 * the agreement so a future flag change cannot quietly widen what Lisp
 * may do to a read-only buffer. */
static void test_lisp_callable_mutation_verdicts(void)
{
	static const char *const mutating[]
	    = { "capitalize-word", "delete-horizontal-space",
		      "delete-trailing-space", "downcase-word", "join-line",
		      "just-one-space", "transpose-chars", "upcase-word" };
	static const char *const not_mutating[]
	    = { "electric-pair-mode", "lisp-arena-stats", "show-paren-mode",
		      "version", "what-cursor-position" };
	size_t i;

	for (i = 0; i < sizeof(mutating) / sizeof(*mutating); i++) {
		const struct named_cmd *cmd = cmd_lookup(mutating[i]);

		CHECKF(cmd && (cmd->flags & CMD_EDITS_BUFFER),
		    "%s must still be an editing command", mutating[i]);
	}
	for (i = 0; i < sizeof(not_mutating) / sizeof(*not_mutating); i++) {
		const struct named_cmd *cmd = cmd_lookup(not_mutating[i]);

		CHECKF(cmd && !(cmd->flags & CMD_EDITS_BUFFER),
		    "%s must not be an editing command", not_mutating[i]);
	}
}

static void test_lookup_edges(void)
{
	int n = table_size();

	CHECK(cmd_lookup(NULL) == NULL);
	CHECK(cmd_lookup("") == NULL);
	CHECK(cmd_lookup("no-such-command") == NULL);
	/* First and last entries, reached by name and by position. */
	CHECK(cmd_lookup(cmd_descriptor_at(0)->name) == cmd_descriptor_at(0));
	CHECK(cmd_lookup(cmd_descriptor_at(n - 1)->name)
	    == cmd_descriptor_at(n - 1));
	CHECK(cmd_descriptor_at(-1) == NULL);
	CHECK(cmd_descriptor_at(n) == NULL);
}

/* ---- Command identity ----
 *
 * "version" is the command these tests invoke: it is Lisp-callable, does
 * not edit, and its whole effect is an echo-area message, so running it
 * for real costs nothing.  "upcase-word" is the one they get refused. */

static int run(const char *name, enum command_origin origin)
{
	struct command_context ctx
	    = { 0, { 0, 0, 0, 0 }, origin }; /* fd 0, no prefix argument */

	return cmd_invoke(name, &ctx);
}

/* The same, with no descriptor to prompt on -- what an init file, a hook
 * or a process filter gives a command reached from Lisp. */
static int run_without_a_terminal(const char *name)
{
	struct command_context ctx = { -1, { 0, 0, 0, 0 }, CMD_ORIGIN_LISP };

	return cmd_invoke(name, &ctx);
}

/* One keystroke that runs `name`, so last_command ends up holding it. */
static void run_as_keystroke(const char *name)
{
	cmd_state_begin_keystroke();
	CHECK(run(name, CMD_ORIGIN_KEY) == CMD_RAN);
}

static void test_static_ids_are_table_slots(void)
{
	int i, n = table_size();

	CHECK(cmd_id_by_name(NULL) == CMD_ID_NONE);
	CHECK(cmd_id_by_name("no-such-command") == CMD_ID_NONE);
	for (i = 0; i < n; i++) {
		const char *name = cmd_descriptor_at(i)->name;
		command_id id = cmd_id_by_name(name);

		CHECKF(id == CMD_ID_STATIC_BASE + (command_id)i,
		    "%s: id %u is not its table slot", name, (unsigned)id);
		CHECKF(id < CMD_ID_RUNTIME_BASE,
		    "the table has outgrown the runtime id base");
	}
	/* Two names for one handler are two commands, which is why a
	 * handler pointer cannot be the identity. */
	CHECK(cmd_lookup("read-only-mode")->fn
	    == cmd_lookup("toggle-read-only")->fn);
	CHECK(cmd_id_by_name("read-only-mode")
	    != cmd_id_by_name("toggle-read-only"));
}

static void test_identity_is_published_for_every_origin(void)
{
	command_id version = cmd_id_by_name("version");
	enum command_origin origins[]
	    = { CMD_ORIGIN_KEY, CMD_ORIGIN_MX, CMD_ORIGIN_LISP };
	size_t i;

	for (i = 0; i < sizeof(origins) / sizeof(origins[0]); i++) {
		cmd_state_begin_keystroke();
		CHECK(run("version", origins[i]) == CMD_RAN);
		CHECKF(cmd_state()->this_command == version,
		    "origin %d did not publish the command's identity",
		    (int)origins[i]);
		cmd_state_begin_keystroke();
		CHECK(cmd_state()->last_command == version);
		CHECK(cmd_state()->this_command == CMD_ID_NONE);
	}
	CHECK(cmd_state()->invocation_depth == 0);
}

/* A keystroke that runs no command still ends the previous command's
 * turn: last_command becomes "nothing", which is what makes a repeat a
 * first invocation again. */
static void test_a_keystroke_without_a_command_ends_the_history(void)
{
	run_as_keystroke("version");
	cmd_state_begin_keystroke(); /* a key that runs nothing */
	cmd_state_begin_keystroke();
	CHECK(cmd_state()->last_command == CMD_ID_NONE);
}

static void test_refused_commands_are_not_what_ran_last(void)
{
	command_id version = cmd_id_by_name("version");

	run_as_keystroke("version");
	cmd_state_begin_keystroke();

	/* Not callable from Lisp: one of Phase 17's audited exceptions. */
	CHECK(run("isearch-forward", CMD_ORIGIN_LISP) == CMD_NOT_CALLABLE);
	CHECK(cmd_state()->this_command == CMD_ID_NONE);
	/* Callable, but this activation has no descriptor to prompt on. */
	CHECK(run_without_a_terminal("goto-line") == CMD_NO_TERMINAL);
	CHECK(cmd_state()->this_command == CMD_ID_NONE);
	/* Refused by a read-only buffer. */
	bcur()->readonly = 1;
	CHECK(run("upcase-word", CMD_ORIGIN_KEY) == CMD_READ_ONLY);
	bcur()->readonly = 0;
	CHECK(cmd_state()->this_command == CMD_ID_NONE);
	/* No such command. */
	CHECK(run("no-such-command", CMD_ORIGIN_KEY) == CMD_UNKNOWN);
	CHECK(cmd_state()->this_command == CMD_ID_NONE);
	CHECK(cmd_state()->last_command == version);
	CHECK(cmd_state()->invocation_depth == 0);
}

/* A nested invocation may say what it is while it runs, and must give
 * the outer command's identity back when it finishes. */
static void test_nested_invocation_restores_the_outer_identity(void)
{
	command_id outer_id = cmd_id_by_name("version");
	command_id inner_id = cmd_id_by_name("sort-lines");
	command_id saved_outer, saved_inner;

	cmd_state_begin_keystroke();
	saved_outer = cmd_state_begin_command(outer_id);
	CHECK(cmd_state()->invocation_depth == 1);

	saved_inner = cmd_state_begin_command(inner_id);
	CHECK(cmd_state()->this_command == inner_id);
	CHECK(cmd_state()->invocation_depth == 2);
	cmd_state_end_command(saved_inner);
	CHECK(cmd_state()->this_command == outer_id);

	cmd_state_end_command(saved_outer);
	CHECK(cmd_state()->invocation_depth == 0);
	CHECK(cmd_state()->this_command == outer_id);
	cmd_state_begin_keystroke();
	CHECKF(cmd_state()->last_command == outer_id,
	    "the nested command rewrote the keystroke's history");
}

static void test_runtime_ids_are_stable_across_redefinition(void)
{
	command_id first = cmd_runtime_define("my-command");
	command_id again = cmd_runtime_define("my-command");
	command_id other = cmd_runtime_define("my-other-command");
	command_id recreated;

	CHECK(first >= CMD_ID_RUNTIME_BASE);
	CHECKF(again == first, "redefining changed the identity");
	CHECKF(other != first, "two commands share one identity");
	CHECK(cmd_id_by_name("my-command") == first);

	cmd_runtime_remove("my-command");
	CHECK(cmd_id_by_name("my-command") == CMD_ID_NONE);
	recreated = cmd_runtime_define("my-command");
	CHECKF(recreated != first,
	    "a removed and recreated command kept its identity");
	CHECKF(recreated != other, "the recreated command took another's id");

	cmd_runtime_remove("my-command");
	cmd_runtime_remove("my-other-command");
	cmd_runtime_remove("my-command"); /* removing twice is harmless */
	CHECK(cmd_runtime_define(NULL) == CMD_ID_NONE);
	CHECK(cmd_runtime_define("") == CMD_ID_NONE);
}

/* The table is bounded, and says so rather than handing out an identity
 * it cannot remember. */
static void test_runtime_id_table_is_bounded(void)
{
	char name[32];
	int i, defined = 0;

	for (i = 0; i < 64; i++) {
		snprintf(name, sizeof(name), "bounded-%d", i);
		if (cmd_runtime_define(name) != CMD_ID_NONE) {
			defined++;
		}
	}
	CHECKF(defined < 64, "the runtime id table is unbounded");
	CHECKF(defined > 0, "the runtime id table holds nothing");
	for (i = 0; i < 64; i++) {
		snprintf(name, sizeof(name), "bounded-%d", i);
		cmd_runtime_remove(name);
	}
	/* Freed slots are usable again. */
	CHECK(cmd_runtime_define("bounded-0") != CMD_ID_NONE);
	cmd_runtime_remove("bounded-0");
}

static void test_transient_state_survives_only_a_repeat(void)
{
	run_as_keystroke("version");
	cmd_set_transient_value(3);

	/* The same command next keystroke: its own state is there. */
	run_as_keystroke("version");
	CHECK(cmd_transient_value() == 3);

	/* Another command next keystroke: nothing to read, for either. */
	cmd_set_transient_value(5);
	run_as_keystroke("what-cursor-position");
	CHECK(cmd_transient_value() == 0);
	run_as_keystroke("version");
	CHECKF(
	    cmd_transient_value() == 0, "state survived a command in between");
}

static void test_transient_state_is_cleared_by_its_owners_removal(void)
{
	command_id id = cmd_runtime_define("transient-owner");
	command_id outer;

	cmd_state_begin_keystroke();
	outer = cmd_state_begin_command(id);
	cmd_set_transient_value(7);
	CHECK(cmd_state()->transient.owner == id);
	cmd_state_end_command(outer);

	cmd_runtime_remove("transient-owner");
	CHECKF(cmd_state()->transient.owner == CMD_ID_NONE,
	    "a removed command left its state behind");
	CHECK(cmd_transient_value() == 0);
	CHECK(cmd_state()->invocation_depth == 0);
}

static void test_transient_state_is_cleared_on_demand(void)
{
	run_as_keystroke("version");
	cmd_set_transient_value(9);
	run_as_keystroke("version");
	CHECK(cmd_transient_value() == 9);
	cmd_clear_transient();
	CHECK(cmd_transient_value() == 0);
	CHECK(cmd_state()->transient.owner == CMD_ID_NONE);
	/* Forgetting an owner that owns nothing is harmless. */
	cmd_forget_transient_owner(cmd_id_by_name("version"));
	cmd_forget_transient_owner(CMD_ID_NONE);
	CHECK(cmd_transient_value() == 0);
}

/* The self-insert fallback runs a command without cmd_invoke(), so it
 * has to be refused and published the same way one that does would be. */
static void test_fast_path_applies_the_same_policy_and_identity(void)
{
	command_id id = cmd_id_by_name("self-insert-command");
	command_id outer = CMD_ID_NONE;

	CHECK(id != CMD_ID_NONE);
	cmd_state_begin_keystroke();
	CHECK(cmd_fast_path_begin("self-insert-command", &outer) == 1);
	CHECK(cmd_state()->this_command == id);
	cmd_fast_path_end(outer);
	CHECK(cmd_state()->invocation_depth == 0);
	cmd_state_begin_keystroke();
	CHECKF(cmd_state()->last_command == id,
	    "the fast path did not publish what ran");

	/* A read-only buffer refuses it, and nothing is published. */
	bcur()->readonly = 1;
	cmd_state_begin_keystroke();
	CHECK(cmd_fast_path_begin("self-insert-command", &outer) == 0);
	CHECK(cmd_state()->this_command == CMD_ID_NONE);
	bcur()->readonly = 0;

	CHECK(cmd_fast_path_begin("no-such-command", &outer) == 0);
	CHECK(cmd_state()->invocation_depth == 0);
}

/* The key is dispatch state, not identity: it says what self-insert
 * inserts, and two keys bound to one command are still one command. */
static void test_the_keystrokes_key_is_recorded(void)
{
	struct key_event key = { 'q', 0 };

	cmd_state_begin_keystroke();
	cmd_state_set_key(key);
	CHECK(key_event_equal(cmd_state()->this_key, key));
	CHECK(cmd_state()->shift_translated == 0);
	cmd_state_set_shift_translated();
	CHECK(cmd_state()->shift_translated == 1);
	cmd_state_begin_keystroke();
	CHECKF(cmd_state()->shift_translated == 0,
	    "shift translation outlived its keystroke");
	CHECKF(key_event_equal(cmd_state()->this_key, key),
	    "the key is replaced by the next keystroke, not cleared");
}

/* An identity resolves back to the descriptor it came from, which is how
 * dispatch bookkeeping asks what the command that just ran may keep. */
static void test_descriptors_are_reachable_by_id(void)
{
	int i, n = table_size();

	CHECK(cmd_descriptor_by_id(CMD_ID_NONE) == NULL);
	CHECK(cmd_descriptor_by_id(CMD_ID_RUNTIME_BASE) == NULL);
	CHECK(cmd_descriptor_by_id(CMD_ID_STATIC_BASE + (command_id)n) == NULL);
	for (i = 0; i < n; i++) {
		const struct named_cmd *cmd = cmd_descriptor_at(i);

		CHECKF(cmd_descriptor_by_id(cmd_id_by_name(cmd->name)) == cmd,
		    "%s does not resolve back to its descriptor", cmd->name);
	}
}

/* The diagnostics command's actual output, not just its table row.
 *
 * Sub-plan 09D's manifest row (test/lisp-compat/features.json,
 * `command-lisp-arena-stats`) cites this test and test/pty/
 * lisp-arena-stats-command.yaml: this one pins the rendering against a
 * live arena in-process, the PTY case pins that the same line reaches a
 * real terminal's echo area.  The numbers themselves are the arena's, so
 * this asserts the whole line against those same counters, plus the one
 * fact that must hold for a session which has only ever loaded the
 * prelude: no allocation has failed.
 *
 * Under WITH_LISP=0 the same command reports "Lisp not available",
 * which is the other half of the contract and is asserted here too. */
static void test_lisp_arena_stats_renders(void)
{
	char expected[256];
	struct kg_lisp_arena_stats stats;

	if (!kg_lisp_active()) {
		editor.statusmsg[0] = '\0';
		CHECK(run("lisp-arena-stats", CMD_ORIGIN_LISP) == CMD_RAN);
		CHECK(strcmp(editor.statusmsg, "Lisp not available") == 0);
		return;
	}
	CHECK(kg_lisp_init() == 0);
	editor.statusmsg[0] = '\0';
	CHECK(run("lisp-arena-stats", CMD_ORIGIN_LISP) == CMD_RAN);
	CHECK(kg_lisp_arena_stats(&stats) == 0);
	/* Every field, not a prefix and four substrings: reading the
	 * counters allocates nothing and mutates no Fe state (that is the
	 * property the CMD_LISP_CALLABLE row rests on), so the second read
	 * has to answer exactly what the command rendered from.  This is
	 * what makes the frames/capacity pair asserted here rather than
	 * only in the PTY case -- capacity is a fixed property of the
	 * arena layout, so it is stable within a session by construction.
	 * The trailing byte count is that same fixed property one level
	 * further out -- which arena this session opened, now that
	 * $KG_LISP_ARENA_BYTES makes that a run-time answer. */
	snprintf(expected, sizeof(expected),
	    "Arena: %zu slots, %zu free, peak %zu; GC %zu; roots %zu; "
	    "frames %zu/%zu; fails %zu; %zu bytes; "
	    "payload %zu/%zu bytes, peak %zu; compactions %zu; "
	    "payload fails %zu",
	    stats.total_slots, stats.free_slots, stats.peak_live_objects,
	    stats.collection_count, stats.peak_gc_stack_depth,
	    stats.peak_frame_depth, stats.frame_capacity,
	    stats.allocation_failures, stats.arena_bytes,
	    stats.payload_live_bytes, stats.payload_capacity_bytes,
	    stats.payload_peak_bytes, stats.payload_compaction_count,
	    stats.payload_allocation_failures);
	CHECKF(strcmp(editor.statusmsg, expected) == 0,
	    "rendered %s, expected %s", editor.statusmsg, expected);
	CHECK(stats.frame_capacity > 0);
	/* A session that has only booted has never failed an allocation. */
	CHECKF(strstr(editor.statusmsg, "; fails 0") != NULL, "rendered %s",
	    editor.statusmsg);
	/* And has a payload region it has not touched: kg carves one since
	 * Phase 24 because a vector's elements live in it (src/lisp_core.c's
	 * lisp_arena_options), and a session that has only booted has built
	 * no vector, so live is 0 of a nonzero capacity and the three
	 * counters after it are the literal zeros.  The whole-line
	 * comparison above already pins the rendering; this pins the
	 * DECISION, in the one place a reader of the command's output would
	 * look for it. */
	CHECK(stats.payload_capacity_bytes > 0);
	CHECKF(strstr(editor.statusmsg, "; payload 0/") != NULL, "rendered %s",
	    editor.statusmsg);
	CHECKF(strstr(editor.statusmsg,
		   " bytes, peak 0; compactions 0; payload fails 0")
		!= NULL,
	    "rendered %s", editor.statusmsg);
	kg_lisp_shutdown();
}

/* The debugger's rows, counted.  Every one of them exists in BOTH
 * configurations -- a row that vanished with `WITH_DAP=0` would leave
 * `M-x dap-continue` reporting a command nobody has heard of -- and
 * test/pty/dap-absent-without-the-feature.yaml sweeps all of them through
 * `M-x` on such a build to prove each answers the one sentence instead.
 * That case names them one by one, so a new `dap-*` row that did not grow
 * it would ship an untested stub.  THIS NUMBER AND THAT CASE MOVE
 * TOGETHER; the case's own header says how its list is generated. */
#define DAP_COMMAND_COUNT 23

static void test_dap_command_rows_are_all_swept_by_the_pty_case(void)
{
	int i, n = table_size(), found = 0;

	for (i = 0; i < n; i++) {
		if (strncmp(cmd_descriptor_at(i)->name, "dap-", 4) == 0) {
			found++;
		}
	}
	CHECKF(found == DAP_COMMAND_COUNT,
	    "%d dap-* rows, expected %d: extend "
	    "test/pty/dap-absent-without-the-feature.yaml to sweep the new "
	    "one, then this count",
	    found, DAP_COMMAND_COUNT);
}

int main(void)
{
	RUN(test_names_sorted_and_unique);
	RUN(test_every_entry_has_a_handler_and_summary);
	RUN(test_lisp_callable_set_is_the_audited_one);
	RUN(test_terminal_reading_classification);
	RUN(test_lisp_callable_mutation_verdicts);
	RUN(test_lisp_arena_stats_renders);
	RUN(test_lookup_edges);
	RUN(test_static_ids_are_table_slots);
	RUN(test_identity_is_published_for_every_origin);
	RUN(test_a_keystroke_without_a_command_ends_the_history);
	RUN(test_refused_commands_are_not_what_ran_last);
	RUN(test_nested_invocation_restores_the_outer_identity);
	RUN(test_runtime_ids_are_stable_across_redefinition);
	RUN(test_runtime_id_table_is_bounded);
	RUN(test_transient_state_survives_only_a_repeat);
	RUN(test_transient_state_is_cleared_by_its_owners_removal);
	RUN(test_transient_state_is_cleared_on_demand);
	RUN(test_fast_path_applies_the_same_policy_and_identity);
	RUN(test_the_keystrokes_key_is_recorded);
	RUN(test_descriptors_are_reachable_by_id);
	RUN(test_dap_command_rows_are_all_swept_by_the_pty_case);
	return test_summary();
}
