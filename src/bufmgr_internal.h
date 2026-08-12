#ifndef KG_BUFMGR_INTERNAL_H
#define KG_BUFMGR_INTERNAL_H

#include "bufhandle.h"
#include "event.h"

struct editor_window;

/* Transaction-only forms used by winconfig.c after it has reserved an
 * entire layout's lifecycle events.  They share the ordinary public
 * attach/detach/create commit bodies; a valid reservation is consumed only
 * when the corresponding transition commits. */
int buf_view_attach_reserved_for_transaction(struct editor_window *window,
    int slot, struct kg_event_reservation *reservation);
int buf_view_detach_reserved_for_transaction(
    struct editor_window *window, struct kg_event_reservation *reservation);
void buf_view_clamp_for_transaction(struct editor_window *window);
struct kg_buffer_handle buf_create_named_reserved_for_transaction(
    const char *name, struct kg_event_reservation *reservation);
int buf_named_creation_available_for_transaction(void);

/* Fail only the next name allocation made by buf_create_named(), then
 * disarm.  Test-only; the editor never arms it. */
void buf_create_named_fail_alloc_once_for_test(void);

#endif /* KG_BUFMGR_INTERNAL_H */
