/* stubs_extra.c — stub for tests that link autocomplete.o or word.o
 * without linking basic.o (which provides the real editor_move_cursor). */

#include "../src/def.h"

void editor_move_cursor(int key) { (void)key; }
void editor_refresh_screen(void) { }
int editor_read_raw_byte(int fd)
{
	(void)fd;
	return 0;
}
