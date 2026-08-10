#ifndef KG_LSP_RENAME_H
#define KG_LSP_RENAME_H

/* `M-x lsp-rename`: rename the symbol at point everywhere the language
 * server says it appears -- Emacs' `eglot-rename`, and the first command
 * in kg that edits files the user did not open.
 *
 * The command asks for the new name, sends `textDocument/rename` and
 * returns; the answer is a WorkspaceEdit, and applying one is
 * src/lsp_edit.h's business.  What is here is the prompt, the symbol at
 * point that fills its default, and the report.
 *
 * Compiled in both configurations, the way src/xref.c is: the command
 * table has one unconditional row for it, so a WITH_LSP=0 kg answers
 * `M-x lsp-rename` with the reason there is no answer rather than with
 * "Unknown command".
 *
 * No key.  Emacs binds `eglot-rename` to nothing either -- a command that
 * rewrites files across a project is one to name deliberately.
 */

/* The longest new name the prompt accepts.  An identifier, not a
 * sentence; the request carries it as one string (src/lsp_req.h) and
 * refuses one that does not fit. */
#define LSP_RENAME_NAME_MAX 128

void editor_lsp_rename(int fd);

#endif /* KG_LSP_RENAME_H */
