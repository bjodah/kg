#ifndef KG_LISP_PROCESS_H
#define KG_LISP_PROCESS_H

/* The Lisp-facing half of src/process_table.[ch]: process objects, the
 * eight natives named in the sub-plan (start-process, start-shell-command,
 * process-live-p, delete-process, process-buffer, set-process-filter,
 * set-process-sentinel, process-status), and the drain subscriber that
 * delivers a process's output to its filter and its exit to its sentinel.
 * See src/lisp_hooks.c for the frame-save/restore discipline this module
 * copies for both callbacks. */

struct FeContext;
struct FeObject;

void lisp_process_init(struct FeContext *ctx);
void lisp_process_shutdown(struct FeContext *ctx);

struct FeObject *native_start_process(
    struct FeContext *context, struct FeObject *arguments);
struct FeObject *native_start_shell_command(
    struct FeContext *context, struct FeObject *arguments);
struct FeObject *native_process_live_p(
    struct FeContext *context, struct FeObject *arguments);
struct FeObject *native_delete_process(
    struct FeContext *context, struct FeObject *arguments);
struct FeObject *native_process_buffer(
    struct FeContext *context, struct FeObject *arguments);
struct FeObject *native_set_process_filter(
    struct FeContext *context, struct FeObject *arguments);
struct FeObject *native_set_process_sentinel(
    struct FeContext *context, struct FeObject *arguments);
struct FeObject *native_process_status(
    struct FeContext *context, struct FeObject *arguments);

#endif /* KG_LISP_PROCESS_H */
