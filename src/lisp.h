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
void kg_lisp_set_interrupt_check(int (*check)(void));
/* Reports compile-time availability without initializing the interpreter. */
[[nodiscard]] int kg_lisp_active(void);

#endif /* KG_LISP_H */
