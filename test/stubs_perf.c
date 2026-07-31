/* stubs_perf.c — the globals main.c owns, for test_perf.
 *
 * test_perf links every editor translation unit except main.c, so that a
 * counter assertion measures the same code the editor runs rather than a
 * stubbed imitation of it.  What is left over is main()'s share of the
 * globals; init_editor() is deliberately not reproduced, because it opens
 * a terminal and installs signal handlers that a unit test has no use
 * for.  test_perf sets the few fields it needs itself. */

#include "../src/def.h"

struct editor_config editor;
int running = 1;
int kg_exit_status = 0;
int suppress_undo = 0;
int global_auto_revert = 0;
int electric_pairs = 0;
