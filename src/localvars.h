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
};

/* What kind of value a file-local variable takes, or LOCAL_VAR_NONE for
 * a name kg does not apply.  The three envelope parsers ask before they
 * scan a value, because a string and a symbol are read differently. */
enum local_var_kind {
	LOCAL_VAR_NONE,
	LOCAL_VAR_STRING,
	LOCAL_VAR_BOOL,
};

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

#endif
