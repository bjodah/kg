/* gitdiag.h — git-mode diagnostics: what a commit message or a rebase
 * todo says that git will not accept.
 *
 * Two rules, both inherited unchanged from the scanners that used to
 * paint them (src/syntax_legacy.c's gitcommit_syntax()/gitrebase_syntax()):
 *
 *   - the part of a commit subject line past column 50, which is the
 *     length convention git tooling everywhere assumes;
 *   - a git-rebase-todo word git will reject -- an action word that names
 *     no action, and an option flag on an action that cannot take one --
 *     either of which fails the whole rebase rather than one line.
 *
 * They are diagnostics about the text, not a reading of it, which is why
 * they are here and not in a highlighting backend: a backend is chosen at
 * build time (`WITH_TREE_SITTER`), and a warning that only one of them
 * knows how to paint is a warning that disappears when the build changes.
 * Everything below is backend-independent, and it publishes through
 * src/decor.h, which display.c already lets override syntax faces
 * whatever painted them.
 *
 * Two halves, in showparen.h's shape.  The first two functions are pure:
 * they read one row, allocate nothing, touch no editor state, and say
 * what would be marked.  gitdiag_update() is the editor side, and the
 * only thing that creates a decoration.
 */

#ifndef KG_GITDIAG_H
#define KG_GITDIAG_H

typedef struct erow erow;
struct editor_buffer;
struct kg_display_options;

/* Columns of subject a commit message gets before the rest is marked.
 * Emacs' git-commit-mode calls the same number
 * `git-commit-summary-max-length'; git's own tooling, and every commit
 * convention built on it, assume it too. */
#define KG_GITDIAG_SUBJECT_LIMIT 50

/* Diagnosable words one rebase todo line may report.  A line that spells
 * more than this many impossible option flags is already one git will
 * refuse on the first of them; the cap is what keeps the pure half free
 * of an allocator, not a judgement about the ninth flag. */
#define KG_GITDIAG_ROW_SPANS_MAX 8

/* Live diagnostic decorations, over all buffers at once.  Same
 * discipline, one level up: a fixed store means this module never
 * allocates, and a buffer whose todo list has more diagnosable words than
 * this shows the first KG_GITDIAG_MAX_DECOR of them. */
#define KG_GITDIAG_MAX_DECOR 64

/* A half-open span of one row, in doc/coordinates.md's **chars** space --
 * byte offsets into `row->chars` -- because a chars offset is what
 * buffer_row_col_to_position() turns into the buffer position a
 * decoration is anchored by. */
struct kg_gitdiag_span {
	int start;
	int end;
};

/* The overlong tail of the commit subject `row`, or 0 when it is within
 * the limit.  The limit counts *rendered* bytes, which is where the
 * legacy scanner counted them: a TAB is worth its expansion, exactly as
 * the eye reads it on screen.  The span it hands back is chars space all
 * the same, converted once here.  The caller decides which row is the
 * subject (syntax_buffer_git_commit_subject()); this function believes
 * it. */
[[nodiscard]] int gitdiag_commit_subject_span(erow *row,
    const struct kg_display_options *options, struct kg_gitdiag_span *out);

/* Every word of the rebase todo line `line[0..len)` that git would
 * reject, in order, into `out[0..max)`; returns how many were written.
 * Chars space in and out: the grammar is whitespace-delimited words, and
 * a TAB delimits in either space, so nothing here needs the rendered
 * line.  A comment, a blank line and a line whose first word names a
 * known action with acceptable flags all report none. */
[[nodiscard]] int gitdiag_rebase_row_spans(
    const char *line, int len, struct kg_gitdiag_span *out, int max);

/* Bring `b`'s diagnostics up to date with its text, its mode and this
 * module's previous answer: retire what was published for `b`, and
 * publish what the two rules above say now.  A buffer in neither git mode
 * is left with none, which is also how a mode change out of one is
 * undone.
 *
 * Called from the repaint (src/display.c), once per frame per displayed
 * buffer, rather than from each of the four places that publish rows or
 * set a mode -- the same argument show_paren_update() is called on that
 * seam for.  The work is skipped outright unless the buffer's content
 * generation or mode has moved since the last publication, so a frame
 * that changed nothing costs a comparison. */
void gitdiag_update(struct editor_buffer *b);

#endif /* KG_GITDIAG_H */
