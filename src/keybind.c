/* keybind.c - User key bindings on the C-c prefix.
 *
 * The single canonical key-sequence parser lives here; configuration
 * (global-set-key), dispatch (kbd.c), and any future help display must all
 * go through it.  Only two-key sequences starting with C-c are
 * bindable: C-c followed by a printable character or a control letter.
 * C-c is reserved for users, matching the Emacs convention, so user
 * bindings can never collide with built-in keys.
 */

#include <string.h>

#include "def.h"
#include "keybind.h"

#define KEYBIND_MAX 32
#define KEYBIND_NAME_MAX 64

struct keybind {
	int key; /* 0 marks a free slot */
	char command[KEYBIND_NAME_MAX];
};

static struct keybind bindings[KEYBIND_MAX];

/* Parse "C-c X" or "C-c C-x" into the second key's code.  Returns 0 on
 * success.  The parser is deliberately strict: exactly one space, no
 * trailing text, and the second key must be printable or a control
 * letter other than C-c/C-g/C-x (which would shadow prefix handling and
 * cancel). */
int keybind_parse(const char *sequence, int *key)
{
	const char *p = sequence;
	int c;

	if (!p || strncmp(p, "C-c ", 4) != 0) {
		return 1;
	}
	p += 4;
	if (p[0] == 'C' && p[1] == '-' && p[2] != '\0' && p[3] == '\0') {
		c = (unsigned char)p[2];
		if (c < 'a' || c > 'z') {
			return 1;
		}
		c = c & 0x1f;
		if (c == CTRL_C || c == CTRL_G || c == CTRL_X) {
			return 1;
		}
	} else if (p[0] != '\0' && p[1] == '\0') {
		c = (unsigned char)p[0];
		if (c < 32 || c > 126 || c == ' ') {
			return 1;
		}
	} else {
		return 1;
	}
	*key = c;
	return 0;
}

/* Bind a parsed sequence to a named command.  Rebinding a key replaces
 * its command.  Returns 0 on success, 1 on a bad sequence or name, 2
 * when the table is full. */
int keybind_bind(const char *sequence, const char *command)
{
	struct keybind *slot = NULL;
	int key, i;

	if (keybind_parse(sequence, &key)) {
		return 1;
	}
	if (!command || !command[0] || strlen(command) >= KEYBIND_NAME_MAX) {
		return 1;
	}
	for (i = 0; i < KEYBIND_MAX; i++) {
		if (bindings[i].key == key) {
			slot = &bindings[i];
			break;
		}
		if (!slot && bindings[i].key == 0) {
			slot = &bindings[i];
		}
	}
	if (!slot) {
		return 2;
	}
	slot->key = key;
	strcpy(slot->command, command);
	return 0;
}

/* Returns 0 on success, 1 on a bad sequence, 2 when the key was not
 * bound. */
int keybind_unbind(const char *sequence)
{
	int key, i;

	if (keybind_parse(sequence, &key)) {
		return 1;
	}
	for (i = 0; i < KEYBIND_MAX; i++) {
		if (bindings[i].key == key) {
			bindings[i].key = 0;
			bindings[i].command[0] = '\0';
			return 0;
		}
	}
	return 2;
}

/* Command bound to C-c <key>, or NULL. */
const char *keybind_lookup(int key)
{
	int i;

	if (key == 0) {
		return NULL;
	}
	for (i = 0; i < KEYBIND_MAX; i++) {
		if (bindings[i].key == key) {
			return bindings[i].command;
		}
	}
	return NULL;
}
