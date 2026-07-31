#ifndef KG_KEYBIND_H
#define KG_KEYBIND_H

#include <stddef.h>

/* User key bindings on the C-c prefix.
 *
 * Parsing is key_parse()'s; this is the restricted subset kg lets a user
 * bind, which is "C-c <key>" and nothing else.  The bindings live in a
 * keymap of their own, so dispatch finds them the way it finds every
 * other binding. */

/* Validates a sequence and writes its canonical spelling into `out`
 * (KEY_FORMAT_MAX * 2 + 2 bytes is always enough).  Returns 0 on
 * success, non-zero for anything outside the bindable subset. */
[[nodiscard]] int keybind_parse(const char *sequence, char *out, size_t size);
/* 0 on success, 1 for a sequence or name that is not bindable, 2 when
 * the keymap has no room. */
[[nodiscard]] int keybind_bind(const char *sequence, const char *command);
[[nodiscard]] int keybind_unbind(const char *sequence);
/* Command bound to `sequence` in the user's map, or NULL. */
[[nodiscard]] const char *keybind_lookup(const char *sequence);

#endif /* KG_KEYBIND_H */
