/* next_error.c — see next_error.h. */

#include "next_error.h"

#include "def.h"

static const struct next_error_source *g_source;

void next_error_set_source(const struct next_error_source *source)
{
	g_source = source;
}

const char *next_error_current_source_name(void)
{
	return g_source ? g_source->name : NULL;
}

/* Unreachable in an editor: compile_nav_install() registers compilation
 * from init_editor(), before any key can be read.  It is reachable in a
 * test binary that links this without that call, and answering with a
 * message rather than a crash is what makes the seam safe to link. */
static void next_error_move(int direction)
{
	if (!g_source) {
		editor_set_status_message("No next-error buffer");
		return;
	}
	g_source->move(direction);
}

void editor_next_error(int fd)
{
	(void)fd;
	next_error_move(1);
}

void editor_previous_error(int fd)
{
	(void)fd;
	next_error_move(-1);
}
