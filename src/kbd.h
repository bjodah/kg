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

/* Installs the built-in keymaps.  The editor calls this on its first
 * keystroke; a test calls it after keymap_reset() to read what the
 * built-in declarations resolve to. */
void key_install_builtin_maps(void);

/* Forgets a key sequence that was part-way through, for the places that
 * reset the editor out from under one. */
void key_reset_pending_sequence(void);

/* The two batched edits a numeric argument asks for: N lines killed as
 * one undo record, and N copies of the kill ring yanked as one.  They
 * write undo records by hand, which the mutation-gateway manifest counts
 * in this file, so they stay here and the commands call them rather than
 * the other way round -- until plan 02 moves both onto the edit
 * gateway. */
void key_kill_lines(int n);
void key_yank_repeated(int n);

#endif /* KG_KBD_H */
