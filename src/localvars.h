#ifndef KG_LOCALVARS_H
#define KG_LOCALVARS_H

#include <stddef.h>

#define KG_COMPILE_COMMAND_MAX 1024

#define DL_MAX_FILESIZE 65536

/* Forward declaration; the full struct is defined in def.h.  localvars.c
 * includes def.h to reach its members. */
typedef struct erow erow;

enum local_bool_value {
	LOCAL_BOOL_UNSET = -1,
	LOCAL_BOOL_FALSE = 0,
	LOCAL_BOOL_TRUE = 1,
};

struct local_settings {
	bool compile_command_set;
	char compile_command[KG_COMPILE_COMMAND_MAX];

	enum local_bool_value buffer_read_only;

	/* File-local display state.  `tab_width' holds only when
	 * `tab_width_set' does; `indent_tabs_mode' is read by the shell
	 * indenter (src/shindent.c), while the C offset is stored but
	 * consumed by nothing yet -- kg has no C/Java indenter. */
	bool tab_width_set;
	int tab_width;
	enum local_bool_value indent_tabs_mode;
	bool c_basic_offset_set;
	int c_basic_offset;

	unsigned ignored_entries;
	unsigned malformed_entries;
};

struct init_settings {
	int tab_width;
	bool tab_width_set;
	bool inhibit_startup_screen;
	bool inhibit_startup_screen_set;
	bool inhibit_startup_message;
	bool inhibit_startup_message_set;
	/* `spell-language' as the init file spelled it, held only when
	 * `spell_language_set' does -- the WITH_LISP=0 build's whole
	 * string channel (kg_lisp_variable_string()'s disabled half).
	 * A tag is a dozen bytes; anything longer is refused at parse
	 * time rather than stored truncated. */
	bool spell_language_set;
	char spell_language[32];
	bool spell_highlight_style_set;
	char spell_highlight_style[16];
};

/* What kind of value a file-local variable takes, or LOCAL_VAR_NONE for
 * a name kg does not apply.  The three envelope parsers ask before they
 * scan a value, because a string and a symbol are read differently.  Kinds
 * past BOOL name one variable each rather than one value shape: two bools
 * already exist and they apply to different slots, so the kind is the
 * dispatch. */
enum local_var_kind {
	LOCAL_VAR_NONE,
	LOCAL_VAR_STRING,
	LOCAL_VAR_BOOL,
	LOCAL_VAR_TAB_WIDTH,
	LOCAL_VAR_C_OFFSET,
	LOCAL_VAR_INDENT_TABS,
};

/* Bounds both envelopes share: tab stops, and the CC offset kg stores
 * without consuming (no indenter reads it yet). */
#define KG_C_BASIC_OFFSET_MIN 0
#define KG_C_BASIC_OFFSET_MAX 32

enum local_var_kind localvars_kind(const char *name);
void localvars_apply_bool(
    struct local_settings *out, const char *text, int len);
void localvars_apply_string(
    struct local_settings *out, const char *text, int len);

void local_settings_init(struct local_settings *settings);

void local_settings_merge(
    struct local_settings *destination, const struct local_settings *source);

void init_settings_init(struct init_settings *settings);

int init_config_parse(
    const char *source, size_t source_len, struct init_settings *out);

int localvars_parse_modeline(
    const erow *rows, int row_count, struct local_settings *out);

int localvars_parse_footer(
    const erow *rows, int row_count, struct local_settings *out);

int dirlocals_find(
    const char *visited_filename, char *result, size_t result_size);

int dirlocals_parse(
    const char *source, size_t source_len, struct local_settings *out);

/* The same grammar with one mode's blocks applied over `nil': what a file
 * visit parses with.  `mode_key' is dirlocals_mode_key()'s answer for the
 * visited buffer, or NULL for `nil' only (which is dirlocals_parse()).
 * Order-independent by construction: every matching mode entry merges
 * after every `nil' one, which is the precedence Emacs was measured to
 * give (mode wins however the file lists them). */
int dirlocals_parse_for_mode(const char *source, size_t source_len,
    struct local_settings *out, const char *mode_key);

/* Forward declaration; the full type lives in syntax.h, which localvars.c
 * reaches through def.h. */
struct editor_syntax;

/* The dir-locals selector a buffer's mode answers to ("c-mode",
 * "java-mode"), or NULL when it answers to none.  NULL syntax is a mode
 * like any other and answers NULL: before the first selection a buffer
 * has no mode, and `nil' still applies to it. */
const char *dirlocals_mode_key(const struct editor_syntax *syntax);

/* Forward declaration; the full struct is defined in def.h. */
struct editor_buffer;

/* Publish a merged visit's settings on its buffer: the display width via
 * editor_set_tab_width(), the two stored-but-unconsumed offsets as buffer
 * state for the indent plan to come.  An unset width clears a previous
 * file-local one back to the default; the Lisp/init sync owns it from
 * there. */
void local_settings_apply_to_buffer(
    struct editor_buffer *b, const struct local_settings *merged);

#endif
