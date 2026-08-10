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

/* The 11 commands Lisp's (command-execute ...) accepted before the
 * allow-list in lisp.c was deleted, plus every command a later slice
 * deliberately added to the set (below).  cmd_invoke() must still accept
 * exactly the two lists together and nothing else -- the point of this
 * test is that widening the set is a decision somebody wrote down, not a
 * flag that drifted. */
static const char *const historical_lisp_callable[] = {
	"capitalize-word",
	"delete-horizontal-space",
	"delete-trailing-space",
	"downcase-word",
	"electric-pair-mode",
	"join-line",
	"just-one-space",
	"transpose-chars",
	"upcase-word",
	"version",
	"what-cursor-position",
};

/* Deliberate additions, one line of reason each.
 *
 * lisp-arena-stats (sub-plan 09D): the arena-diagnostics command.  It
 * reads Fe's counters through kg_lisp_arena_stats(), which allocates
 * nothing and mutates no state, and its whole effect is one echo-area
 * line -- the same shape as `version`, which has been Lisp-callable
 * since before the allow-list was deleted.  A Lisp program that has just
 * caught an `arena-exhaustion` is exactly the caller that wants it.
 *
 * dabbrev-expand: a synchronous, self-contained edit of the current
 * buffer -- it reads no key, opens no prompt and waits on nothing, and
 * CMD_EDITS_BUFFER already gives it the read-only refusal every other
 * editing command in this set has.  That is what separates it from the
 * xref commands, which stay out: those return before the language
 * server has answered, so a Lisp caller would get control back with
 * nothing to observe.  A Lisp caller only ever gets the first expansion
 * -- the cycle is keyed on command identity and a command reached
 * through (command-execute ...) is nested -- which is a smaller
 * behaviour than the key has, not a wider one.
 *
 * xterm-mouse-mode: a global mode toggle with no buffer effect at all --
 * one flag, one pair of DECSET strings and one echo-area line, which is
 * `electric-pair-mode`'s shape exactly, and that has been Lisp-callable
 * since before the allow-list was deleted.  Mouse reporting is on by
 * default, so an init file that wants it off is the caller this is for;
 * without the flag that init file would have no way to say so.
 *
 * show-paren-mode: the same shape again -- one global flag, no buffer
 * effect, one echo-area line -- and the same reason it is needed.  The
 * mode is ON by default (Emacs 28.1 and later), so an init file that
 * wants it off is exactly the caller here, and without the flag it would
 * have no way to say so.  It is not CMD_EDITS_BUFFER: the highlight is a
 * decoration, and a decoration changes no byte of the text, which is why
 * it stays available in a read-only buffer as it does in Emacs. */
static const char *const added_lisp_callable[] = {
	"dabbrev-expand",
	"lisp-arena-stats",
	"show-paren-mode",
	"xterm-mouse-mode",
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
			       | CMD_REPEATS | CMD_KEEPS_GOAL_COLUMN))
			== 0,
		    "%s has an unknown flag", cmd->name);
	}
}

static void test_lisp_callable_set_is_the_historical_one(void)
{
	static const char *const *const lists[]
	    = { historical_lisp_callable, added_lisp_callable };
	static const size_t lengths[] = {
		sizeof(historical_lisp_callable)
		    / sizeof(*historical_lisp_callable),
		sizeof(added_lisp_callable) / sizeof(*added_lisp_callable),
	};
	size_t k, list, expected = lengths[0] + lengths[1];
	int i, n = table_size(), found = 0;

	for (list = 0; list < sizeof(lists) / sizeof(*lists); list++) {
		for (k = 0; k < lengths[list]; k++) {
			const struct named_cmd *cmd
			    = cmd_lookup(lists[list][k]);

			CHECKF(cmd != NULL, "%s is gone from the table",
			    lists[list][k]);
			if (cmd) {
				CHECKF(cmd->flags & CMD_LISP_CALLABLE,
				    "%s lost CMD_LISP_CALLABLE", cmd->name);
			}
		}
	}
	for (i = 0; i < n; i++) {
		if (cmd_descriptor_at(i)->flags & CMD_LISP_CALLABLE) {
			found++;
		}
	}
	CHECKF(found == (int)expected,
	    "%d commands are Lisp-callable, the allow-list had %zu", found,
	    expected);
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

	/* Not callable from Lisp. */
	CHECK(run("sort-lines", CMD_ORIGIN_LISP) == CMD_NOT_CALLABLE);
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
	 * arena layout, so it is stable within a session by construction. */
	snprintf(expected, sizeof(expected),
	    "Arena: %zu slots, %zu free, peak %zu; GC %zu; roots %zu; "
	    "frames %zu/%zu; fails %zu",
	    stats.total_slots, stats.free_slots, stats.peak_live_objects,
	    stats.collection_count, stats.peak_gc_stack_depth,
	    stats.peak_frame_depth, stats.frame_capacity,
	    stats.allocation_failures);
	CHECKF(strcmp(editor.statusmsg, expected) == 0,
	    "rendered %s, expected %s", editor.statusmsg, expected);
	CHECK(stats.frame_capacity > 0);
	/* A session that has only booted has never failed an allocation. */
	CHECKF(strstr(editor.statusmsg, "; fails 0") != NULL, "rendered %s",
	    editor.statusmsg);
	kg_lisp_shutdown();
}

int main(void)
{
	RUN(test_names_sorted_and_unique);
	RUN(test_every_entry_has_a_handler_and_summary);
	RUN(test_lisp_callable_set_is_the_historical_one);
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
	return test_summary();
}
