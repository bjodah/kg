#ifndef KG_CMD_H
#define KG_CMD_H

/* The command table and the one route into a command.
 *
 * Every interactive command is invoked through cmd_invoke(), so the
 * question "may this run here, and who may ask for it?" has exactly one
 * answer.  cmdtable in cmd.c is the table; adding a command is adding a
 * row to it. */

#include <stdint.h>

/* Which of Emacs' four raw prefix forms the user typed.
 *
 * `value` below is the *effective* integer every built-in consumer has
 * always used (bounded, C-u collapsed to 4/16/...); this says what the
 * raw `current-prefix-arg` a Lisp command sees should be built from.
 * Sub-plan 07A Decision 6 asks for the distinction to be a type rather
 * than the bare 0/1/2/3 it was first written as. */
enum prefix_raw_kind {
	PREFIX_RAW_NONE = 0, /* no prefix typed: raw nil, effective 1 */
	PREFIX_RAW_INTEGER, /* C-u DIGITS, M-DIGITS, M-- DIGITS: raw N */
	PREFIX_RAW_UNIVERSAL, /* bare C-u, C-u C-u, ...: raw (4), (16), ... */
	PREFIX_RAW_MINUS, /* bare M--: raw symbol `-', effective -1 */
};

/* Prefix argument context for commands */
struct command_prefix {
	int supplied;
	int value;
	enum prefix_raw_kind raw_kind;
	/* How many bare C-u were typed, and only meaningful when raw_kind is
	 * PREFIX_RAW_UNIVERSAL.  `value` above is capped at 1000 for the
	 * built-in repeat consumers, but Emacs' raw `(4)`/`(16)`/`(64)`/…
	 * list is not capped -- C-u C-u C-u C-u C-u is `(1024)` there and was
	 * `(1000)` here.  The count is what survives the cap. */
	int universal_count;
};

/* Who, if anyone, is the command currently dispatching. */
struct active_command {
	struct command_prefix prefix;
	bool present;
};

/* Everything cmd_invoke() saves and restores around one command, as a
 * value.  A Lisp completion raised inside a command reached through
 * (command-execute ...) longjmps past cmd_invoke's own restore, so that
 * native saves this before the call and puts it back through an Fe
 * cleanup, which runs on the unwind as well as on the ordinary return. */
struct command_scope {
	struct active_command context;
	struct command_prefix ambient;
	int prompt_fd;
};

/* A command's identity, handed out by the registry and by nothing else.
 *
 * A handler pointer is not an identity: two commands may share one (see
 * read-only-mode and toggle-read-only below), and a Lisp-defined command
 * has none.  A name is not one either: the storage a name lives in is
 * reused when a runtime command is removed and another defined.  So the
 * registry says: a static command is its table slot, a runtime command
 * gets the next unused number, redefining one keeps its number, and
 * removing and recreating one does not. */
typedef uint32_t command_id;

#define CMD_ID_NONE ((command_id)0)
/* Static ids are 1-based table slots; runtime ids start past any table
 * this file could plausibly grow to, so the two never collide and an id
 * says which kind of command it names. */
#define CMD_ID_STATIC_BASE ((command_id)1)
#define CMD_ID_RUNTIME_BASE ((command_id)0x10000)

/* Every interactive command is invoked through one descriptor, so the
 * question "may this run here, and who may ask for it?" has exactly one
 * answer.  Before this, the read-only verdict was spelled three times
 * (cmd.c's flags, the Lisp adapter's allow-list, kbd.c's per-keycode
 * blocklist);
 * the first two are now this table, and kbd.c's list is retired by the
 * layered-keymap migration when built-in keys resolve to command names. */
typedef void (*cmdfn)(int fd);

enum command_flags {
	CMD_NONE = 0,
	/* Refused outright in a read-only buffer. */
	CMD_EDITS_BUFFER = 1 << 0,
	/* May be reached from Lisp's (command-execute ...). */
	CMD_LISP_CALLABLE = 1 << 1,
	/* A numeric argument repeats it: cmd_invoke() runs it that many
	 * times.  Repetition is metadata rather than a loop in each
	 * handler, so a key, M-x, a macro and Lisp all repeat the same
	 * command the same way.  Commands that read the prefix argument
	 * themselves, or that mean something else with one, do not have
	 * this. */
	CMD_REPEATS = 1 << 2,
	/* A vertical motion: it moves between lines at a remembered
	 * column, so the goal column survives it.  Any other command
	 * invalidates it.  Read by key_finish_keypress(). */
	CMD_KEEPS_GOAL_COLUMN = 1 << 3,
	/* The handler can read the terminal -- a minibuffer prompt, a y/n
	 * confirmation, a raw key.  Harmless from a keystroke or from M-x,
	 * where there is a terminal by definition; from Lisp it is only
	 * safe when the activation has a prompt descriptor, which is
	 * exactly what `lisp_prompt_require()' asks for the natives.  With
	 * no descriptor the read would be `read(-1, ...)', whose EBADF path
	 * clears `running' and takes the editor down, so cmd_invoke()
	 * refuses such a command from Lisp instead.  Set it on any command
	 * that can reach editor_read_key(), editor_read_raw_byte(), one of
	 * the editor_read_line* prompts, or prompt_ask_yn(). */
	CMD_READS_TERMINAL = 1 << 4,
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
	CMD_NO_TERMINAL = 4, /* exists, but this activation cannot prompt */
};

/* cmd.c */
void editor_named_command(int fd);
[[nodiscard]] int cmd_execute_named(const char *name, int fd);
[[nodiscard]] int cmd_invoke(
    const char *name, const struct command_context *ctx);
[[nodiscard]] const struct named_cmd *cmd_lookup(const char *name);
[[nodiscard]] const struct named_cmd *cmd_descriptor_at(int index);
/* Every command name by index -- the static table then the Lisp registry,
 * NULL past the last.  The M-x picker walks it, and so does Lisp's
 * `(internal--command-names)`. */
[[nodiscard]] const char *cmd_name_at(int index);
void cmd_eval_print_last_sexp(void);
/* Evaluate the s-expression before point and report it in the status area
 * (or insert it, with a prefix argument). */
void cmd_eval_last_sexp(int print_to_buffer);

/* The two halves of cmd_invoke() a dispatch path needs when it runs a
 * command without going through it.  The self-insert fallback is the one
 * such path: it batches a repeated character into a single insertion, so
 * it cannot be a plain handler call, but it must still be refused by a
 * read-only buffer and still publish the command's identity.
 *
 * Returns 0 when policy refuses the command; on success the caller pairs
 * it with cmd_fast_path_end(*outer). */
[[nodiscard]] int cmd_fast_path_begin(const char *name, command_id *outer);
void cmd_fast_path_end(command_id outer);

/* The identity of `name`, or CMD_ID_NONE when nothing is called that. */
[[nodiscard]] command_id cmd_id_by_name(const char *name);
/* The descriptor an identity names, or NULL.  Dispatch bookkeeping asks
 * what the command that just ran was allowed to keep. */
[[nodiscard]] const struct named_cmd *cmd_descriptor_by_id(command_id id);
/* Told by the runtime command registry (the Lisp adapter) that a command
 * has been defined or removed.  Defining a name that already has an id
 * keeps it;
 * removing frees the slot, so the next definition of that name is a
 * different command.  Returns CMD_ID_NONE when no slot is free. */
command_id cmd_runtime_define(const char *name);
void cmd_runtime_remove(const char *name);
const struct command_prefix *cmd_active_prefix(void);
/* Save/restore the dispatch scope above; see struct command_scope. */
[[nodiscard]] struct command_scope cmd_scope_save(void);
void cmd_scope_restore(struct command_scope scope);
int cmd_prompt_fd(void);
void cmd_prompt_block(void);
void cmd_prompt_unblock(void);

#endif /* KG_CMD_H */
