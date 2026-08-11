#ifndef KG_LSP_COMPLETE_H
#define KG_LSP_COMPLETE_H

#include <stddef.h>

/* `M-x completion-at-point` (M-TAB): complete the symbol before point
 * from what the language server offers -- Emacs' completion-at-point,
 * whose terminal-safe binding this is (a terminal sends C-M-i and M-TAB
 * as the same two bytes, ESC and TAB, so kg spells the key M-TAB and the
 * two are one key here as they are in Emacs).
 *
 * The shape is Emacs': one candidate is inserted, several with more text
 * in common than has been typed extend the text by what they share, and
 * several with nothing left in common are listed in a read-only
 * `*Completions*` buffer while point stays where it was.
 *
 * Compiled in both configurations, the way src/xref.c is: the command
 * table's row and the M-TAB binding are unconditional, so a WITH_LSP=0
 * kg answers the key with the reason there is no answer.
 */

/* How many candidates one answer keeps.  A server asked to complete an
 * empty prefix answers with everything it knows -- thousands of items --
 * and a listing nobody scrolls to the end of is not worth that many
 * copies.  The cap is visible in the listing when it is reached. */
#define LSP_COMPLETE_MAX_ITEMS 200

/* The listing buffer.  Named here rather than spelled again elsewhere,
 * for the reason XREF_BUFFER_NAME is. */
#define LSP_COMPLETE_BUFFER_NAME "*Completions*"

/* One candidate.  `insert` is what completing it puts in the buffer,
 * which is not always the label: a server may offer `insertText` (or a
 * `textEdit`) that differs from what it wants shown.  `sort_key` is the
 * server's own `sortText`, or the label when it sent none -- the
 * protocol's ordering rule, spelled out.  `detail` is a one-line note
 * shown in the listing, or NULL.
 *
 * Every pointer is owned by whoever built the array; nothing below
 * frees one. */
struct lsp_complete_item {
	char *label;
	char *insert;
	char *sort_key;
	char *detail;
};

/* Order candidates the way the protocol says a client must: by
 * `sortText`, then by label, so two items a server gave one sort key are
 * still shown in a stable order a reader can predict. */
void lsp_complete_sort(struct lsp_complete_item *items, size_t n);

/* Whether `text` is a completion of the `len` bytes at `prefix`.  Plain
 * byte comparison: kg's completion is exact-case, like its dabbrev and
 * unlike Emacs' completion-ignore-case default, and a server that
 * returned an unfiltered set is filtered here. */
bool lsp_complete_matches(const char *text, const char *prefix, size_t len);

/* The length in bytes of the longest common prefix of `n` strings, never
 * splitting a UTF-8 character: what completing them all at once may
 * safely insert.  Zero for no strings, and the whole string for one. */
size_t lsp_complete_common_prefix(const char *const *texts, size_t n);

void editor_completion_at_point(int fd);

/* Once per processed keystroke, from the main loop's post-command seam:
 * take the listing down when point has left what the listing is a listing
 * of.
 *
 * Emacs' rule, measured against 31.0.90 rather than assumed.  Its
 * `completion-in-region-mode' survives a keystroke when point is still at
 * or after the start of the region being completed, still on that
 * region's line, and the completion function asked afresh still says the
 * region begins where it began; otherwise the mode exits and takes
 * *Completions* down with it.  So typing more of the same symbol leaves
 * the listing up (stale -- Emacs does not recompute it either), and a
 * space, a C-a, or a move to another line closes it.  Repeated M-TAB
 * keeps it and replaces it when the next answer lands.
 *
 * kg's stand-in for "ask the completion function again" is the word scan
 * M-/ uses (src/dabbrev.h), which is what the fallback target is measured
 * with in the first place, and which costs no round trip to the server:
 * a listing must not depend on one to know it is over.
 *
 * C in both configurations rather than in shipped Lisp: a WITH_LISP=0 kg
 * has no post-command-hook to hang this on, and a listing that lingers in
 * one build and not the other is a difference between builds nobody asked
 * for.  A no-op with LSP compiled out, where there is no listing. */
void lsp_complete_post_command(void);

#endif /* KG_LSP_COMPLETE_H */
