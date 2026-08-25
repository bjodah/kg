#ifndef KG_PASTE_H
#define KG_PASTE_H

#include <stdbool.h>
#include <stddef.h>

#define BRACKETED_PASTE_ON "\x1b[?2004h"
#define BRACKETED_PASTE_OFF "\x1b[?2004l"

/* Whether the terminal TERM is capable of bracketed paste. */
[[nodiscard]] bool kg_bracketed_paste_term_supported(const char *term);

/* Start / stop asking the terminal for bracketed paste. */
void kg_bracketed_paste_start(void);
void kg_bracketed_paste_stop(void);

/* Store / clear decoded bracketed paste payload. */
void kg_bracketed_paste_record(char *data, size_t len);
void kg_bracketed_paste_clear(void);

/* Access the stored payload. */
[[nodiscard]] const char *kg_bracketed_paste_data(size_t *len);

/* Insert the pending paste in the current buffer at point. */
void kg_bracketed_paste_handle_pending(void);

#endif /* KG_PASTE_H */
