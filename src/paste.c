/* ======================== Bracketed paste reporting ======================== */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bufmgr.h"
#include "def.h"
#include "edit.h"
#include "paste.h"

static struct {
	char *data;
	size_t len;
} pending_paste;

static bool paste_reporting;

bool kg_bracketed_paste_term_supported(const char *term)
{
	return term && term[0] != '\0' && strcmp(term, "dumb") != 0
	    && strcmp(term, "unknown") != 0;
}

static bool paste_wanted_now(void)
{
#ifdef KG_FUZZ
	return true;
#endif
	return kg_bracketed_paste_term_supported(getenv("TERM"));
}

void kg_bracketed_paste_start(void)
{
	if (!paste_wanted_now() || paste_reporting) {
		return;
	}
	(void)tty_write(BRACKETED_PASTE_ON, sizeof(BRACKETED_PASTE_ON) - 1);
	paste_reporting = true;
}

void kg_bracketed_paste_stop(void)
{
	if (!paste_reporting) {
		return;
	}
	(void)tty_write(BRACKETED_PASTE_OFF, sizeof(BRACKETED_PASTE_OFF) - 1);
	paste_reporting = false;
	kg_bracketed_paste_clear();
}

void kg_bracketed_paste_record(char *data, size_t len)
{
	kg_bracketed_paste_clear();
	pending_paste.data = data;
	pending_paste.len = len;
}

void kg_bracketed_paste_clear(void)
{
	free(pending_paste.data);
	pending_paste.data = NULL;
	pending_paste.len = 0;
}

const char *kg_bracketed_paste_data(size_t *len)
{
	if (len) {
		*len = pending_paste.len;
	}
	return pending_paste.data;
}

void kg_bracketed_paste_handle_pending(void)
{
	if (!pending_paste.data || pending_paste.len == 0) {
		kg_bracketed_paste_clear();
		return;
	}
	if (bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		kg_bracketed_paste_clear();
		return;
	}
	editor_insert_text_at_point(pending_paste.data, (int)pending_paste.len);
	kg_bracketed_paste_clear();
}
