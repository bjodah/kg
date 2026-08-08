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

/* Stable identity of an editor mode, independent of how -- or whether --
 * it highlights.  Every struct editor_syntax carries one: the HLDB
 * registry entries in syntax.c and the synthetic records bufmgr.c and
 * dired.c own, so no non-highlighting behavior has to recognize a mode by
 * its highlighter function pointer.  KG_MODE_TEXT is first, and is the
 * mode a zero-initialized record names. */
enum kg_mode_id {
	KG_MODE_TEXT = 0,
	/* The HLDB registry, in table order. */
	KG_MODE_C,
	KG_MODE_PYTHON,
	KG_MODE_SHELL,
	KG_MODE_JAVASCRIPT,
	KG_MODE_RUST,
	KG_MODE_JAVA,
	KG_MODE_TYPESCRIPT,
	KG_MODE_CSHARP,
	KG_MODE_PHP,
	KG_MODE_RUBY,
	KG_MODE_SWIFT,
	KG_MODE_SQL,
	KG_MODE_DART,
	KG_MODE_HTML,
	KG_MODE_REACT,
	KG_MODE_VUE,
	KG_MODE_ANGULAR,
	KG_MODE_SVELTE,
	KG_MODE_MAKEFILE,
	KG_MODE_MARKDOWN,
	KG_MODE_LISP,
	KG_MODE_GIT_COMMIT,
	KG_MODE_GIT_REBASE,
	KG_MODE_YAML,
	/* Synthetic modes, deliberately outside HLDB: never selected by a
	 * filename, so syntax_find_by_mode() does not resolve them. */
	KG_MODE_IBUFFER,
	KG_MODE_COMPILATION,
	KG_MODE_LISP_INTERACTION,
	KG_MODE_DIRED,
};

/* Syntax highlight definition */
struct editor_syntax {
	enum kg_mode_id id;
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
struct editor_syntax *syntax_find_by_mode(enum kg_mode_id id);
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
