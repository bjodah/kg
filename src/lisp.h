#ifndef KG_LISP_H
#define KG_LISP_H

#include <stddef.h>

[[nodiscard]] int kg_lisp_init(void);
void kg_lisp_shutdown(void);
[[nodiscard]] int kg_lisp_eval_string(
    const char *source, size_t length, char *result, size_t result_size);
[[nodiscard]] int kg_lisp_load_file(const char *path);
/* Reports compile-time availability without initializing the interpreter. */
[[nodiscard]] int kg_lisp_active(void);

#endif /* KG_LISP_H */
