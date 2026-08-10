#ifndef KG_LSP_SYNC_H
#define KG_LSP_SYNC_H

#include <stddef.h>

#include "lsp_client.h" /* enum lsp_position_encoding */

/* What the server thinks a document says, and where in it a position is.
 *
 * This is the boundary module: everything below it (transport, JSON,
 * client, registry) is editor-free and links into a test with no editor
 * object, and this one reads buffers, because somebody has to.  It knows
 * nothing above it -- not xref, not commands, not the echo area -- and it
 * takes buffers by handle rather than by pointer, so a buffer killed
 * between a request and its answer is a handle that stops resolving rather
 * than a pointer that stops being valid.
 *
 * Synchronisation is lazy and pull-shaped.  kg does not push every
 * keystroke at a language server: KG_EVENT_BUFFER_CHANGED carries byte
 * ranges rather than text and coalesces under pressure (src/event.h), so
 * replaying edits from events is not something a correct implementation
 * could do.  Instead each tracked document keeps a shadow -- the exact
 * bytes last sent -- and lsp_sync_before_request() compares the buffer's
 * `content_generation` against the one the shadow was taken at.  Unchanged
 * costs one comparison; changed costs one flatten and one notification.
 * The consequence worth stating: a server's view of a buffer is only ever
 * as fresh as the last command that asked it something, which is exactly
 * as fresh as it needs to be for that command's answer to be right.
 *
 * The whole module is inert for a server that wants no synchronisation at
 * all (`textDocumentSync` absent or 0, and no `openClose`): such a server
 * reads files from disk, and sending it documents it did not ask for is a
 * protocol violation rather than a kindness.  Inert means inert, including
 * for the first command of all: a document asked for before the handshake
 * has settled is tracked but silent, and what the server said is what
 * decides -- when it reaches READY -- whether its didOpen is sent or the
 * document is forgotten.  That is lsp_sync_install()'s ready hook.
 */

/* Declared, not included: def.h and bufhandle.h define these, and a caller
 * that has a buffer or a handle to pass has already included them. */
struct editor_buffer;
struct kg_buffer_handle;
struct lsp_client;

/* Bounds.  Sixteen documents is the working set of an editing session --
 * every buffer an xref command has been run in, across every server -- and
 * it is per (client, buffer) rather than per buffer, because two servers
 * asked about the same file each need their own shadow and their own
 * version counter.  A seventeenth is refused rather than evicted: evicting
 * a document silently desynchronises the server that still believes in it.
 *
 * A URI is bounded by PATH_MAX's worth of percent-encoding, which is the
 * bound src/lsp_uri.h refuses past. */
#define LSP_SYNC_MAX_DOCS 16
#define LSP_SYNC_MAX_URI 4096

/* ---------------------------- positions ------------------------------- */

/* kg's columns are UTF-8 byte offsets into `row->chars` -- doc/coordinates.md's
 * **chars** space -- and the protocol's are (line, character) with the
 * character counted in the encoding the handshake settled.  These four are
 * that conversion and nothing else: they take a row's bytes rather than a
 * row, so they are pure functions a unit test drives with a string literal,
 * and so that the sync layer can run them over its shadow, which is bytes
 * and has no rows at all.
 *
 * Malformed UTF-8 counts as one unit per byte, the way a display that
 * substitutes one replacement character per bad byte does: kg never
 * refuses to show a file, so it must never refuse to say where a position
 * in one is.  A byte offset that falls inside a glyph is rounded up to the
 * end of that glyph, since the protocol has no way to name a position
 * inside a character; both directions clamp out-of-range input to the end
 * of the line, which is what the specification requires of a server
 * receiving one. */

/* UTF-16 code units in `chars[0..byte_col)`: one per character below
 * U+10000, two for an astral one. */
size_t lsp_pos_utf16_from_byte(const char *chars, size_t len, size_t byte_col);

/* The inverse: the byte offset `utf16_col` code units into `chars`. */
size_t lsp_pos_byte_from_utf16(const char *chars, size_t len, size_t utf16_col);

/* The same pair, choosing by the encoding the server asked for.  For
 * `LSP_POSITION_UTF8` a character offset IS a byte offset -- which is the
 * whole reason kg asks for utf-8 first -- so both are a clamp. */
size_t lsp_pos_encode(enum lsp_position_encoding enc, const char *chars,
    size_t len, size_t byte_col);
size_t lsp_pos_decode(enum lsp_position_encoding enc, const char *chars,
    size_t len, size_t character);

/* ------------------------- document synchronisation ------------------- */

/* The file `b` visits, as an absolute path, written to `out`.  A buffer
 * stores the name it was opened with, which is relative whenever the user
 * typed a relative one (src/bufmgr.c's buf_open_path()), and every part of
 * the protocol that names a file wants an absolute one: a relative URI is
 * one the server resolves against a directory kg never told it about, and a
 * relative workspace-root query is one src/lsp_server.h refuses.
 *
 * Deliberately not realpath(): the file need not exist, and resolving
 * symlinks would hand the server a name it cannot match against the one it
 * found itself.
 *
 * Returns false for a buffer visiting no file, for a working directory that
 * cannot be read, and for a result that does not fit `out_size`.  It is
 * public because the command layer asks the same question before this
 * module does -- src/xref.c needs the path to find the workspace root, one
 * step before the document is synchronised at all. */
bool lsp_sync_abs_path(
    const struct editor_buffer *b, char *out, size_t out_size);

/* Bring `c`'s idea of the buffer `buf` names up to date, and do it before
 * every positional request: a `textDocument/definition` measured against a
 * document the server last saw three edits ago answers about a position
 * that no longer exists.
 *
 * Untracked buffer:      `textDocument/didOpen` with the whole text, version 1
 *                        -- held, not sent, until the client is READY.
 * Unchanged generation:  nothing at all, which is the common case.
 * Changed generation:    `textDocument/didChange` at the next version --
 *                        one contiguous replacement for an incremental
 *                        server, the whole text for a full-sync one -- and
 *                        the shadow is replaced by what was sent.
 * A different file:      the buffer visits somewhere else than the document
 *                        says (C-x C-w), so the old URI is closed and the
 *                        new one opened.  The path is re-derived here on
 *                        every call, since nothing announces such a move.
 *
 * Returns 0 when the server is up to date, including when there was
 * nothing to do, and -1 when it is not: a handle that no longer resolves,
 * a buffer visiting no file, a document table with no room, or a
 * notification that could not be built or sent.  A caller that gets -1 has
 * no business sending a positional request. */
int lsp_sync_before_request(struct lsp_client *c, struct kg_buffer_handle buf);

/* The URI `buf` is synchronised to on `c`, or NULL when it is not tracked.
 * This is what a request's `textDocument.uri` is built from, so it is the
 * one thing above this module that has to agree with what was sent.  The
 * pointer belongs to the document table and stays valid until that
 * document is closed or its client dropped. */
const char *lsp_sync_uri(struct lsp_client *c, struct kg_buffer_handle buf);

/* The version number the server last saw for this document, or -1 when it
 * is not tracked.  For tests, and for a log line; no policy reads it. */
long long lsp_sync_version(struct lsp_client *c, struct kg_buffer_handle buf);

/* Tell every server that has it open that the buffer `buf` named is gone,
 * and forget it.  A killed buffer whose document is left open is a server
 * answering out of a file that no longer has an editor behind it, and --
 * once the handle stops resolving -- one kg can never close, since closing
 * needs the URI this table is holding.
 *
 * lsp_sync_install() subscribes this to KG_EVENT_BUFFER_KILLED, so the
 * editor never calls it by hand: the buffer table publishes the kill and
 * kg_event_drain_safe() delivers it at one of src/main.c's safe points.
 * The handle is dead by then and is used only as an identity -- documents
 * are matched to it by comparing slot, id and generation, never by
 * resolving it -- which is what makes closing a killed buffer's document
 * possible at all.  Still public, and still called directly by the tests,
 * since a document table is worth asserting without an event queue in the
 * way. */
void lsp_sync_close_buffer(struct kg_buffer_handle buf);

/* Forget every document belonging to `c`, without telling it anything.
 * This is the death path, not the closing one: the client is dead or being
 * disposed of, so a `didClose` would be queued for a server nobody is
 * listening to.  Installed as the registry's drop hook by
 * lsp_sync_install(), which is why a reclaimed instance slot cannot leave
 * this table pointing at freed memory. */
void lsp_sync_drop_client(struct lsp_client *c);

/* Wire this module up, once, from lsp_init(): the registry's instance-drop
 * hook, the client's ready hook (which sends -- or abandons -- the didOpens
 * held through a handshake), and a KG_EVENT_BUFFER_KILLED subscriber that
 * closes the killed buffer's documents.
 *
 * Separate from the hooks themselves so that src/lsp_server.c keeps
 * knowing nothing about documents or buffers -- it calls a function
 * pointer it was handed, and a test binary that links the registry without
 * this module simply never hands it one.  Calling it more than once is
 * safe and leaves exactly one subscriber registered. */
void lsp_sync_install(void);

#endif /* KG_LSP_SYNC_H */
