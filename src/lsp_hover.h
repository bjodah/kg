/* lsp_hover.h — "what is this thing?", asked of a language server
 *
 * `textDocument/hover` is the one request whose answer is prose rather
 * than a place, so this module is a renderer with a request attached
 * rather than the other way round: the interesting half is turning
 * `Hover.contents` -- three different shapes across three revisions of the
 * protocol, one of them Markdown -- into plain text kg can put in front of
 * a reader.
 *
 * kg has no eldoc, so nothing asks this question by itself: `M-x
 * lsp-hover` is the whole of the interface, and the answer goes to the
 * echo area the way eldoc's does.  There is no key binding.  Emacs has
 * none either (eldoc is automatic there), and `C-c <key>` is reserved for
 * the user's own bindings in kg (src/keybind.h).
 */

#ifndef KG_LSP_HOVER_H
#define KG_LSP_HOVER_H

#include <stddef.h>

/* Declared, not included: src/lsp_json.h defines it, and a caller that has
 * one to pass has already included that. */
struct lsp_json_value;

/* Where the full text of a multi-line answer goes.  Created lazily, never
 * selected and never given a window -- `C-x b` reaches it -- which is
 * `*lsp-log*`'s policy exactly (src/lsp_log.h) and for the same reason: an
 * answer to a question about a symbol is a record, not an interruption. */
#define LSP_HOVER_BUFFER_NAME "*lsp-hover*"

/* How much of an answer kg keeps.  A server is entitled to send a
 * paragraph of documentation; this is well past any signature and short
 * enough that the whole of it is one append to a buffer. */
#define LSP_HOVER_MAX 4096

/* `Hover.contents` as plain text, written to `out` and NUL-terminated.
 * Returns the number of bytes written, 0 for a node that carries no text
 * at all -- which is how a server says it has nothing to say about this
 * position, and is not an error.
 *
 * All three shapes the protocol has ever had are read: a MarkupContent
 * object (`{kind, value}`), a bare string, and a MarkedString (a string,
 * or `{language, value}`) or an array of any of those, joined with
 * newlines.  A shape is recognised by the members present rather than by a
 * `kind` nobody is required to send.
 *
 * Markdown is neutralised rather than rendered, and only the noise that
 * would otherwise be read as text: fence lines go, backticks go, a
 * heading's leading `#` goes, a horizontal rule goes.  Nothing here
 * decides how a byte reaches a terminal -- display_glyph_at() does, as it
 * does for every other byte kg did not write (src/width.h) -- so this is
 * about legibility and not about safety.
 *
 * Pure, and public for that reason: the three shapes are where a hover
 * client is either right or shows a reader a JSON fragment. */
size_t lsp_hover_render(
    const struct lsp_json_value *contents, char *out, size_t out_size);

/* `M-x lsp-hover`.  Asks the server about point and reports the answer's
 * first line in the echo area; an answer with more lines in it also goes
 * whole to LSP_HOVER_BUFFER_NAME, which the message then names.
 *
 * The divergence from Emacs worth stating: eglot's eldoc shows the same
 * first line in the echo area and offers the rest through `M-x
 * eldoc-doc-buffer`, on demand and in a window.  kg writes the rest
 * unconditionally and shows it never -- one buffer, one policy, no
 * window management, and nothing lost. */
void editor_lsp_hover(int fd);

#endif /* KG_LSP_HOVER_H */
