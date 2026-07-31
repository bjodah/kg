#ifndef KG_SYNTAX_H
#define KG_SYNTAX_H

/* Syntax highlighting: what a row's hl[] bytes mean, what a mode is, and
 * the scanners that fill them in.  Every offset into row->hl is a byte
 * offset into row->render -- see doc/coordinates.md. */

/* Forward declarations; the full types are defined in def.h, which
 * syntax.c includes to reach their members.  Rows are spelled with the
 * struct tag rather than def.h's `erow` typedef so this header needs
 * nothing from def.h at all. */
struct editor_buffer;
struct erow;

/* Syntax highlight types */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2 /* Single line comment. */
#define HL_MLCOMMENT 3 /* Multi-line comment. */
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8 /* Search match. */
#define HL_WARNING 9 /* Overlong commit subject, etc. */

#define HL_HIGHLIGHT_STRINGS (1 << 0)
#define HL_HIGHLIGHT_NUMBERS (1 << 1)

/* Syntax highlight definition */
struct editor_syntax {
	char *name; /* Display name shown in mode line, e.g. "C", "Python" */
	char **filematch;
	char **keywords;
	char singleline_comment_start[5];
	char multiline_comment_start[5];
	char multiline_comment_end[5];
	int flags;
	/* NULL => generic keyword scanner.  The buffer is passed because a
	 * multi-line construct's state comes from the row above, which is
	 * only reachable through the buffer that owns it. */
	void (*highlight)(struct editor_buffer *b, struct erow *row);
};

/* syntax.c */
int is_separator(int c);
int editor_row_has_open_comment(struct erow *row);
void editor_update_syntax(struct editor_buffer *b, struct erow *row);
void editor_rehighlight_from(struct editor_buffer *b, int start_idx);
struct editor_syntax *syntax_find_by_name(const char *name);
void editor_set_syntax(struct editor_buffer *b, struct editor_syntax *syntax);
void editor_rehighlight_all(struct editor_buffer *b);
int editor_syntax_to_color(int hl);
void editor_select_syntax_highlight(struct editor_buffer *b, char *filename);
[[nodiscard]] int syntax_is_git_commit(void);
[[nodiscard]] int syntax_git_commit_subject(void);
[[nodiscard]] int syntax_is_git_rebase(void);
int syntax_git_rebase_pick_span(
    const char *line, int len, int *start, int *wlen);
int syntax_git_rebase_flags_end(const char *line, int len, int from);

#endif /* KG_SYNTAX_H */
