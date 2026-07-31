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

/* What an edit through kg_buffer_replace() or editor_row_replace_range()
 * should also do.  Zero is the ordinary user edit -- record undo, count
 * the buffer dirty -- and is what every command that is one user
 * operation passes. */
enum edit_option {
	/* The caller records its own undo step covering the whole
	 * operation, so this one must not push a second, finer record. */
	KG_EDIT_NO_UNDO = 1 << 0,
	/* This edit is undo putting a record back.  It records nothing --
	 * for the same reason KG_EDIT_NO_UNDO does not -- and it is a
	 * distinct option because the two must not be confused when the
	 * one flag they both replace, `suppress_undo`, is retired: a
	 * caller writing its own coarse record and undo replaying one are
	 * different situations that happen to want the same silence. */
	KG_EDIT_REPLAY = 1 << 1,
};

/* One replacement of a byte range by other bytes.  `begin_byte` and
 * `end_byte` are flat byte positions (see buffer_byte_length());
 * `end_byte` equal to `begin_byte` is an insertion, an empty replacement
 * is a deletion, and `replacement` must be non-NULL even when its length
 * is zero. */
struct kg_edit {
	struct editor_buffer *buffer;
	size_t begin_byte;
	size_t end_byte;
	const char *replacement;
	size_t replacement_len;
	unsigned options; /* bitwise OR of enum edit_option */
};

/* What the edit did, for a caller that has to describe it afterwards.
 * The generations bracket the commit: they differ by exactly one when
 * the edit changed bytes, and are equal when it was refused. */
struct kg_edit_result {
	size_t old_length;
	size_t new_length;
	uint64_t before_generation;
	uint64_t after_generation;
};

int kg_buffer_replace(const struct kg_edit *e, struct kg_edit_result *out);

#endif /* KG_EDIT_H */
