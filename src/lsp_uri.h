#ifndef KG_LSP_URI_H
#define KG_LSP_URI_H

#include <stdbool.h>
#include <stddef.h>

/* `file:` URIs, in both directions, for a protocol that names every
 * document by one.
 *
 * It is its own pair of files rather than a corner of the document sync
 * because two modules need it and only one of them may see the editor: the
 * client writes the workspace `rootUri` of its `initialize` request
 * (src/lsp_client.c) and the sync layer writes one per open document
 * (src/lsp_sync.c).  Nothing here includes anything but the C library, so
 * the client keeps linking into a test binary with no editor object in it.
 *
 * The encoding is deliberately conservative in one direction and strict in
 * the other.  Writing, every byte outside RFC 3986's unreserved set --
 * everything but the separator a path is made of -- is percent-encoded:
 * over-encoding a byte is legal and readable, under-encoding one is a URI
 * some server rejects.  Reading, anything that is not a `file:` URI kg can
 * turn back into exactly the bytes of a path is refused, because the only
 * caller is one that would otherwise open, or navigate to, the wrong file.
 *
 * Neither direction knows what a valid path is.  Bytes above 0x7F are
 * percent-encoded and decoded byte for byte, so a path whose name is UTF-8
 * round-trips, and so does one whose name is not.
 */

/* `abs_path` (which must be absolute) as a `file://` URI, written to `out`.
 * Returns false for a relative or empty path, or when the encoded result
 * would not fit -- which a caller reads as "there is no URI for this", not
 * as an error to report: a client with no `rootUri` still works. */
bool lsp_uri_from_path(const char *abs_path, char *out, size_t out_size);

/* The path a `file:` URI names, percent-decoded into `out`.  Returns false,
 * having written nothing worth reading, for anything else:
 *
 *   - another scheme (`http:`, `jar:`, or no scheme at all);
 *   - an authority that is neither empty nor `localhost`, since
 *     `file://server/share` names a file on another host and kg has no way
 *     to open it;
 *   - a `%` that is not followed by two hexadecimal digits;
 *   - `%00`, because a path is a NUL-terminated string and a decoded NUL
 *     would silently truncate it -- the classic way a check on the whole
 *     name ends up applied to a prefix of it;
 *   - a result that does not fit `out_size`.
 *
 * The scheme and the `localhost` authority are compared case-insensitively,
 * as RFC 3986 requires; everything after the authority is path bytes and is
 * decoded literally, query and fragment syntax included, since a language
 * server that puts either in a document URI is not describing a file kg
 * can find. */
bool lsp_uri_to_path(const char *uri, char *out, size_t out_size);

#endif /* KG_LSP_URI_H */
