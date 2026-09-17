#ifndef KG_SHINDENT_H
#define KG_SHINDENT_H

/* Shell-script indentation: the one indenter kg has, for the mode whose
 * block language is line-oriented enough to read without a parser.
 *
 * RET in a shell buffer indents the new line (buffer.c asks
 * shindent_insert_newline() instead of copying the old line's
 * whitespace); TAB reindents the current line
 * (shindent_indent_current_line()), the way Emacs'
 * indent-for-tab-command does.  Every other mode keeps the old
 * behaviour: copied indent, literal TAB.
 *
 * The rules are sh-mode's common cases, measured against Emacs 31
 * (sh-basic-offset 4): a line ending in `then', `do' or `{' -- plus
 * `else', a bare `then' or `do', `case ... in', a trailing backslash
 * and a trailing `&&' or `||' -- indents the next one by
 * SH_INDENT_WIDTH; `fi', `done', `esac', `}' and `elif' find their
 * opener by scanning upward, so nesting works; a `)' pattern sits one
 * level inside its `case', and `;;' keeps the body level.  What this
 * deliberately does not do is what needs a real parser: alignment
 * after an unclosed `(' and the two-space `{ ... ;' group style.
 * doc/kg.1 names these where a user would meet them.
 *
 * Widths are visual columns with tabs expanded at `tab_width'; the bytes
 * written back are tabs-where-possible when the buffer's indent-tabs-mode
 * is not nil (Emacs' default, which is why a depth-2 line there is a TAB),
 * else spaces. */

/* Forward declarations; the full types live in def.h, which shindent.c
 * includes to reach their members. */
typedef struct erow erow;
struct editor_buffer;

/* One sh-basic-offset, Emacs' default. */
#define SH_INDENT_WIDTH 4

/* True when `b' is a shell buffer and TAB/RET indent it.  NULL syntax is
 * not one: before the first selection a buffer has no mode. */
int shindent_active_for_buffer(const struct editor_buffer *b);

/* The same question about the current buffer: what the key and newline
 * paths ask. */
int shindent_active(void);

/* The indent column shindent_indent_current_line() would give row `idx':
 * the pure computation, over any row array, for the test suite.  `rows'
 * holds `numrows' rows; out-of-range `idx' and a first row both answer
 * 0. */
int shindent_target_for_rows(
    const erow *rows, int numrows, int idx, int tab_width);

/* The indent column a newline split after row `filerow' starts with. */
/* `filerow' is the line RET was pressed on, which may be blank. */
int shindent_newline_indent(
    const erow *rows, int numrows, int filerow, int tab_width);

/* Write a `target_col' indent into `out': tabs to the tab stops when
 * `use_tabs' and spaces otherwise.  Returns the byte count, or -1 when
 * `out' is too small.  Pure, for buffer.c and the test suite. */
int shindent_build_indent(
    char *out, int out_size, int target_col, int tab_width, int use_tabs);

/* Reindent the current line to its target, as one undoable edit, and
 * leave point after the new indent when it was inside the old one.
 * Refused silently on a read-only buffer (the transaction's verdict),
 * and a no-op past the end of the buffer. */
void shindent_indent_current_line(void);

/* The newline half for buffer.c: one user edit of `\n' plus the
 * computed indent, point after it.  `filerow' is the line RET split. */
void shindent_insert_newline(int filerow);

#endif /* KG_SHINDENT_H */
