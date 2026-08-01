#ifndef KG_EDIT_H
#define KG_EDIT_H

#include <stddef.h>
#include <stdint.h>

/* The edit transaction: one replacement of a byte range of a live buffer
 * by other bytes, which is the whole of what a command may do to a
 * buffer's text.  Follow-up Plan 02 is the migration of every remaining
 * hand-written mutation onto it.
 *
 * Forward declaration; the full struct is defined in def.h.  buffer.c
 * includes def.h to reach its members. */
struct editor_buffer;
struct editor_syntax;
typedef struct erow erow;

/* Why an edit is being made, which is the whole of what its policy
 * depends on.  An intent rather than a flag word, because the flag word
 * made one answer stand for several: KG_EDIT_NO_UNDO said "I write my
 * own undo record", and the transaction read it as "and therefore
 * read-only does not apply", which is not the same statement and was
 * never true of a command.
 *
 * The transaction asks three questions, and each intent answers all of
 * them:
 *   undo      -- does the transaction write the record, or does the
 *                caller own one covering more than this edit?
 *   authority -- is a read-only buffer's refusal this edit's business?
 *   modified  -- does the buffer become modified, or was its text never
 *                the user's to modify?
 * `content_generation` advances for every intent that changes bytes,
 * including one that deliberately leaves `dirty` at zero: "did the text
 * change" and "has the user unsaved work" are different questions.
 *
 * A fourth question -- ordinary change event or broad replacement -- is
 * follow-up Plan 03's, and arrives with the event queue that would carry
 * it; KG_EDIT_INTERNAL already names the case it would distinguish.  A
 * committed reload, which resets a buffer to a *new* clean state instead
 * of preserving the one it had, is Plan 02 phase 4's and gets its own
 * constructor there. */
enum kg_edit_intent {
	/* One whole user operation: records undo, refused on a read-only
	 * buffer, counts the buffer modified. */
	KG_EDIT_USER,
	/* Part of one user operation whose undo record the caller writes
	 * itself, covering the whole of it.  Refused on a read-only
	 * buffer like anything else the user asked for. */
	KG_EDIT_USER_PART,
	/* Undo putting a record back.  Records nothing, and read-only is
	 * not what it is about: the record predates the flag, and the
	 * buffer is being returned to a state it already had. */
	KG_EDIT_REPLAY,
	/* An owner rewriting text that was never the user's: a listing, a
	 * process's output, a rebuild.  Records nothing, is not subject
	 * to the read-only flag that presents such a buffer, and leaves
	 * the buffer as clean as it found it -- while still advancing the
	 * content generation, because the bytes did change. */
	KG_EDIT_INTERNAL,
};

/* One replacement of a byte range by other bytes.  `begin_byte` and
 * `end_byte` are flat byte positions (see buffer_byte_length());
 * `end_byte` equal to `begin_byte` is an insertion, an empty replacement
 * is a deletion, and `replacement` must be non-NULL even when its length
 * is zero.  Build one with a constructor below rather than by hand. */
struct kg_edit {
	struct editor_buffer *buffer;
	size_t begin_byte;
	size_t end_byte;
	const char *replacement;
	size_t replacement_len;
	enum kg_edit_intent intent;
};

/* The four ways to ask for an edit.  Same five arguments each; the name
 * is the policy, so there is no combination to get wrong at a call
 * site and no default to forget. */
struct kg_edit kg_edit_user(struct editor_buffer *b, size_t begin, size_t end,
    const char *replacement, size_t replacement_len);
struct kg_edit kg_edit_user_part(struct editor_buffer *b, size_t begin,
    size_t end, const char *replacement, size_t replacement_len);
struct kg_edit kg_edit_replay(struct editor_buffer *b, size_t begin, size_t end,
    const char *replacement, size_t replacement_len);
struct kg_edit kg_edit_internal(struct editor_buffer *b, size_t begin,
    size_t end, const char *replacement, size_t replacement_len);

/* What the edit did, for a caller that has to describe it afterwards.
 * The generations bracket the commit: they differ by exactly one when
 * the edit changed bytes, and are equal when it was refused. */
struct kg_edit_result {
	size_t old_length;
	size_t new_length;
	uint64_t before_generation;
	uint64_t after_generation;
};

/* Replace the bytes [begin_byte, end_byte) of `e->buffer` by
 * `e->replacement`, as one step.  Returns 0 with the buffer
 * byte-identical when the range is not one the buffer has, the intent
 * has no authority over a read-only buffer, or memory runs out.
 *
 * Replacing bytes with the same bytes is a well-formed request that
 * changes nothing, so it succeeds having recorded no undo, left the
 * buffer as modified as it was and left the content generation alone --
 * there is no change for a later reader to be told about. */
int kg_buffer_replace(const struct kg_edit *e, struct kg_edit_result *out);

/* The same replacement addressed inside one row of the current buffer:
 * the `delete_len` bytes at (filerow, at) become the `insert_len` bytes
 * at `insert`.  Row bounds are checked here, which is the checking a
 * flat position cannot do -- a column past the end of a row is refused,
 * where a flat position past the end of the buffer clamps. */
int editor_row_replace_range(int filerow, int at, int delete_len,
    const char *insert, int insert_len, enum kg_edit_intent intent);

/* Insert `text` at point as one user edit and leave point after it: one
 * undo record, and one row rebuild per row the result has.  The commands
 * that put a run of text into the current buffer -- a newline and its
 * auto-indent, a yank, a repeated yank -- are all this call. */
void editor_insert_text_at_point(const char *text, int len);

/* Test-only allocation-failure seam.  Lets the transaction's next `n`
 * allocations before publication succeed and fails the one after that,
 * then disarms itself; a negative `n` disarms it immediately, which is
 * how the editor always runs.  Nothing outside test/ calls this.
 *
 * It exists because refusal atomicity is a promise no ordinary input can
 * exercise: a bogus range is refused before anything is staged, so it
 * proves nothing about the state a half-staged edit leaves behind.  The
 * allocations it counts are the ones the transaction makes before it
 * publishes anything -- the replaced bytes, the replacement rows, the
 * row array's growth and the undo record. */
void kg_edit_fail_alloc_after(int n);

/* ---- The staged row builder --------------------------------------------
 * A second way to make rows, alongside the byte-range replacement above:
 * one for editing text already in a buffer, one for building a buffer's
 * *whole* content before it has one.  A file load, a directory listing, a
 * rebuilt *Buffer List* or *help* screen all start from nothing, already
 * know the syntax and rendering they want, and would gain nothing from
 * being expressed as a replacement of the old content by the new -- the
 * old content is not being edited, it is being discarded.
 *
 * Raw row construction (editor_insert_row() and friends, declared in
 * def.h) is legal here because these rows belong to no buffer yet: they
 * are not observable, so nothing can be lost by racing a partial rebuild
 * against a failure, and nothing has to unwind.  That stops being true
 * the moment they are attached to a buffer, which is what
 * kg_buffer_adopt_rows() does, once, deliberately, as the seam's only
 * publication point. */

/* Append one row of `len` bytes to `*rows`, growing `*rows`/`*row_capacity`
 * the same way the live row array does (editor_rows_reserve()), so
 * building R rows costs O(log R) reallocations rather than R of them.
 * Returns 1, or 0 with errno set (ENOMEM, or EOVERFLOW when `len` or the
 * resulting row count cannot fit where a row counts them) and `*rows`
 * unchanged. */
int kg_row_builder_add_line(erow **rows, int *numrows, int *row_capacity,
    const char *s, size_t len);

/* Render and syntax-highlight every row of a staged, unpublished array, in
 * the order they appear.  `numrows` is announced to the highlighter one
 * row at a time as the pass goes, rather than up front: a highlighter
 * that carries state into the next row (an open block comment) must not
 * be let propagate into rows this pass has not reached yet, which have no
 * render and would be misread as empty.  Returns 0, or -1 with errno set
 * (ENOMEM, or the editor shutting down mid-pass) leaving whatever
 * rendered so far as it is -- the caller's abandon path
 * (kg_row_builder_free()) does not care how far a failed pass got. */
int kg_row_builder_highlight(
    erow *rows, int numrows, struct editor_syntax *syntax);

/* Free a staged row array exactly as kg_row_builder_add_line() built it:
 * every row's owned storage, then the array, then the triple that
 * described it.  The counterpart to a staged rebuild that is abandoned
 * rather than adopted. */
void kg_row_builder_free(erow **rows, int *numrows, int *row_capacity);

/* Publish a freshly built row array as `b`'s whole content, discarding
 * whatever rows `b` held.  `*rows`/`*numrows`/`*row_capacity` are taken
 * over -- on return they describe an empty, still-valid staged array
 * (like a fresh kg_row_builder), never `b`'s previous content, so the
 * same variables may be reused or simply left to go out of scope.
 *
 * This is not kg_buffer_replace(): no undo record could restore content
 * that was never spliced in through it, so none is written, and `b` is
 * always left clean regardless of what it was -- a file load, a revert
 * and a listing rebuild all want a new baseline, not a change to the old
 * one.  content_generation still advances, because the bytes did
 * change. */
void kg_buffer_adopt_rows(
    struct editor_buffer *b, erow **rows, int *numrows, int *row_capacity);

/* Append `text` to `b`'s last row, opening a new one at each embedded '\n'
 * -- what a compilation's or a shell command's output does to the special
 * buffer that displays it, arriving a chunk at a time rather than as one
 * finished file.  Opens a first empty row when `b` has none.  No undo
 * record is written for output that was never the user's to type, and the
 * buffer is left clean regardless of what it held: `b->dirty` is reset
 * once this returns, whatever the row primitives underneath did to it
 * along the way. */
void kg_buffer_append_internal(
    struct editor_buffer *b, const char *text, size_t len);

#endif /* KG_EDIT_H */
