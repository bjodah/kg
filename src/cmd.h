#ifndef KG_CMD_H
#define KG_CMD_H

/* The command table and the one route into a command.
 *
 * Every interactive command is invoked through cmd_invoke(), so the
 * question "may this run here, and who may ask for it?" has exactly one
 * answer.  cmdtable in cmd.c is the table; adding a command is adding a
 * row to it. */

/* Prefix argument context for commands */
struct command_prefix {
	int supplied;
	int value;
};

/* Every interactive command is invoked through one descriptor, so the
 * question "may this run here, and who may ask for it?" has exactly one
 * answer.  Before this, the read-only verdict was spelled three times
 * (cmd.c's flags, lisp.c's allow-list, kbd.c's per-keycode blocklist);
 * the first two are now this table, and kbd.c's list is what plan 11
 * phase 3 retires when built-in keys resolve to command names. */
typedef void (*cmdfn)(int fd);

enum command_flags {
	CMD_NONE = 0,
	/* Refused outright in a read-only buffer. */
	CMD_EDITS_BUFFER = 1 << 0,
	/* May be reached from Lisp's (command-execute ...). */
	CMD_LISP_CALLABLE = 1 << 1,
};

struct named_cmd {
	const char *name;
	cmdfn fn; /* NULL for a command that lives in Lisp */
	unsigned flags;
	const char *summary; /* one line, <= 60 columns, no trailing period */
};

/* Who asked for the command.  Only the Lisp origin is policed differently:
 * it is the one caller that must clear CMD_LISP_CALLABLE, and the one that
 * reports a refusal as a Lisp error rather than an echo-area message. */
enum command_origin {
	CMD_ORIGIN_KEY,
	CMD_ORIGIN_MX,
	CMD_ORIGIN_LISP,
};

struct command_context {
	int fd;
	struct command_prefix prefix;
	enum command_origin origin;
};

/* cmd_invoke() verdicts. */
enum command_result {
	CMD_RAN = 0,
	CMD_UNKNOWN = 1, /* no command of that name */
	CMD_NOT_CALLABLE = 2, /* exists, but not from this origin */
	CMD_READ_ONLY = 3, /* exists, but the buffer refuses edits */
};

/* cmd.c */
void editor_named_command(int fd);
[[nodiscard]] int cmd_execute_named(const char *name, int fd);
[[nodiscard]] int cmd_invoke(
    const char *name, const struct command_context *ctx);
[[nodiscard]] const struct named_cmd *cmd_lookup(const char *name);
[[nodiscard]] const struct named_cmd *cmd_descriptor_at(int index);
void cmd_eval_print_last_sexp(void);

#endif /* KG_CMD_H */
