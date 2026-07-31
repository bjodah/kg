#ifndef KG_DESCRIBE_H
#define KG_DESCRIBE_H

/* The command table and the keymaps, describing themselves.
 *
 * Every answer here is read out of the two registries at the moment it
 * is asked, never out of a table kept beside them: that is the whole
 * point of having one command table and one set of keymaps, and it is
 * what makes these commands stay true when a runtime command is defined
 * or removed, or a mode map switches on.
 *
 * Each of these is a cmdfn and is reached through cmd_invoke() like any
 * other command; `fd` is the terminal the prompting ones read from. */

void describe_key(int fd);
void describe_command(int fd);
void describe_bindings(int fd);
void describe_where_is(int fd);

/* The buffer the first three render into.  Deliberately not *help*,
 * which is the static key-binding table C-h paints. */
#define DESCRIBE_BUFFER_NAME "*Describe*"

#endif /* KG_DESCRIBE_H */
