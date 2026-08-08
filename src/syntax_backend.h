#ifndef KG_SYNTAX_BACKEND_H
#define KG_SYNTAX_BACKEND_H

/* The private contract between the common syntax facade (src/syntax.c) and
 * whichever highlighting backend is compiled in (today src/syntax_legacy.c,
 * the bespoke row scanners).  Nothing outside those two files includes this
 * header: editor modules speak src/syntax.h, which says what a mode is and
 * what a row's hl[] bytes mean, and never which backend filled them in.
 *
 * Both directions are declared here.  The backend provides
 * syntax_backend_update_row(); the facade provides the common services
 * below, so a backend that needs to re-run a row, or to ask what a
 * git-rebase action word is, does not need its own copy. */

/* Rows are spelled with the struct tag rather than def.h's `erow` typedef
 * so this header needs nothing from def.h at all. */
struct editor_buffer;
struct erow;
struct kg_syntax_edit;
struct kg_syntax_state;

/* ---- Provided by the backend ---- */

/* Derive whatever this backend keeps about a whole document, from a staged
 * buffer that already holds all of it: `staged->row[0 .. numrows)` are
 * rendered (kg_row_builder_render()) and `staged->syntax` is the mode the
 * finished buffer will be in.  The record is a throwaway the facade built
 * over rows that belong to no buffer yet, and prepare must leave it as it
 * found it -- a backend that wants to walk the document incrementally works
 * on its own copy.
 *
 * Colouring every row is part of the job: on return each row's hl and hl_oc
 * must be what the same text would settle at after publication, because
 * this is the only highlighting pass a load pays for.  A mode that owns its
 * own highlighter (struct editor_syntax::highlight -- dired) is not this
 * backend's to fontify; per-row work routed through the facade's
 * syntax_update_row_only() honours that automatically.
 *
 * Returns the state, which the caller owns; a backend that keeps none
 * returns NULL, so NULL is not a failure.  `*ok` is set to 1 on success and
 * to 0 when the pass ran out of memory, in which case nothing is returned
 * to free. */
struct kg_syntax_state *syntax_backend_prepare(
    struct editor_buffer *staged, int *ok);

/* Release a state syntax_backend_prepare() returned.  NULL is accepted --
 * it is what a backend that keeps no state returns for every buffer. */
void syntax_backend_state_free(struct kg_syntax_state *st);

/* Fill row->hl (and, for a backend that keeps cross-row state, row->hl_oc)
 * for one non-empty row.  The facade has already reserved row->hl for
 * row->rsize bytes and set every one of them to HL_NORMAL, so a backend
 * that recognizes nothing may simply return.
 *
 * Called only when b->syntax is non-NULL and carries no mode-owned
 * highlighter (struct editor_syntax::highlight): those two cases are the
 * facade's, not a backend's. */
void syntax_backend_update_row(struct editor_buffer *b, struct erow *row);

/* One completed edit transaction's whole highlighting cost, paid once after
 * kg_buffer_replace() has published the new rows and rendered them.  `edit`
 * is the transaction in doc/coordinates.md's buffer-byte and chars-space
 * coordinates (src/syntax.h's kg_syntax_edit).
 *
 * What the facade has already done, and what a backend may therefore
 * assume -- the whole of it, deliberately small:
 *   - `b`'s rows are published and rendered;
 *   - edit->start_point.row is a valid row index of `b`;
 *   - nothing else.  In particular no row's hl has been reserved or
 *     blanked yet, and edit->new_end_point.row may be past the last row
 *     (a deletion that took the tail of the buffer with it), so a backend
 *     that walks to it clamps for itself.
 * Every row a backend colours here goes through syntax_update_row_only()
 * below, which is what reserves and blanks that row's hl; nothing else
 * reserves it for the backend. */
void syntax_backend_after_edit(
    struct editor_buffer *b, const struct kg_syntax_edit *edit);

/* "Everything changed": the notification for a path that rewrites a buffer
 * without being able to say what it replaced, so a backend holding a parse
 * tree throws it away rather than trying to edit it.  Same facade
 * guarantees as syntax_backend_after_edit() minus the edit description, and
 * the same rule about routing per-row colouring through
 * syntax_update_row_only().
 *
 * It is also where a buffer acquires backend state it does not yet have: a
 * mode change releases whatever the old mode's backend state was and then
 * lands here (editor_set_syntax()), and a buffer whose rows were replaced
 * wholesale (kg_buffer_adopt_rows()) arrives with no state either. */
void syntax_backend_rebuild(struct editor_buffer *b);

/* ---- Provided by the common facade ---- */

/* Re-highlight one row without propagating the result downstream.  A
 * backend whose scan of row N changes what row N-1 should look like (the
 * Markdown setext underline) calls this rather than editor_update_syntax(),
 * which would recurse through the propagation loop. */
void syntax_update_row_only(struct editor_buffer *b, struct erow *row);

/* Re-highlight the rows below row `idx` until one of them comes out with
 * the cross-row state (row->hl_oc) it already had, which is where the
 * change stops being visible.  A service of the facade rather than of the
 * legacy backend because hl_oc is a row field the facade owns and clears;
 * a backend that keeps no per-row carry (tree-sitter) never calls it. */
void syntax_propagate_below(struct editor_buffer *b, int idx);

/* Index of the first byte at or after `from` that is not a space or tab. */
int syntax_git_rebase_skip_ws(const char *line, int len, int from);

/* Index just past the word starting at `from`. */
int syntax_git_rebase_skip_word(const char *line, int len, int from);

/* The canonical name of the git-rebase-todo action spelled by
 * word[0..len) -- long or single-letter abbreviation -- or NULL when it
 * names none.  On a hit *takes_commit says whether a commit hash follows
 * the action word.  The action table itself is command semantics and
 * stays in the facade: the C-c action keys need it whether or not a
 * highlighting backend does. */
const char *syntax_git_rebase_action_name(
    const char *word, int len, int *takes_commit);

#endif /* KG_SYNTAX_BACKEND_H */
