#ifndef KG_BUFMGR_H
#define KG_BUFMGR_H

/* Results shared by all minibuffer readers. */
enum minibuf_result {
	MINIBUF_CANCELLED = -1,
	MINIBUF_ACCEPTED = 0,
	MINIBUF_OVERFLOW = 1,
};

/* Which of the three buffer-name reads is being made.  C-x b and the Lisp
 * `b` and `B` interactive codes share one picker but not one policy, and
 * spelling the differences as separate boolean parameters is how C-x b
 * silently acquired `b`'s re-prompt-on-a-miss (which made C-x b RET on a
 * non-matching query loop forever) and `B`'s M-RET accept. */
enum buf_name_mode {
	/* Lisp `b`.  Only an existing display name is accepted, a miss
	 * re-prompts, and blank means the current buffer. */
	BUF_NAME_EXISTING,
	/* C-x b and Lisp `B`.  A typed name that matches nothing is accepted,
	 * M-RET accepts typed text literally even when a completion exists,
	 * and blank takes the first "other buffer" candidate.  C-x b answers
	 * a name no buffer has by creating that buffer, which is why it wants
	 * the name rather than a dismissal. */
	BUF_NAME_ANY,
};

/* The longest buffer-name query the picker will accept.  Typing past it
 * is refused rather than truncated, and the refusal is retired as soon as
 * the query shrinks back under the cap. */
#define BUF_NAME_QUERY_MAX 64

/* Read a buffer display name without changing the selected buffer.
 *
 * `picked`, when not NULL, receives the buflist index the answer names,
 * or -1 when the answer is a name no buffer has (BUF_NAME_ANY only).
 * A caller that means to select the answer must use it: display names are
 * disambiguated by one parent directory and can still collide, so
 * resolving the returned text by scanning for the first match can select
 * a different buffer than the one the user highlighted. */
enum minibuf_result buf_read_name(int fd, const char *prompt, char *out,
    int outsize, enum buf_name_mode mode, int *picked);

#endif /* KG_BUFMGR_H */
