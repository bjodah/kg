#ifndef KG_KEYBIND_H
#define KG_KEYBIND_H

#include <stddef.h>

/* User key bindings: the C-c prefix, and the function keys.
 *
 * Parsing is key_parse()'s; this is the restricted subset kg lets a user
 * bind, which is "C-c <key>", one function key ("<f2>", "C-<f5>",
 * "M-<f10>") and that function key's ESC-prefix spelling ("ESC <f2>").
 * keybind.c says why each shape is in and what the last two have to do
 * with each other.  The bindings live in a keymap of their own, so
 * dispatch finds them the way it finds every other binding. */

/* Validates a sequence and writes its canonical spelling into `out`
 * (KEYMAP_SEQUENCE_FORMAT_MAX bytes is always enough).  The ESC-prefix
 * and Meta spellings of a function key share one canonical form, the
 * Meta one.  Returns 0 on success, non-zero for anything outside the
 * bindable subset. */
[[nodiscard]] int keybind_parse(const char *sequence, char *out, size_t size);
/* 0 on success, 1 for a sequence or name that is not bindable, 2 when
 * the keymap has no room. */
[[nodiscard]] int keybind_bind(const char *sequence, const char *command);
[[nodiscard]] int keybind_unbind(const char *sequence);
/* Command bound to `sequence` in the user's map, or NULL. */
[[nodiscard]] const char *keybind_lookup(const char *sequence);

#endif /* KG_KEYBIND_H */
