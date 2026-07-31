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

#endif /* KG_EDIT_H */
