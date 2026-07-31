/* stubs_win.c — window globals and no-op window entry points for tests that
 * link bufmgr.o but not winmgr.o.  test_winmgr links the real winmgr.o
 * instead of this file, which is why these live apart from stubs_buffer.c. */

#include "../src/def.h"

struct editor_window winlist[MAX_WINDOWS];
int win_current = 0;
int win_count = 0;
int win_total_rows = 24;
int win_total_cols = 80;

void win_display_buffer_other_window(int idx) { (void)idx; }
void win_position_at_end(int idx) { (void)idx; }
