#ifndef KG_KEYBIND_H
#define KG_KEYBIND_H

/* User key bindings on the C-c prefix.  keybind.c is the single canonical
 * key-sequence parser: configuration (global-set-key), dispatch (kbd.c)
 * and any future help display all go through it.  Only "C-c <key>"
 * sequences are bindable. */

/* Parses one sequence into the keycode kbd.c dispatches on.  Returns 0 and
 * writes *key on success, non-zero for anything not bindable. */
[[nodiscard]] int keybind_parse(const char *sequence, int *key);
[[nodiscard]] int keybind_bind(const char *sequence, const char *command);
[[nodiscard]] int keybind_unbind(const char *sequence);
/* Command name bound to `key`, or NULL. */
[[nodiscard]] const char *keybind_lookup(int key);

#endif /* KG_KEYBIND_H */
