/* undo.c - Simple undo/redo functionality */

#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "localvars.h"
#include "perf.h"

/* Initialize an undo stack.  clean_size starts at 0, not -1: a stack is
 * only ever initialized for a buffer that is at its saved state (freshly
 * loaded, freshly created, or rebuilt from scratch), so popping back to
 * zero records is popping back to that state. */
void undo_stack_init(struct undo_stack *st)
{
	st->head = NULL;
	st->size = 0;
	st->max_size = MAX_UNDO_SIZE;
	st->clean_size = 0;
}

void undo_init(void) { undo_stack_init(&bcur()->undostack); }

/* Drop every record in `st`.  Buffers that are not the current one are
 * rebuilt by background producers, so this takes the stack rather than
 * reaching for bcur(): the two callers that do mean the current buffer
 * say so through undo_free(). */
void undo_stack_free(struct undo_stack *st)
{
	struct undo_op *op = st->head;

	while (op) {
		struct undo_op *next = op->next;
		if (op->text) {
			free(op->text);
		}
		free(op);
		op = next;
	}
	st->head = NULL;
	st->size = 0;
	/* The records that led back to the saved state are gone, so no
	 * amount of undoing reaches it; -1 is "unreachable". */
	st->clean_size = -1;
}

void undo_free(void) { undo_stack_free(&bcur()->undostack); }

static void undo_stack_add(struct undo_stack *st, struct undo_op *op);

/* Whether a record pushed now would be kept.  The ambient flag has three
 * meanings and every one of them says "not this edit"; the two explicit
 * edit options that replace it are checked by the transaction, not here.
 * One reader, so retiring the flag is one edit. */
static int undo_recording(void) { return !suppress_undo; }

/* A record of `type` owning its own copy of `len` bytes of `text`, or
 * NULL when either allocation fails.  The addressing fields are left to
 * the caller: the eleven row-addressed opcodes fill row/col, UNDO_CHANGE
 * fills position, and nothing fills both. */
static struct undo_op *undo_record_new(
    enum undo_type type, int c, char *text, int len)
{
	struct undo_op *op = malloc(sizeof(struct undo_op));
	int alloc_sz;

	if (!op) {
		return NULL;
	}
	op->type = type;
	op->row = 0;
	op->col = 0;
	op->position = 0;
	op->c = c;
	op->text = NULL;
	op->len = len > 0 ? len : 0;

	if (text && len > 0) {
		if (!checked_add_int_size(&alloc_sz, len, 1)) {
			free(op);
			return NULL;
		}
		op->text = malloc(alloc_sz);
		if (!op->text) {
			free(op);
			return NULL;
		}
		memcpy(op->text, text, len);
		op->text[len] = '\0';
	}
	return op;
}

/* Push an undo operation onto the stack */
int undo_push(struct editor_buffer *b, enum undo_type type, int row, int col,
    int c, char *text, int len)
{
	struct undo_stack *st = &b->undostack;
	struct undo_op *op;

	/* Skip if undo recording is suppressed */
	if (!undo_recording()) {
		return 1;
	}

	op = undo_record_new(type, c, text, len);
	if (!op) {
		return 0;
	}
	op->row = row;
	op->col = col;
	undo_stack_add(st, op);
	return 1;
}

/* Record one flat-position replacement: the `old_len` bytes of
 * `old_text` that were at `position` are now `new_len` other bytes.  The
 * reverse is that statement read backwards, which is why UNDO_CHANGE
 * needs no opcode of its own per kind of edit.  Returns 1 without
 * recording while undo is suppressed, as undo_push() does. */
int undo_push_change(struct editor_buffer *b, size_t position, char *old_text,
    int old_len, int new_len)
{
	struct undo_op *op;

	if (!undo_recording()) {
		return 1;
	}
	op = undo_record_new(UNDO_CHANGE, new_len, old_text, old_len);
	if (!op) {
		return 0;
	}
	op->position = position;
	undo_stack_add(&b->undostack, op);
	return 1;
}

/* Link `op` at the head and drop the oldest records once the stack is
 * over its limit. */
static void undo_stack_add(struct undo_stack *st, struct undo_op *op)
{
	/* Add to front of stack */
	op->next = st->head;
	st->head = op;
	st->size++;
	KG_PERF_INC(KG_PERF_UNDO_PUSH);

	/* Trim stack if too large */
	if (st->size > st->max_size) {
		struct undo_op *curr = st->head;
		struct undo_op *prev = NULL;
		int count = 0;

		/* Find the last operation to keep */
		while (curr && count < st->max_size - 1) {
			KG_PERF_INC(KG_PERF_UNDO_EVICT_LINKS);
			prev = curr;
			curr = curr->next;
			count++;
		}

		/* Free the rest */
		if (prev) {
			prev->next = NULL;
			/* clean_size counts records down to the saved
			 * state, so it only means anything while the
			 * bottom-most ones are still here.  Eviction takes
			 * them from exactly that end, and only ever after a
			 * later edit was pushed, so the checkpoint is now
			 * unreachable rather than merely renumbered. */
			st->clean_size = -1;
			while (curr) {
				struct undo_op *next = curr->next;
				if (curr->text) {
					free(curr->text);
				}
				free(curr);
				curr = next;
				st->size--;
			}
		}
	}
}

/* Reverse a rectangle operation.
 *
 * op->row  = first row affected
 * op->c    = numrows before the operation
 * op->text = original content of rows [row, row+N), '\n'-joined, where N
 *            is the number of lines in op->text (0 if empty)
 *
 * Trim back to the original row count, then restore each row.  The row
 * primitives it replays through would record their own steps, which is
 * what `suppress_undo` is silencing; phase 9 replaces both this and the
 * rectangle commands with one KG_EDIT_REPLAY transaction. */
static void undo_replay_rect_overwrite(struct undo_op *op)
{
	int orig_numrows = op->c;
	char *p = op->text;
	char *end = op->text ? op->text + op->len : NULL;
	int i = 0;

	suppress_undo = 1;
	while (bcur()->numrows > orig_numrows) {
		editor_del_row(bcur(), bcur()->numrows - 1);
	}
	while (op->text && op->len > 0 && p <= end) {
		char *nl = (p < end) ? memchr(p, '\n', end - p) : NULL;
		int line_len = nl ? (nl - p) : (end - p);
		int target = op->row + i;

		if (target < bcur()->numrows) {
			editor_del_row(bcur(), target);
		}
		editor_insert_row(bcur(), target, p, line_len);
		if (!nl) {
			break;
		}
		p = nl + 1;
		i++;
	}
	suppress_undo = 0;
	buffer_note_change(bcur());
}

/* Reverse a paragraph reflow.
 *
 * op->row  = paragraph start row
 * op->col  = number of reflowed rows to delete
 * op->text = the original lines, joined with '\n'
 *
 * Same shape, and the same `suppress_undo` region, as the rectangle
 * replay above. */
static void undo_replay_reflow_para(struct undo_op *op)
{
	char *line_start, *nl, *end;
	int r;

	suppress_undo = 1;
	for (r = 0; r < op->col; r++) {
		if (op->row < bcur()->numrows) {
			editor_del_row(bcur(), op->row);
		}
	}
	r = op->row;
	line_start = op->text;
	end = op->text ? op->text + op->len : NULL;
	while (line_start && line_start < end) {
		nl = memchr(line_start, '\n', end - line_start);
		editor_insert_row(bcur(), r++, line_start,
		    nl ? (size_t)(nl - line_start)
		       : (size_t)(end - line_start));
		line_start = nl ? nl + 1 : end;
	}
	suppress_undo = 0;
	buffer_note_change(bcur());
}

/* Perform undo operation */
void editor_undo(void)
{
	struct undo_stack *st = &bcur()->undostack;
	struct undo_op *op;

	if (!st->head) {
		editor_set_status_message("Nothing to undo");
		return;
	}

	op = st->head;
	st->head = op->next;
	st->size--;

	/* Position cursor at operation location.  UNDO_CHANGE is addressed
	 * by a byte position, not a row and column, and lands point itself
	 * once the text is back. */
	if (op->type != UNDO_CHANGE) {
		editor_cursor_goto(op->row, op->col);
	}

	/* Perform the reverse operation */
	switch (op->type) {
	case UNDO_CHANGE: {
		/* Reverse: put the saved bytes back where the replacement
		 * bytes now are.  Through the same primitive that made the
		 * change, in replay mode, so nothing is recorded and no
		 * second implementation of the splice exists to disagree. */
		struct kg_edit e = {
			.buffer = bcur(),
			.begin_byte = op->position,
			.end_byte = op->position + (size_t)op->c,
			.replacement = op->text ? op->text : "",
			.replacement_len = (size_t)op->len,
			.options = KG_EDIT_REPLAY,
		};
		int row, col;

		kg_buffer_replace(&e, NULL);
		buffer_position_to_row_col(bcur(), op->position, &row, &col);
		editor_cursor_goto(row, col);
		break;
	}

	case UNDO_INSERT_CHAR:
		/* Reverse: delete the character */
		if (op->row < bcur()->numrows) {
			erow *row = &bcur()->row[op->row];
			if (op->col < row->size) {
				editor_row_del_char(row, op->col);
				buffer_note_change(bcur());
			}
		}
		break;

	case UNDO_DELETE_CHAR:
		/* Reverse: insert the character */
		if (op->row < bcur()->numrows) {
			erow *row = &bcur()->row[op->row];
			editor_row_insert_char(row, op->col, op->c);
			buffer_note_change(bcur());
		}
		break;

	case UNDO_INSERT_LINE:
		/* Reverse: delete the line */
		if (op->row < bcur()->numrows) {
			editor_del_row(bcur(), op->row);
			buffer_note_change(bcur());
		}
		break;

	case UNDO_DELETE_LINE:
		/* Reverse: insert the line */
		if (op->text) {
			editor_insert_row(bcur(), op->row, op->text, op->len);
			buffer_note_change(bcur());
		}
		break;

	case UNDO_SPLIT_LINE:
		/* Reverse: truncate row at split point, append saved rest,
		 * delete row+1. Using saved op->text rather than live row+1
		 * content because row+1 may have an auto-indent prefix that was
		 * not part of the original text. */
		if (op->row < bcur()->numrows) {
			erow *row = &bcur()->row[op->row];
			int split_col = op->col;
			if (split_col < 0) {
				split_col = 0;
			}
			if (split_col > row->size) {
				split_col = row->size;
			}
			row->size = split_col;
			row->chars[split_col] = '\0';
			if (op->text && op->len > 0) {
				editor_row_append_string(
				    row, op->text, op->len);
			} else {
				editor_update_row(bcur(), row);
			}
			if (op->row + 1 < bcur()->numrows) {
				editor_del_row(bcur(), op->row + 1);
			}
			buffer_note_change(bcur());
		}
		break;

	case UNDO_JOIN_LINE:
		/* Reverse: split the line */
		if (op->row < bcur()->numrows && op->text) {
			erow *row;
			int split_col;
			/* Insert new line after current; this realloc's
			 * bcur()->row, so fetch the row pointer afterwards. */
			editor_insert_row(
			    bcur(), op->row + 1, op->text, op->len);
			row = &bcur()->row[op->row];
			split_col = op->col;
			if (split_col < 0) {
				split_col = 0;
			}
			if (split_col > row->size) {
				split_col = row->size;
			}
			row->size = split_col;
			row->chars[split_col] = '\0';
			editor_update_row(bcur(), row);
			buffer_note_change(bcur());
		}
		break;

	case UNDO_KILL_TEXT:
		/* Reverse: re-insert the killed text at the original position.
		 * Cursor is already set to (op->row, op->col) above. */
		if (op->text && op->len > 0) {
			editor_insert_text_raw(op->text, op->len);
		}
		break;

	case UNDO_YANK_TEXT:
		/* Reverse: delete the yanked text forward from (op->row,
		 * op->col). Cursor is already set to (op->row, op->col) above.
		 */
		if (op->len > 0) {
			editor_delete_text_range_raw(op->row, op->col, op->len);
		}
		break;

	case UNDO_REPLACE_TEXT:
		/* Reverse: delete the replacement span, then restore the
		 * original bytes saved in op->text.  op->c is the replacement
		 * byte length. */
		if (op->c > 0
		    && !editor_delete_text_range_raw(op->row, op->col, op->c)) {
			break;
		}
		if (op->text && op->len > 0) {
			editor_insert_text_raw(op->text, op->len);
		}
		break;

	case UNDO_RECT_OVERWRITE:
		undo_replay_rect_overwrite(op);
		break;

	case UNDO_REFLOW_PARA:
		undo_replay_reflow_para(op);
		break;
	}

	/* Check if we've undone back to the saved state */
	if (st->size == st->clean_size) {
		bcur()->dirty = 0;
	}

	/* Free the operation */
	if (op->text) {
		free(op->text);
	}
	free(op);

	editor_set_status_message("Undo");
}

/* Mark current state as clean (called after save) */
void undo_mark_clean(void)
{
	bcur()->undostack.clean_size = bcur()->undostack.size;
}
