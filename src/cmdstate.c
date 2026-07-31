/* ===================== Command state for one session ===================== */

#include "cmdstate.h"

/* One session, one answer.  The editor runs one command at a time (a
 * nested one only while its caller waits), so this is a single record
 * rather than anything per buffer or per window. */
static struct command_state state;

const struct command_state *cmd_state(void) { return &state; }

void cmd_state_begin_keystroke(void)
{
	state.last_command = state.this_command;
	state.this_command = CMD_ID_NONE;
}

command_id cmd_state_begin_command(command_id id)
{
	command_id outer = state.this_command;

	state.this_command = id;
	state.invocation_depth++;
	return outer;
}

void cmd_state_end_command(command_id outer)
{
	if (state.invocation_depth > 0) {
		state.invocation_depth--;
	}
	/* A nested command borrows this_command while it runs, and gives it
	 * back when it finishes: the identity the keystroke ends up
	 * publishing is the outermost command's, so (command-execute ...)
	 * inside a command cannot rewrite the repeat history around it. */
	if (state.invocation_depth > 0) {
		state.this_command = outer;
	}
}

int cmd_transient_value(void)
{
	if (state.transient.owner == CMD_ID_NONE) {
		return 0;
	}
	/* Both halves matter: the state belongs to the command asking for
	 * it, and that command is the one that ran last.  Anything else in
	 * between -- another command, a keystroke that ran none -- makes
	 * this invocation a first one. */
	if (state.transient.owner != state.this_command
	    || state.transient.owner != state.last_command) {
		return 0;
	}
	return state.transient.value;
}

void cmd_set_transient_value(int value)
{
	state.transient.owner = state.this_command;
	state.transient.value = value;
}

void cmd_clear_transient(void)
{
	state.transient.owner = CMD_ID_NONE;
	state.transient.value = 0;
}

void cmd_forget_transient_owner(command_id id)
{
	if (id != CMD_ID_NONE && state.transient.owner == id) {
		cmd_clear_transient();
	}
}
