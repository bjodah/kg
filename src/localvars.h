#ifndef KG_LOCALVARS_H
#define KG_LOCALVARS_H

#include <stddef.h>

#define KG_COMPILE_COMMAND_MAX 1024

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

void local_settings_init(struct local_settings *settings);

void local_settings_merge(
    struct local_settings *destination, const struct local_settings *source);

int localvars_parse_modeline(
    const erow *rows, int row_count, struct local_settings *out);

int localvars_parse_footer(
    const erow *rows, int row_count, struct local_settings *out);

int dirlocals_find(
    const char *visited_filename, char *result, size_t result_size);

int dirlocals_parse(
    const char *source, size_t source_len, struct local_settings *out);

#endif
