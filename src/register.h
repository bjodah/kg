/* register.h — the bounded register table behind C-x r
 *
 * A register is a named place to put one of two things: a position in a
 * buffer (`point-to-register`, jumped back to with `jump-to-register`) or
 * a span of text (`copy-to-register`, put back with `insert-register`).
 * The name is a single byte the command reads from the keyboard, so this
 * table is keyed by that byte rather than by a slot index the user would
 * have to remember.
 *
 * Everything here is bounded by construction.  There are
 * KG_REGISTER_SLOTS slots and no more: a name that is not already stored
 * takes a free slot, and a table with none left refuses rather than
 * evicting somebody else's register -- a register is named by the user,
 * so the kill ring's "drop the oldest" rule would silently discard the
 * one thing they asked to keep.  Text is capped per register and across
 * the whole table, refusing an oversize store the same way, and an
 * allocation failure leaves the table exactly as it was.
 *
 * A position register stores a buffer handle plus a marker, never a slot
 * index or a line number: the marker is what keeps the position pointing
 * at the same text after edits, and the handle is what makes a killed
 * buffer report a stale register instead of jumping into whatever now
 * occupies its old slot.  Neither is re-derived at jump time; both are
 * resolved and refused on the spot.
 *
 * This module stores registers.  It reads no keys and prints no
 * messages: the commands that will read a name from the keyboard and
 * report what happened are the next slice, and the wording of every
 * refusal already lives here, in kg_register_result_message(), so they
 * all report the same condition the same way.
 */

#ifndef KG_REGISTER_H
#define KG_REGISTER_H

#include <stddef.h>

struct editor_buffer;
struct kg_buffer_handle;

/* How many registers may be stored at once, how many bytes one text
 * register may hold, and how many bytes every text register may hold
 * together. */
#define KG_REGISTER_SLOTS 32
#define KG_REGISTER_TEXT_MAX ((size_t)1024 * 1024)
#define KG_REGISTER_TEXT_TOTAL_MAX ((size_t)4 * 1024 * 1024)

enum kg_register_kind {
	KG_REGISTER_EMPTY = 0,
	KG_REGISTER_POSITION,
	KG_REGISTER_TEXT,
};

enum kg_register_result {
	KG_REGISTER_OK = 0,
	/* The name is not a byte this table accepts (see
	 * kg_register_name_valid()). */
	KG_REGISTER_BAD_NAME,
	/* Nothing has ever been stored under this name. */
	KG_REGISTER_UNSET,
	/* Something has, but not of the kind that was asked for. */
	KG_REGISTER_WRONG_KIND,
	/* A position whose buffer or marker is gone. */
	KG_REGISTER_STALE,
	/* Every slot is taken and none of them is this name. */
	KG_REGISTER_FULL,
	/* One entry over KG_REGISTER_TEXT_MAX. */
	KG_REGISTER_TOO_BIG,
	/* This entry fits, the table's total would not. */
	KG_REGISTER_NO_ROOM,
	/* Allocation failed; the table is unchanged. */
	KG_REGISTER_NOMEM,
};

/* Whether `name` may name a register: one printable, non-space ASCII
 * byte.  Deliberately narrower than Emacs, which takes any key event:
 * kg reads the name as a single raw byte, so anything outside this range
 * is either a control key the user meant as an escape or the first byte
 * of a multi-byte sequence this table would store half of. */
bool kg_register_name_valid(int name);

/* What is stored under `name` -- KG_REGISTER_EMPTY for an invalid name
 * as well as an unused one, since neither holds anything. */
enum kg_register_kind kg_register_kind_at(int name);

/* Store the position `pos` in buffer `b` under `name`, replacing
 * whatever was there.  Creates the marker; KG_REGISTER_NOMEM if that
 * fails, in which case the previous contents are left alone. */
enum kg_register_result kg_register_set_position(
    int name, struct editor_buffer *b, size_t pos);

/* The buffer and byte position stored under `name`.  KG_REGISTER_STALE
 * when the buffer has been killed or the marker no longer resolves,
 * which leaves the register in place -- a stale register keeps saying
 * so, and never turns into a position in a reused buffer slot. */
enum kg_register_result kg_register_get_position(
    int name, struct kg_buffer_handle *out_buffer, size_t *out_pos);

/* Store `len` bytes of `text` under `name`, replacing whatever was
 * there.  The bytes are copied exactly, embedded NULs included; `len` is
 * authoritative.  On any refusal (bad name, full table, oversize entry,
 * no room, allocation failure) the table is bit-identical to before the
 * call, including the register `name` already had. */
enum kg_register_result kg_register_set_text(
    int name, const char *text, size_t len);

/* The bytes stored under `name`.  `*out_text` stays valid until the next
 * call that changes this register; the span is followed by one defensive
 * NUL that `*out_len` does not count. */
enum kg_register_result kg_register_get_text(
    int name, const char **out_text, size_t *out_len);

/* Empty the whole table, freeing every text register and deleting every
 * position register's marker. */
void kg_register_clear(void);

/* How many slots are in use, and how many bytes the text registers hold
 * together.  For the native tests and for `describe`-style reporting. */
size_t kg_register_used(void);
size_t kg_register_text_bytes(void);

/* One sentence naming what went wrong, for a command to put in the echo
 * area.  "" for KG_REGISTER_OK. */
const char *kg_register_result_message(enum kg_register_result result);

/* ---- The interactive commands ----
 *
 * Each prompts, reads one key as the register name, and reports through
 * kg_register_result_message().  Bound through the keymap only (C-x r
 * SPC and C-x r j -- see src/kbd.c) and reachable from M-x.
 *
 * copy-to-register stores the region's exact bytes; insert-register
 * puts them back at point as one edit, and so as one undo step, with
 * the mark left where the insertion began the way a yank leaves it.
 * A read-only buffer refuses insert-register through its command
 * descriptor's CMD_EDITS_BUFFER, which is the editor's one read-only
 * verdict -- this module does not ask again.
 *
 * jump-to-register moves point in the selected window, switching it to
 * the register's buffer when that is a different one: a register names a
 * buffer and a position, never a window, so which window shows it is
 * whichever one the user is in -- the same policy as any other buffer
 * switch, and deliberately not a saved window configuration, which is a
 * separate Emacs register kind kg does not have. */
void editor_point_to_register(int fd);
void editor_jump_to_register(int fd);
void editor_copy_to_register(int fd);
void editor_insert_register(int fd);

/* Test-only allocation-failure seam for this module's text storage --
 * distinct from kg_yank_fail_alloc_after() in yank.h and
 * kg_decor_fail_alloc_after() in decor.h, one seam per allocator.  Lets
 * the next `n` allocations succeed and fails the one after that, then
 * disarms itself; a negative `n` disarms it immediately, which is how
 * the editor always runs.  Nothing outside test/ calls this. */
void kg_register_fail_alloc_after(int n);

#endif /* KG_REGISTER_H */
