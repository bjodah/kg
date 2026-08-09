#ifndef KG_XREF_H
#define KG_XREF_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

/* Cross-referencing commands: "where is this defined", and -- from Stage 6
 * of doc/plans/2026-08-08-lsp.md -- "where is it used".
 *
 * This is the editor half of the LSP feature and the only part of it a user
 * ever names.  It knows what a buffer, a window and the echo area are, and
 * it knows nothing about clangd, ty, JSON-RPC or framing: it asks
 * src/lsp_server.h for a client, src/lsp_sync.h to bring the document up to
 * date, and src/lsp_client.h to carry one request.  The answer arrives in a
 * callback run from lsp_poll(), so the command itself returns the moment it
 * has sent the question -- an editor that blocked on a language server
 * would be an editor that hangs whenever one is slow.
 *
 * Both entry points exist in every build.  A WITH_LSP=0 kg has the command
 * and the binding and reports that the feature was not compiled in, which
 * is a better answer than an unknown command for something the manual
 * documents.
 */

struct lsp_json_value;

/* Where the server says something is.  `line` and `character` are the
 * protocol's own numbers: zero-based, and `character` counted in whatever
 * encoding the handshake settled (src/lsp_sync.h converts it), which is why
 * it is not called a column -- it is not one until it has been decoded
 * against the target row's bytes. */
struct xref_location {
	char path[PATH_MAX];
	int line;
	long long character;
};

/* Read one element of a `textDocument/definition` result into `out`.  The
 * protocol allows three shapes for the same answer and a server picks one:
 *
 *   Location      { uri, range }
 *   LocationLink  { targetUri, targetSelectionRange | targetRange }
 *
 * -- the selection range being preferred where both are offered, since it
 * names the identifier rather than the whole body of what was defined.
 *
 * Returns false, having written nothing worth reading, for a node that is
 * not one of those, for a URI that is not a `file:` one kg can turn back
 * into a path (src/lsp_uri.h), and for a negative line.  Public, and pure,
 * because it is the part of this module worth a unit test: everything else
 * here needs an editor and a server. */
bool xref_location_of(
    const struct lsp_json_value *v, struct xref_location *out);

/* The results buffer.  Named here rather than spelled again in kbd.c
 * because the mode map that gives it RET, n, p and q is keyed on the name:
 * two spellings of it would be two chances for one of them to be wrong. */
#define XREF_BUFFER_NAME "*xref*"

/* `M-x xref-find-definitions` (M-.).  Sends the request and returns; the
 * echo area says what happened, twice -- once when the question goes out
 * and once when the answer comes back.  One definition is visited; a server
 * that offers a choice gets the same listing references get. */
void editor_xref_find_definitions(int fd);

/* `M-x xref-find-references` (M-?).  Always lists, even for one result:
 * "where is this used" is answered by the set, and a command that sometimes
 * jumps and sometimes lists is one whose next keystroke a user cannot
 * predict. */
void editor_xref_find_references(int fd);

/* `M-x xref-goto-xref` (RET in *xref*).  Visits the result on the line
 * point is on; says so and does nothing anywhere else.  The place it left
 * goes on the go-back stack, and so -- once, on the first visit out of a
 * listing -- does the place the search was started from. */
void editor_xref_goto_xref(int fd);

/* `M-x xref-go-back` (M-,).  Returns to where the newest jump started and
 * forgets it, so pressing it again walks further back.
 *
 * A departure point whose buffer has been killed is not a place to return
 * to: such entries are dropped on the way past rather than reported, and
 * the command lands on the newest one that is still there.  An empty stack
 * -- or one holding nothing but dead entries -- says "No xref history". */
void editor_xref_go_back(int fd);

/* How many departure points are on the go-back stack.  This is how a test
 * asks whether a jump remembered where it came from, and that the stack is
 * bounded; no policy reads it.  It counts what is stored, including entries
 * whose buffer has since died -- those are noticed by
 * `xref-go-back`, which is the only thing that pops them. */
size_t xref_go_back_depth(void);

#endif /* KG_XREF_H */
