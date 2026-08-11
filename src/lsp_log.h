#ifndef KG_LSP_LOG_H
#define KG_LSP_LOG_H

#include <stddef.h>

/* The `*lsp-log*` buffer: what the language servers said when nobody was
 * asking them anything.  A server's standard error used to go to
 * /dev/null, so a server that explained its refusal explained it to
 * nobody; this is where the explanation goes instead, together with the
 * requests kg stopped waiting for (src/lsp_client.h's deadline) and the
 * reason a client died.
 *
 * It is a record, never an interruption.  The buffer is created on the
 * first line and never selected, never popped into a window and never
 * mentioned in the echo area: `C-x b` is how a user asks for it, on the
 * day they want to know why something hung.  That is the whole of its
 * policy, and it is why this module has no command.
 *
 * The lines are prefixed with the server's name (`[clangd] ...`), which is
 * what keeps two servers writing at once tellable apart.
 *
 * This header names no editor type, so it compiles on its own
 * (`make header-check`), and the store below is pure enough to test
 * without one.
 */

#define LSP_LOG_BUFFER_NAME "*lsp-log*"

/* CONSTRAINT: the log holds at most this many bytes, and a line that
 * pushes it past the cap evicts whole lines from the head -- oldest
 * first, never a partial line -- until the store is a quarter clear of the
 * cap again, and the buffer is rewritten from the store when that happens
 * so what is on screen is what is kept.
 *
 * The quarter is what makes "the rare occasion the cap is crossed" true:
 * evicting to the cap exactly would rewrite the whole buffer once per line
 * for as long as a server kept talking, and rewriting it is the expensive
 * half.  Evicting to three quarters instead buys 16 KiB of appends before
 * the next rewrite.
 *
 * 64 KiB is roughly a thousand lines of server chatter: past what anyone
 * scrolls back through, and small enough that the whole thing can be
 * rewritten in one append.  A log that grew without a bound would be a
 * memory leak with a friendly name, since a misconfigured server can write
 * to its stderr forever. */
#define LSP_LOG_MAX_BYTES (64u * 1024u)

/* The retained text, as bytes with newlines in them: the buffer's contents
 * are a copy of this, which is what makes the head eviction above possible
 * at all -- a buffer can be appended to and cleared, not shortened from
 * the front. */
struct lsp_log_store {
	char *data;
	size_t len;
	size_t cap;
};

/* Append `len` bytes (the caller's own newline included) and, when that
 * takes the store past `max_bytes`, evict whole lines from the head until
 * it is back under three quarters of it.  `*trimmed` says whether anything
 * was evicted, which is what tells an append apart from a rewrite.  False,
 * with the store unchanged, when memory ran out. */
bool lsp_log_store_push(struct lsp_log_store *s, const char *text, size_t len,
    size_t max_bytes, bool *trimmed);

/* Empty the store and free what it held. */
void lsp_log_store_reset(struct lsp_log_store *s);

/* Start listening to the LSP client layer.  Called once from main.c's
 * init, beside compile_nav_install(): the log is editor state, so it is
 * installed by the editor rather than from behind the LSP facade, which
 * every test binary links.  A no-op in a WITH_LSP=0 build. */
void lsp_log_install(void);

/* One line into the log, from `server` -- the shape of the client layer's
 * log hook, and public so a test can drive it. */
void lsp_log_line(const char *server, const char *text, size_t len);

#endif /* KG_LSP_LOG_H */
