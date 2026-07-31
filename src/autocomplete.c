/* ============================ Autocompletion ============================= */

#include <string.h>

#include "def.h"
#include "localvars.h"

/* Define pairs of characters that should be autocompleted */
struct autopair {
	int open_char; /* Opening character (like '{') */
	int close_char; /* Closing character (like '}') */
};

/* Array of autocomplete pairs. Can be extended with more character pairs. */
struct autopair autopairs[] = {
	{ '{', '}' },
	{ '[', ']' },
	{ '(', ')' },
	{ '\"', '\"' },
	{ '\'', '\'' },
	{ '`', '`' },
	{ '<', '>' },
};

#define AUTOPAIR_COUNT (sizeof(autopairs) / sizeof(autopairs[0]))

/* Find the matching closing character for the given opening character.
 * Returns the closing character if found, or 0 if no match exists. */
int editor_find_close_char(int open_char)
{
	size_t i;

	for (i = 0; i < AUTOPAIR_COUNT; i++) {
		if (autopairs[i].open_char == open_char) {
			return autopairs[i].close_char;
		}
	}
	return 0;
}

/* Handle character insertion with potential autocompletion.
 * Checks if the typed character has a closing pair and inserts it automatically
 * when appropriate. */
void editor_insert_char_auto_complete(int c)
{
	erow *row = NULL;
	int filerow;
	int filecol;
	int next_char_space;
	int close_char;
	int at_end;

	if (!electric_pairs) {
		editor_insert_char(c);
		return;
	}

	filerow = editor_current_filerow_or_eof();
	if (filerow < bcur()->numrows) {
		row = &bcur()->row[filerow];
	}
	filecol = editor_current_filecol();

	/* Check if we're at end of line or the next character is
	 * whitespace/symbol */
	at_end = (!row || filecol >= row->size);
	next_char_space = at_end
	    || ascii_is_space((unsigned char)row->chars[filecol])
	    || strchr(",.()+-/*=~%[];{}", row->chars[filecol]);

	/* Find closing character if this is an opening bracket/quote */
	close_char = editor_find_close_char(c);

	/* Insert the character first */
	editor_insert_char(c);

	/* Skip autocompletion during paste operations */
	if (editor.paste_mode) {
		return;
	}

	/* If this is a bracket/quote and we're either at end of line or
	 * next character is whitespace/symbol, insert the closing character */
	if (close_char && next_char_space) {
		editor_insert_char(close_char);

		/* Move cursor back between the pair */
		editor_move_cursor(ARROW_LEFT);
	}
}

void editor_self_insert_char(int c)
{
	if (editor.overwrite_mode) {
		editor_overwrite_char(c);
	} else {
		editor_insert_char_auto_complete(c);
	}
}
