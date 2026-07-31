/* test_cmd.c — invariants of the one command table.
 *
 * cmdtable is the single statement of what a command is allowed to do
 * (plan 11 phase 1): whether it edits the buffer, whether Lisp may ask
 * for it, and what it is called in one line.  Nothing else in the tree
 * enforces those properties, so they are asserted here.
 *
 * This binary links every editor translation unit except main.c, the way
 * test_perf does, because reaching the table means linking cmd.o and
 * cmd.o reaches most of the editor.  It calls no handler. */

#include "../src/cmd.h"
#include "../src/def.h"
#include "test.h"

#include <string.h>

/* The 11 commands Lisp's (command-execute ...) accepted before the
 * allow-list in lisp.c was deleted.  cmd_invoke() must still accept
 * exactly these and no others. */
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
			   & ~(unsigned)(CMD_EDITS_BUFFER | CMD_LISP_CALLABLE))
			== 0,
		    "%s has an unknown flag", cmd->name);
	}
}

static void test_lisp_callable_set_is_the_historical_one(void)
{
	size_t k,
	    expected = sizeof(historical_lisp_callable)
	    / sizeof(*historical_lisp_callable);
	int i, n = table_size(), found = 0;

	for (k = 0; k < expected; k++) {
		const struct named_cmd *cmd
		    = cmd_lookup(historical_lisp_callable[k]);

		CHECKF(cmd != NULL, "%s is gone from the table",
		    historical_lisp_callable[k]);
		if (cmd) {
			CHECKF(cmd->flags & CMD_LISP_CALLABLE,
			    "%s lost CMD_LISP_CALLABLE", cmd->name);
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
	    = { "electric-pair-mode", "version", "what-cursor-position" };
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

int main(void)
{
	RUN(test_names_sorted_and_unique);
	RUN(test_every_entry_has_a_handler_and_summary);
	RUN(test_lisp_callable_set_is_the_historical_one);
	RUN(test_lisp_callable_mutation_verdicts);
	RUN(test_lookup_edges);
	return test_summary();
}
