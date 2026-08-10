/* lsp_log.c — see lsp_log.h.
 *
 * Two halves.  The store is a bounded byte buffer with whole-line eviction
 * and no editor in it at all, which is where the cap policy lives and what
 * a unit test drives.  The mirror is the half that owns a buffer: it
 * appends the new line in the ordinary case and rewrites `*lsp-log*` from
 * the store on the rare one where the cap was crossed, since a buffer can
 * be appended to and cleared but not shortened from the front.
 *
 * Everything here runs from lsp_poll(), which is the editor's own poll
 * site -- the same place an xref answer lands and paints -- so appending
 * to a buffer here is as safe as it is there, and neither half ever
 * selects a buffer or touches a window.
 */

#include "lsp_log.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------- the store ---------------------------- */

/* Drop whole lines from the front until the store fits `max_bytes`.  A
 * store holding one line longer than the whole cap is emptied rather than
 * cut mid-line: the alternative is a first line that starts in the middle
 * of a word, which reads as corruption rather than as eviction. */
static bool store_trim(struct lsp_log_store *s, size_t max_bytes)
{
	const char *nl;
	size_t drop;

	if (s->len <= max_bytes) {
		return false;
	}
	while (s->len > max_bytes) {
		nl = memchr(s->data, '\n', s->len);
		if (!nl) {
			s->len = 0;
			break;
		}
		drop = (size_t)(nl - s->data) + 1;
		memmove(s->data, s->data + drop, s->len - drop);
		s->len -= drop;
	}
	return true;
}

static bool store_reserve(struct lsp_log_store *s, size_t extra)
{
	size_t need = s->len + extra;
	size_t cap = s->cap ? s->cap : 1024;
	char *data;

	if (need < s->len) {
		return false; /* size_t overflow */
	}
	if (need <= s->cap) {
		return true;
	}
	while (cap < need) {
		if (cap > (size_t)-1 / 2) {
			cap = need;
			break;
		}
		cap *= 2;
	}
	data = realloc(s->data, cap);
	if (!data) {
		return false;
	}
	s->data = data;
	s->cap = cap;
	return true;
}

bool lsp_log_store_push(struct lsp_log_store *s, const char *text, size_t len,
    size_t max_bytes, bool *trimmed)
{
	if (trimmed) {
		*trimmed = false;
	}
	if (len == 0) {
		return true;
	}
	if (!store_reserve(s, len)) {
		return false;
	}
	memcpy(s->data + s->len, text, len);
	s->len += len;
	if (store_trim(s, max_bytes) && trimmed) {
		*trimmed = true;
	}
	return true;
}

void lsp_log_store_reset(struct lsp_log_store *s)
{
	free(s->data);
	s->data = NULL;
	s->len = 0;
	s->cap = 0;
}

#ifdef KG_USE_LSP

#include "bufhandle.h"
#include "def.h"
#include "lsp_client.h"

#include <limits.h>
#include <stdio.h>

/* One line's room: the longest run the transport calls a stderr line
 * (LSP_TRANSPORT_MAX_STDERR_LINE, 4096) plus the prefix and the newline,
 * so a server's longest line reaches the log whole. */
#define LSP_LOG_LINE_MAX 4160

static struct lsp_log_store g_store;
static struct kg_buffer_handle g_buffer;

/* The slot holding the log, created on first use.  `*fresh` says the
 * buffer is empty and has to be filled from the store rather than appended
 * to -- which is true when it has just been made, and equally true when
 * the user killed the last one and this is its replacement.  -1 when the
 * editor had no room for it, which is not worth a message: the log is a
 * record nobody asked for yet. */
static int log_slot(bool *fresh)
{
	struct editor_buffer *b = buf_resolve(g_buffer);
	int slot;

	*fresh = false;
	if (b && b->filename && strcmp(b->filename, LSP_LOG_BUFFER_NAME) == 0) {
		return buf_handle_slot(g_buffer);
	}
	slot = buf_prepare_special_text(LSP_LOG_BUFFER_NAME, &text_syntax, 1);
	if (slot < 0) {
		return -1;
	}
	g_buffer = buf_handle(slot);
	*fresh = true;
	return slot;
}

void lsp_log_line(const char *server, const char *text, size_t len)
{
	char line[LSP_LOG_LINE_MAX];
	bool trimmed = false;
	bool fresh = false;
	int slot;
	int n;

	if (len > INT_MAX) {
		return;
	}
	n = snprintf(line, sizeof(line), "[%s] %.*s\n",
	    (server && server[0]) ? server : "lsp", (int)len, text ? text : "");
	if (n < 0) {
		return;
	}
	if ((size_t)n >= sizeof(line)) {
		n = (int)sizeof(line) - 1;
	}
	if (!lsp_log_store_push(
		&g_store, line, (size_t)n, LSP_LOG_MAX_BYTES, &trimmed)) {
		return;
	}
	slot = log_slot(&fresh);
	if (slot < 0) {
		return;
	}
	/* The two rewriting cases, and they are the same rewrite: a buffer
	 * that has just been created holds nothing, and one whose store has
	 * evicted its head holds too much at the front. */
	if (fresh || trimmed) {
		buf_clear_special_text(slot);
		(void)buf_append_special_text(slot, g_store.data, g_store.len);
		return;
	}
	(void)buf_append_special_text(slot, line, (size_t)n);
}

void lsp_log_install(void) { lsp_client_set_log_hook(lsp_log_line); }

#else /* !KG_USE_LSP */

void lsp_log_install(void) { }

void lsp_log_line(const char *server, const char *text, size_t len)
{
	(void)server;
	(void)text;
	(void)len;
}

#endif /* KG_USE_LSP */
