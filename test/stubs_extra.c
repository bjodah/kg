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

/* word.o asks Lisp what `fill-column' is; a suite that links neither the
 * adapter nor an interpreter gets the same answer a WITH_LISP=0 build
 * gets, which is what makes test_word's reflow expectations the pre-Lisp
 * ones. */
int kg_lisp_variable_integer(const char *name, int fallback)
{
	(void)name;
	return fallback;
}
