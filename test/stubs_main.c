/* stubs_main.c — the globals main.c owns.
 *
 * test_perf and test_cmd link every editor translation unit except
 * main.c, so that what they assert about is the code the editor runs
 * rather than a stubbed imitation of it.  What is left over is main()'s
 * share of the globals; init_editor() is deliberately not reproduced,
 * because it opens a terminal and installs signal handlers that a unit
 * test has no use for.  Each test sets the few fields it needs itself. */

#include "../src/def.h"

struct editor_config editor;
int running = 1;
/* main.c latches this from Lisp; a stub never inhibits the
 * startup screen, so display.c draws it exactly as it always did. */
int inhibit_startup_screen = 0;
int kg_exit_status = 0;
int global_auto_revert = 0;
int electric_pairs = 0;
