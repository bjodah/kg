/* lsp_diag.h — what the servers said about the files, unasked
 *
 * Diagnostics are the one part of the protocol kg does not ask for: a
 * server publishes `textDocument/publishDiagnostics` whenever it has an
 * opinion, and the client layer hands it here through its notification
 * hook (src/lsp_client.h).  So this module is a store first and a command
 * second -- the listing and the in-buffer marks are two views of the same
 * table, and neither of them is what makes a diagnostic arrive.
 *
 * The store is keyed on the FILE, not on the buffer and not on the client.
 * A publish names a URI; the path behind it may have no buffer at all, may
 * have one that is about to be killed, and may be reported on by two
 * servers at once.  Keying on the path is what lets a diagnostic outlive
 * every one of those, and it is what makes "replace what this file had"
 * -- the protocol's own semantics for a publish -- one memcpy of policy
 * rather than a diff.
 *
 * Positions arrive in the encoding the handshake settled (UTF-16 unless
 * the server took kg's utf-8 offer) and are stored exactly as they
 * arrived, for src/xref.c's reason in the other direction: a character
 * offset can only be turned into a byte column against the bytes of the
 * target row, and this module does not have those until a buffer visits
 * the file.  The conversion therefore happens at paint time, against
 * whatever the row says then.
 *
 * The in-buffer marks are decorations (src/decor.h) and not syntax: a
 * diagnostic is not a property of the text, it is what one server thought
 * about it a moment ago, and the next publish is entitled to take it back.
 */

#ifndef KG_LSP_DIAG_H
#define KG_LSP_DIAG_H

#include <stddef.h>

#include "lsp_client.h" /* enum lsp_position_encoding */

/* Declared, not included: src/json.h defines it, and the one caller
 * that has one to pass has already included that. */
struct kg_json_value;

/* The listing buffer.  Named here rather than spelled again in kbd.c, for
 * the reason OCCUR_BUFFER_NAME is: the mode map that gives it RET, n, p
 * and q is keyed on the name, and two spellings of it would be two chances
 * for one of them to be wrong. */
#define LSP_DIAG_BUFFER_NAME "*Diagnostics*"

/* Bounds, and both of them are the same promise the rest of the client
 * makes: a talkative server costs a cap, not the address space.
 *
 * Eight files is the working set of a session -- every file a server has
 * had an opinion about since kg started -- and a ninth is refused rather
 * than evicted while any of the eight still has a diagnostic in it; an
 * empty entry is reclaimed first, which is what makes a fixed file
 * publishing `[]` free its slot.  Sixty-four diagnostics in one file is
 * well past the point where a reader stops reading and starts fixing the
 * first one, and the listing says when it cut.  A message is one line of
 * a listing, so it is bounded like every other line kg builds one from. */
#define LSP_DIAG_MAX_FILES 8
#define LSP_DIAG_MAX_PER_FILE 64
#define LSP_DIAG_MESSAGE_MAX 200

/* The protocol's own DiagnosticSeverity numbering, so a server's number
 * needs no translation to land here.  Anything else a server sends is
 * kept as it arrived and named "diagnostic" in the listing: an unknown
 * severity is not a reason to drop what it was about. */
enum lsp_diag_severity {
	LSP_DIAG_ERROR = 1,
	LSP_DIAG_WARNING = 2,
	LSP_DIAG_INFORMATION = 3,
	LSP_DIAG_HINT = 4,
};

/* Wire this module up, once, from init_editor(): the client layer's
 * notification hook, and a KG_EVENT_BUFFER_OPENED subscriber that paints
 * whatever is already known about a file the moment a buffer visits it.
 *
 * Installed by the editor rather than from behind the LSP facade, exactly
 * as src/lsp_log.h is and for the same reason: this half reaches buffers,
 * so it is not something every test binary linking the protocol modules
 * should have to link too.  Calling it more than once is safe and leaves
 * exactly one subscriber registered. */
void lsp_diag_install(void);

/* The whole of what a `textDocument/publishDiagnostics` notification does
 * -- read it, replace what that file had, repaint every buffer visiting it
 * -- minus the client it arrived on, which contributes only `enc`.
 *
 * A publish carrying a `version` older than the one already stored for
 * that file is dropped whole: the server is describing text that has since
 * been sent to it again, and painting it would put marks where the
 * characters they were measured against no longer are.  A publish with no
 * `version` at all is always taken, which is what the protocol says a
 * server that does not track versions means by omitting it.
 *
 * Public because it is the interesting half and a unit test can drive it
 * with a parsed document and no server in the way. */
void lsp_diag_publish(
    const struct kg_json_value *params, enum lsp_position_encoding enc);

/* `M-x lsp-diagnostics`.  Brings the current buffer to its server's
 * attention (which is what makes a first publish happen at all -- kg opens
 * a document lazily, on the first command that needs one) and lists
 * everything known, in `path:line:col: severity: message` order by file
 * and position, in a read-only listing shown in another window.  Taking
 * the next-error keys with it, the way occur does.
 *
 * No key binding.  Emacs reaches its flymake listing through a `C-c !`
 * prefix kg has no notion of, and `C-c <key>` is reserved for the user's
 * own bindings here (src/keybind.h), so M-x is the whole of it. */
void editor_lsp_diagnostics(int fd);

/* `M-x lsp-diagnostics-goto` (RET in the listing).  Visits the diagnostic
 * the row point is on, leaving the listing's window the way xref and occur
 * do; says so and does nothing anywhere else. */
void editor_lsp_diagnostics_goto(int fd);

/* ---- Test-only introspection of the store ----
 *
 * Nothing outside test/ calls these.  They report what a publish left
 * behind, in the order the listing would print it, without needing a
 * window or a server.  lsp_diag_test_reset() empties the store and drops
 * every decoration it painted, which is how one case stops being the next
 * one's fixture. */
size_t lsp_diag_test_row_count(void);
bool lsp_diag_test_row(size_t index, const char **out_path, int *out_line,
    long long *out_character, int *out_severity, const char **out_message);
void lsp_diag_test_reset(void);

#endif /* KG_LSP_DIAG_H */
