#ifndef KG_KBD_H
#define KG_KBD_H

/* Key dispatch: what the editor does with one keystroke.
 *
 * editor_process_keypress() reads a key and runs it, including the prefix
 * traversal (C-x, C-x r, C-c) and the numeric-argument collection that
 * precede a command. */

void editor_process_keypress(int fd);

/* Ask a yes/no question in the echo area and read one key.  Returns 1 only
 * for a literal yes; anything else, C-g included, is a no.  Every y/n
 * question in the editor goes through here, so they all agree on what a
 * yes is and on when the question is on screen. */
[[nodiscard]] int editor_confirm_yn(int fd, const char *fmt, ...);

/* Whether `c` is a key kbd.c refuses outright in a read-only buffer.
 *
 * This is the second read-only verdict, beside CMD_EDITS_BUFFER in the
 * command table: it judges keycodes rather than commands, so it says
 * nothing about a command reached by M-x or from Lisp, and it names only
 * the editing keys someone remembered to list.  It is exported so
 * test/test_keys.c can check the binding inventory's recorded verdict
 * against it; both it and the check go when every editing key resolves to
 * a command name (plan 01 phase 4). */
[[nodiscard]] int key_would_edit_readonly_buffer(int c);

#endif /* KG_KBD_H */
