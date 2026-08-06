#ifndef KG_BUFMGR_H
#define KG_BUFMGR_H

/* Results shared by all minibuffer readers. */
enum minibuf_result {
	MINIBUF_CANCELLED = -1,
	MINIBUF_ACCEPTED = 0,
	MINIBUF_OVERFLOW = 1,
};

/* Read a buffer display name without changing the selected buffer.  When
 * allow_new is false, only an existing name is accepted. */
enum minibuf_result buf_read_name(int fd, const char *prompt, char *out,
    int outsize, int allow_new, int blank_current);

#endif /* KG_BUFMGR_H */
