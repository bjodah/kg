#ifndef KG_CMDSTATE_H
#define KG_CMDSTATE_H

#include "cmd.h"
#include "keyevent.h"

/* What the editor is running, what it ran last, and the little state a
 * command keeps between two of its own invocations.
 *
 * A command that behaves differently when repeated -- C-l cycling
 * centre/top/bottom, M-r cycling top/middle/bottom -- has to recognise
 * its own previous invocation.  Asking "was the last *key* C-l?" answers
 * a different question: the same command reached by M-x, by a macro or
 * from Lisp is then not a repeat, and a key that is rebound stops being
 * one.  Identity is the command's, so the state is keyed by command_id.
 *
 * Only a completed top-level command advances last_command.  Prefix
 * keystrokes, keys the decoder eats, prompt keystrokes and refused
 * commands are not commands and leave the history alone. */

struct command_transient {
	command_id owner; /* CMD_ID_NONE when there is nothing to keep */
	int value;
};

struct command_state {
	command_id this_command;
	command_id last_command;
	unsigned invocation_depth;
	struct command_transient transient;
	/* The key that started this keystroke, for the commands that are
	 * about the key rather than about the buffer: self-insert-command
	 * inserts it.  It is dispatch state, not the command's identity:
	 * two keys bound to one command are still one command. */
	struct key_event this_key;
	/* Set when this keystroke was a shift-translated motion, which is
	 * what keeps the region alive across it. */
	int shift_translated;
};

[[nodiscard]] const struct command_state *cmd_state(void);

/* Start one keystroke: whatever ran during the previous keystroke becomes
 * last_command, and this keystroke starts out running nothing. */
void cmd_state_begin_keystroke(void);
/* Records what the keystroke about to be dispatched was, and that it
 * extends a shift-selected region. */
void cmd_state_set_key(struct key_event key);
void cmd_state_set_shift_translated(void);

/* Publish `id` as the running command.  Returns the identity to hand back
 * to cmd_state_end_command(), which is the outer command's when this one
 * is nested inside it. */
command_id cmd_state_begin_command(command_id id);
void cmd_state_end_command(command_id outer);

/* The value the running command stored the last time it ran, or 0 when
 * the command that ran last was a different one -- which is exactly when
 * a cycle has to start over. */
[[nodiscard]] int cmd_transient_value(void);
void cmd_set_transient_value(int value);
void cmd_clear_transient(void);
/* Drop transient state owned by `id`: a runtime command being removed
 * must not leave state that a later command with a fresh id could read. */
void cmd_forget_transient_owner(command_id id);

#endif /* KG_CMDSTATE_H */
