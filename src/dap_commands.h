#ifndef KG_DAP_COMMANDS_H
#define KG_DAP_COMMANDS_H

/* The debugger's commands, as src/cmd.c's table reaches them.
 *
 * Every one of these exists in BOTH build configurations, which is the
 * story src/xref.c already tells for the language-server commands: the
 * cmdtable row is unconditional and the WITH_DAP=0 half answers "kg was
 * built without DAP support" rather than leaving `M-x` to report a command
 * nobody has heard of.  A user who typed the name deserves to be told which
 * of the two it is.
 *
 * This is the only file in the debugger that PROMPTS, and the only one that
 * touches buffers, windows or the command table (parent plan,
 * Architecture).  Everything under it -- the session, the client, the
 * breakpoint table, the stop model -- is reached through its own header and
 * knows nothing about any of that.
 *
 * The keys these are bound to are subplan 02's; nothing here binds
 * anything, which is why `make docs-check` has nothing new to check and why
 * doc/kg.1 documents them by NAME.
 */

void editor_dap_debug(int fd);
void editor_dap_disconnect(int fd);
void editor_dap_terminate(int fd);
void editor_dap_continue(int fd);
void editor_dap_pause(int fd);
void editor_dap_next(int fd);
void editor_dap_step_in(int fd);
void editor_dap_step_instruction(int fd);
void editor_dap_step_out(int fd);
void editor_dap_breakpoint_toggle(int fd);
void editor_dap_breakpoint_temporary(int fd);
void editor_dap_until(int fd);
void editor_dap_goto(int fd);
void editor_dap_restart(int fd);
void editor_dap_evaluate(int fd);
void editor_dap_repl(int fd);
void editor_dap_frame_up(int fd);
void editor_dap_frame_down(int fd);
void editor_dap_many_windows(int fd);

/* Wire the command layer's hooks into the session, the stop model and the
 * decoration projection.  Called from dap_init(), before the init file is
 * read.
 *
 * The shutdown half is deliberately NOT on the editor's teardown path:
 * everything it undoes is either process-lifetime state or a compilation
 * whose own shutdown already cancels the callback, and dap_shutdown() lives
 * in an object every test binary links, which this one is not.  It exists
 * for the tests, which need one case's hooks and choices gone before the
 * next case's. */
void dap_commands_init(void);
void dap_commands_shutdown(void);

#endif /* KG_DAP_COMMANDS_H */
