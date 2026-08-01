/* undo.c - Simple undo/redo functionality */

#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "edit.h"
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

/* Record one flat-position replacement: the `old_len` bytes of
 * `old_text` that were at `position` are now `new_len` other bytes.  The
 * reverse is that statement read backwards. */
int undo_push_change(struct editor_buffer *b, size_t position, char *old_text,
    int old_len, int new_len)
{
	struct undo_op *op = malloc(sizeof(struct undo_op));
	int alloc_sz;

	if (!op) {
		return 0;
	}
	op->position = position;
	op->c = new_len;
	op->text = NULL;
	op->len = old_len > 0 ? old_len : 0;

	if (old_text && old_len > 0) {
		if (!checked_add_int_size(&alloc_sz, old_len, 1)) {
			free(op);
			return 0;
		}
		op->text = malloc(alloc_sz);
		if (!op->text) {
			free(op);
			return 0;
		}
		memcpy(op->text, old_text, old_len);
		op->text[old_len] = '\0';
	}
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

/* Reverse one UNDO_CHANGE: put the saved bytes back where the
 * replacement bytes now are.  Through the same primitive that made the
 * change, in replay mode, so nothing is recorded and no second
 * implementation of the splice exists to disagree.  Returns 0 with the
 * buffer, point and the modified flag untouched when the transaction
 * refused it -- it stages every allocation before it publishes, so a
 * refusal is a whole edit that did not happen. */
static int undo_replay_change(struct undo_op *op)
{
	struct kg_edit e
	    = kg_edit_replay(bcur(), op->position, op->position + (size_t)op->c,
		op->text ? op->text : "", (size_t)op->len);
	int row, col;

	if (!kg_buffer_replace(&e, NULL)) {
		/* A refusal that was fatal has already said why; saying
		 * "Undo failed" over "Out of memory" loses the reason. */
		if (running) {
			editor_set_status_message("Undo failed");
		}
		return 0;
	}
	buffer_position_to_row_col(bcur(), op->position, &row, &col);
	editor_cursor_goto(row, col);
	return 1;
}

/* Put a popped record back at the head, exactly as it was.  Not
 * undo_stack_add(): this record was never spent, so it neither counts as
 * a push nor may evict the oldest records -- and evicting them here
 * would throw away the saved checkpoint over an edit that did not
 * happen. */
static void undo_relink(struct undo_stack *st, struct undo_op *op)
{
	op->next = st->head;
	st->head = op;
	st->size++;
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

	if (!undo_replay_change(op)) {
		/* Nothing changed, so neither has which undo the
		 * user is owed: put the record back rather than
		 * make the edit it describes unreachable for the
		 * rest of the session. */
		undo_relink(st, op);
		return;
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
