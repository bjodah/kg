/* visit.c — see visit.h. */

#include "visit.h"

#include "bufhandle.h"
#include "def.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

void editor_visit_escape_window(struct kg_buffer_handle escape_from)
{
	if (win_count <= 1 || !win_shows_buffer(wcur(), escape_from)) {
		return;
	}
	for (int i = 1; i <= MAX_WINDOWS; i++) {
		int idx = (win_current + i) % MAX_WINDOWS;

		if (winlist[idx].active && idx != win_current) {
			win_current = idx;
			return;
		}
	}
}

/* Whether opening `path` is something this module is willing to do, with
 * the reason reported when it is not.  Only asked of a path no buffer is
 * already visiting: a buffer that is open has already answered every one of
 * these questions, and a file deleted under it is still a buffer worth
 * showing. */
static bool visit_may_open(const char *who, const char *path)
{
	struct stat st;

	if (buf_find_open(path) >= 0) {
		return true;
	}
	if (stat(path, &st) != 0) {
		editor_set_status_message(
		    "%s: %s: %s", who, path, strerror(errno));
		return false;
	}
	if (S_ISDIR(st.st_mode)) {
		editor_set_status_message("%s: %s is a directory", who, path);
		return false;
	}
	if (buf_count >= MAX_BUFFERS) {
		editor_set_status_message(
		    "%s: too many open buffers (%d max)", who, MAX_BUFFERS);
		return false;
	}
	return true;
}

bool editor_visit_file_position(const char *who, const char *path, int line,
    int col, struct kg_buffer_handle escape_from)
{
	if (!visit_may_open(who, path)) {
		return false;
	}
	editor_visit_escape_window(escape_from);
	buf_open_path(path, 0);
	if (!bcur()->filename || strcmp(bcur()->filename, path) != 0) {
		return false;
	}
	editor_goto_line_direct(line, col);
	return true;
}
