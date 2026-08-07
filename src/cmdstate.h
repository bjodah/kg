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

/* The kill ring's coalescing class: what kind of thing, if any, the last
 * top-level command did to it.  A *class* rather than a command_id on
 * purpose -- kill-line, kill-word and kill-region are different
 * commands that must still coalesce with each other, the way Emacs'
 * kill-region does regardless of which higher-level command called it.
 * KILL_COALESCE_NONE is the default every keystroke starts from, so a
 * command that never touches the kill ring (an unrelated command, a
 * refused one, an undefined key, a prompt, C-g, a buffer switch) breaks
 * eligibility for free, just by not calling cmd_set_kill_class(). */
enum kill_coalesce_class {
	KILL_COALESCE_NONE = 0,
	/* A kill: consecutive kills grow one ring entry (forward kills
	 * append, backward kills prepend -- see yank.h). */
	KILL_COALESCE_KILL,
	/* A copy: always starts a fresh entry and is not itself eligible,
	 * so neither a repeated copy nor a kill that follows one grows an
	 * existing entry. */
	KILL_COALESCE_COPY,
	/* A yank or yank-pop: what makes M-y eligible on the very next
	 * keystroke.  This is the one true "did a yank run last" test --
	 * yank.c's editor_yank_pop() reads it back through
	 * cmd_last_kill_class() rather than keeping a second opinion of its
	 * own, the same way a kill producer already does for coalescing. */
	KILL_COALESCE_YANK,
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
	/* What this keystroke's command did to the kill ring, and what the
	 * previous one did -- same begin/end-keystroke lifecycle as
	 * this_command/last_command, just for coalescing instead of
	 * identity. */
	enum kill_coalesce_class this_kill_class;
	enum kill_coalesce_class last_kill_class;
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

/* A prompt keystroke's kill-class boundary: what the previous prompt
 * key did to the kill ring becomes cmd_last_kill_class()'s answer, the
 * way cmd_state_begin_keystroke() does it for whole commands -- WITHOUT
 * touching command identity or the transient, which prompt keystrokes
 * leave alone (see the file comment).  Called once per keystroke by the
 * prompt read loops (bufmgr.c) so their kills coalesce under the same
 * rule as buffer kills: only with a kill on the directly preceding
 * keystroke. */
void cmd_state_prompt_keystroke(void);

/* What the previous top-level command did to the kill ring, for a kill
 * or copy producer to decide whether it may coalesce with it. */
[[nodiscard]] enum kill_coalesce_class cmd_last_kill_class(void);
/* Publish what *this* command did, for the next keystroke to read back
 * as cmd_last_kill_class(). */
void cmd_set_kill_class(enum kill_coalesce_class cls);

#endif /* KG_CMDSTATE_H */
