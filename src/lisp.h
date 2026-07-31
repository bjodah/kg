#ifndef KG_LISP_H
#define KG_LISP_H

#include <stddef.h>

[[nodiscard]] int kg_lisp_init(void);
void kg_lisp_shutdown(void);
[[nodiscard]] int kg_lisp_eval_string(
    const char *source, size_t length, char *result, size_t result_size);
[[nodiscard]] int kg_lisp_load_file(const char *path);
/* Loads the resolved init file; missing files are normal and succeed. */
[[nodiscard]] int kg_lisp_load_init(void);
[[nodiscard]] const char *kg_lisp_last_error(void);
/* Runs a Lisp-defined command: 0 when the name is known (errors are
 * shown in the status area), nonzero when no such command exists. */
[[nodiscard]] int kg_lisp_run_command(const char *name, int fd);
/* Whether a Lisp-defined command of this name is registered.  cmd_invoke()
 * asks before running one so command policy is applied to it too. */
[[nodiscard]] int kg_lisp_command_exists(const char *name);
/* Iterates Lisp-defined command names for M-x completion; nullptr past
 * the end. */
[[nodiscard]] const char *kg_lisp_command_name(int index);
void kg_lisp_set_interrupt_check(int (*check)(void));
/* Reports compile-time availability without initializing the interpreter. */
[[nodiscard]] int kg_lisp_active(void);

void cmd_eval_last_sexp(int print_to_buffer);

#endif /* KG_LISP_H */
