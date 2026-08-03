#ifndef KG_LISP_HOOKS_H
#define KG_LISP_HOOKS_H

struct FeContext;
struct FeObject;

void lisp_hooks_init(struct FeContext *ctx);
void lisp_hooks_shutdown(struct FeContext *ctx);

struct FeObject *native_add_hook(
    struct FeContext *context, struct FeObject *arguments);
struct FeObject *native_remove_hook(
    struct FeContext *context, struct FeObject *arguments);
struct FeObject *native_run_hooks(
    struct FeContext *context, struct FeObject *arguments);

#endif /* KG_LISP_HOOKS_H */
