/* process_table.c — the bounded table described in process_table.h.
 *
 * See that header for the poll/drain split and the identity rules; this
 * file is the one place struct kg_process_table_entry is defined, so
 * nothing outside it ever indexes the table directly. */

#include "process_table.h"

#include "def.h"
#include "edit.h"
#include "event.h"
#include "process.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define KG_PROCESS_READ_CHUNK 4096
/* Bytes read per poll per entry, budgeted the same way compile.c's
 * COMPILATION_TICK_BUDGET is: enough to drain a burst, not so much that one
 * very chatty child starves every other poll caller in the same call. */
#define KG_PROCESS_TICK_BUDGET (64 * 1024)

/* Field order is chosen by size/alignment, not by relatedness -- the
 * pointer/size_t/uint64_t/handle fields first, then the four-and-under-byte
 * fields, then every bool last, so the struct pads to what alignment
 * actually needs (clang-analyzer-optin.performance.Padding; see commit
 * 46e77fd for the same fix in src/tty.c's struct meta_key). Values are
 * unchanged -- only the declaration order moved. */
struct kg_process_table_entry {
	char
	    *output; /* KG_PROCESS_OUTPUT_MAX bytes, owned, malloc'd at spawn */
	size_t output_len;
	uint64_t finish_seq; /* set when reaped; orders reclaim eligibility */
	struct kg_buffer_handle buffer;
	uint32_t generation;
	pid_t pid;
	pid_t pgid;
	int output_fd;
	struct kg_process_status wait_status;
	bool active; /* slot claimed: running, or finished and unreleased */
	bool pipe_eof;
	bool reaped;
	bool
	    output_truncated; /* oldest bytes dropped since the last delivery */
	bool exit_needs_publish; /* reaped, but KG_EVENT_PROCESS_EXIT is not
				  * yet queued -- retried every poll */
	bool has_filter; /* a Lisp filter owns this process's output; see
			  * kg_process_table_set_has_filter() */
};

static struct kg_process_table_entry table[KG_PROCESS_TABLE_MAX];
static uint64_t next_finish_seq = 1;

/* ---- identity -------------------------------------------------------- */

static int slot_of(struct kg_process_handle handle)
{
	if (handle.generation == 0 || handle.slot >= KG_PROCESS_TABLE_MAX) {
		return -1;
	}
	if (!table[handle.slot].active
	    || table[handle.slot].generation != handle.generation) {
		return -1;
	}
	return (int)handle.slot;
}

static struct kg_process_handle handle_for(int slot)
{
	return (struct kg_process_handle) {
		.slot = (uint32_t)slot,
		.generation = table[slot].generation,
	};
}

bool kg_process_table_resolves(struct kg_process_handle handle)
{
	return slot_of(handle) >= 0;
}

bool kg_process_table_query(
    struct kg_process_handle handle, struct kg_process_table_info *out)
{
	int slot = slot_of(handle);
	struct kg_process_table_entry *e;

	if (slot < 0) {
		return false;
	}
	if (!out) {
		return true;
	}
	e = &table[slot];
	if (!e->reaped) {
		*out = (struct kg_process_table_info) {
			.status = KG_PROCESS_RUNNING, .code = 0
		};
	} else if (e->wait_status.exited) {
		*out = (struct kg_process_table_info) {
			.status = KG_PROCESS_EXITED,
			.code = e->wait_status.exit_code,
		};
	} else {
		*out = (struct kg_process_table_info) {
			.status = KG_PROCESS_SIGNALED,
			.code = e->wait_status.signal_number,
		};
	}
	return true;
}

/* ---- slot lifecycle ---------------------------------------------------- */

/* Free `slot`'s owned resources and mark it unclaimed.  Safe to call on a
 * slot that was never claimed (output NULL, fd already -1): every field
 * this touches is guarded, so it doubles as spawn's "make sure the slot I
 * am about to reuse starts from nothing" step.  Never touches `generation`
 * -- that is spawn's to bump, which is what stops a handle from resolving
 * to whichever process claims the slot next. */
static void release_entry(int slot)
{
	struct kg_process_table_entry *e = &table[slot];

	free(e->output);
	e->output = NULL;
	e->output_len = 0;
	e->output_truncated = false;
	kg_close_fd(&e->output_fd);
	e->active = false;
	e->reaped = false;
	e->exit_needs_publish = false;
	e->pid = 0;
	e->pgid = 0;
}

bool kg_process_table_release(struct kg_process_handle handle)
{
	int slot = slot_of(handle);

	if (slot < 0 || !table[slot].reaped) {
		return false;
	}
	release_entry(slot);
	return true;
}

bool kg_process_table_set_has_filter(
    struct kg_process_handle handle, bool has_filter)
{
	int slot = slot_of(handle);

	if (slot < 0) {
		return false;
	}
	table[slot].has_filter = has_filter;
	return true;
}

struct kg_buffer_handle kg_process_table_buffer(struct kg_process_handle handle)
{
	int slot = slot_of(handle);

	if (slot < 0) {
		return (struct kg_buffer_handle) { -1, 0, 0 };
	}
	return table[slot].buffer;
}

/* delete-process's bounded wait for a reap after a signal: up to
 * `max_iters` 1 ms ticks, matching kg_process_table_shutdown()'s own
 * bound.  Reaping here (rather than leaving it to the next poll) is what
 * lets kg_process_table_terminate_and_release() answer synchronously. */
static bool wait_for_reap(struct kg_process_table_entry *e, int max_iters)
{
	for (int i = 0; i < max_iters && !e->reaped; i++) {
		if (kg_process_reap(e->pid, &e->wait_status)) {
			e->reaped = true;
			e->finish_seq = next_finish_seq++;
		} else {
			usleep(1000);
		}
	}
	return e->reaped;
}

#define KG_PROCESS_TERM_WAIT_ITERS 50
#define KG_PROCESS_KILL_WAIT_ITERS 100

bool kg_process_table_terminate_and_release(struct kg_process_handle handle)
{
	int slot = slot_of(handle);
	struct kg_process_table_entry *e;

	if (slot < 0) {
		return false;
	}
	e = &table[slot];
	if (!e->reaped) {
		if (e->pgid > 0) {
			kg_process_signal_group(e->pgid, SIGTERM);
		}
		if (!wait_for_reap(e, KG_PROCESS_TERM_WAIT_ITERS)
		    && e->pgid > 0) {
			kg_process_signal_group(e->pgid, SIGKILL);
			wait_for_reap(e, KG_PROCESS_KILL_WAIT_ITERS);
		}
		if (!e->reaped) {
			/* Never blocks indefinitely: a child that ignores
			 * both signals stays running, and the slot with it,
			 * rather than hang this call. */
			return false;
		}
		kg_close_fd(&e->output_fd);
		e->pipe_eof = true;
		e->exit_needs_publish = false; /* about to release; nothing
						* left to notify */
	}
	return kg_process_table_release(handle);
}

char *kg_process_table_take_output(struct kg_process_handle handle, size_t *len)
{
	static const char marker[] = "kg: output truncated\n";
	int slot = slot_of(handle);
	struct kg_process_table_entry *e;
	size_t total, off = 0;
	char *buf;

	if (len) {
		*len = 0;
	}
	if (slot < 0) {
		return NULL;
	}
	e = &table[slot];
	total = e->output_len + (e->output_truncated ? sizeof(marker) - 1 : 0);
	if (total == 0) {
		return NULL;
	}
	buf = malloc(total + 1);
	if (!buf) {
		return NULL; /* leave it queued for the next delivery */
	}
	if (e->output_truncated) {
		memcpy(buf, marker, sizeof(marker) - 1);
		off = sizeof(marker) - 1;
	}
	memcpy(buf + off, e->output, e->output_len);
	buf[total] = '\0';
	e->output_len = 0;
	e->output_truncated = false;
	if (len) {
		*len = total;
	}
	return buf;
}

static int find_free_slot(void)
{
	for (int i = 0; i < KG_PROCESS_TABLE_MAX; i++) {
		if (!table[i].active) {
			return i;
		}
	}
	return -1;
}

/* The oldest finished entry whose exit event has already made it into the
 * event queue -- reclaiming any other finished entry would drop an exit no
 * poll will ever get another chance to publish. */
static int find_reclaimable_slot(void)
{
	int best = -1;

	for (int i = 0; i < KG_PROCESS_TABLE_MAX; i++) {
		struct kg_process_table_entry *e = &table[i];

		if (!e->active || !e->reaped || e->exit_needs_publish) {
			continue;
		}
		if (best < 0 || e->finish_seq < table[best].finish_seq) {
			best = i;
		}
	}
	return best;
}

struct kg_process_handle kg_process_table_spawn(
    const struct kg_spawn_request *req, struct kg_buffer_handle buffer)
{
	int slot = find_free_slot();
	pid_t pid;
	int fd;
	char *output;
	uint32_t generation;

	if (slot < 0) {
		slot = find_reclaimable_slot();
	}
	if (slot < 0) {
		return (struct kg_process_handle) { 0, 0 };
	}

	if (kg_process_spawn(req, &pid, &fd) != 0) {
		return (struct kg_process_handle) { 0, 0 };
	}

	output = malloc(KG_PROCESS_OUTPUT_MAX);
	if (!output) {
		/* Nothing tracks this child if it is left running: kill and
		 * reap it synchronously rather than leak a zombie no slot
		 * owns. */
		struct kg_process_status discard;

		kg_close_fd(&fd);
		kg_process_signal_group(pid, SIGKILL);
		kg_process_wait(pid, &discard);
		return (struct kg_process_handle) { 0, 0 };
	}

	generation = table[slot].generation + 1;
	release_entry(slot);
	table[slot] = (struct kg_process_table_entry) { 0 };
	table[slot].active = true;
	table[slot].generation = generation;
	table[slot].pid = pid;
	table[slot].pgid = pid;
	table[slot].output_fd = fd;
	table[slot].buffer = buffer;
	table[slot].output = output;

	return handle_for(slot);
}

void kg_process_table_shutdown(void)
{
	for (int i = 0; i < KG_PROCESS_TABLE_MAX; i++) {
		struct kg_process_table_entry *e = &table[i];

		if (!e->active) {
			continue;
		}
		if (e->pgid > 0) {
			kg_process_signal_group(e->pgid, SIGKILL);
		}
		if (!e->reaped && e->pid > 0) {
			struct kg_process_status status;

			for (int tries = 0;
			    tries < 100 && !kg_process_reap(e->pid, &status);
			    tries++) {
				usleep(1000);
			}
		}
		release_entry(i);
	}
}

/* ---- output queue -------------------------------------------------------
 * Drop-oldest on overflow: `bytes` is appended, and whatever no longer
 * fits in KG_PROCESS_OUTPUT_MAX is discarded from the *front* of what was
 * already queued (or, when `len` alone exceeds the cap, from the front of
 * `bytes` itself), so the queue always holds the most recent bytes it can
 * afford and the truncation flag it sets is what tells a reader that
 * something older is missing. */
static void entry_append_output(
    struct kg_process_table_entry *e, const char *bytes, size_t len)
{
	if (len == 0) {
		return;
	}
	if (len >= KG_PROCESS_OUTPUT_MAX) {
		memcpy(e->output, bytes + (len - KG_PROCESS_OUTPUT_MAX),
		    KG_PROCESS_OUTPUT_MAX);
		e->output_len = KG_PROCESS_OUTPUT_MAX;
		e->output_truncated = true;
		return;
	}
	if (e->output_len + len > KG_PROCESS_OUTPUT_MAX) {
		size_t drop = e->output_len + len - KG_PROCESS_OUTPUT_MAX;

		memmove(e->output, e->output + drop, e->output_len - drop);
		e->output_len -= drop;
		e->output_truncated = true;
	}
	memcpy(e->output + e->output_len, bytes, len);
	e->output_len += len;
}

void kg_process_table_test_append_output(
    struct kg_process_handle handle, const char *bytes, size_t len)
{
	int slot = slot_of(handle);

	if (slot < 0) {
		return;
	}
	entry_append_output(&table[slot], bytes, len);
}

/* ---- poll: fd -> queue, publish only --------------------------------- */

static void publish_output(int slot)
{
	struct kg_event_reservation res = kg_event_reserve_lifecycle();

	if (!res.valid) {
		/* Harmless: the bytes stay queued, and the next read that
		 * produces anything tries again.  If nothing more ever
		 * arrives, the eventual (never-dropped) exit event flushes
		 * what is left. */
		return;
	}
	kg_event_publish_lifecycle(&res,
	    kg_event_make_process_output(handle_for(slot), table[slot].buffer));
}

static void publish_exit(int slot)
{
	struct kg_process_table_entry *e = &table[slot];
	struct kg_event_reservation res = kg_event_reserve_lifecycle();

	if (!res.valid) {
		/* Retried by the next poll: exit_needs_publish stays set. */
		return;
	}
	kg_event_publish_lifecycle(&res,
	    kg_event_make_process_exit(handle_for(slot), e->buffer,
		e->wait_status.exited,
		e->wait_status.exited ? e->wait_status.exit_code
				      : e->wait_status.signal_number));
	e->exit_needs_publish = false;
}

/* One entry's share of kg_process_table_poll(): read what is available up
 * to the tick budget, reap if the child has finished, and publish whatever
 * events that produced.  Split out so the loop in kg_process_table_poll()
 * stays a single pass with no per-entry branching of its own. */
static void poll_entry(int slot)
{
	struct kg_process_table_entry *e = &table[slot];
	bool produced = false;

	if (!e->pipe_eof && e->output_fd >= 0) {
		char buf[KG_PROCESS_READ_CHUNK];
		size_t read_total = 0;

		while (read_total < KG_PROCESS_TICK_BUDGET) {
			ssize_t n = read(e->output_fd, buf, sizeof buf);

			if (n > 0) {
				entry_append_output(e, buf, (size_t)n);
				read_total += (size_t)n;
				produced = true;
			} else if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					break;
				}
				if (errno == EINTR) {
					continue;
				}
				e->pipe_eof = true;
				break;
			} else {
				e->pipe_eof = true;
				break;
			}
		}
	}

	if (produced) {
		publish_output(slot);
	}

	if (!e->reaped && e->pid > 0
	    && kg_process_reap(e->pid, &e->wait_status)) {
		e->reaped = true;
		e->finish_seq = next_finish_seq++;
		e->exit_needs_publish = true;
		kg_close_fd(&e->output_fd);
		e->pipe_eof = true;
	}

	if (e->exit_needs_publish) {
		publish_exit(slot);
	}
}

void kg_process_table_poll(void)
{
	for (int i = 0; i < KG_PROCESS_TABLE_MAX; i++) {
		if (table[i].active) {
			poll_entry(i);
		}
	}
}

/* ---- the drain subscriber: queue -> buffer ----------------------------
 * Registered with kg_event_subscribe(), so kg_event_drain_safe() is the
 * only thing that ever calls this -- one of src/main.c's three safe
 * points, never the poll call site. */

/* The slot `ev` names, when this drain subscriber has anything to do for
 * it: a process event, resolved (not KG_EVENT_RESOLVED_GONE), naming a
 * live table entry that is not filter-owned (set-process-filter hands that
 * case to src/lisp_process.c's own subscriber instead) and has output
 * queued.  -1 otherwise.  Split out so process_drain_cb() itself stays a
 * single pass over the two things it actually does: write the truncation
 * marker, then the output. */
static int process_drain_slot(
    const struct kg_event *ev, enum kg_event_resolution resolution)
{
	int slot;

	if (ev->kind != KG_EVENT_PROCESS_OUTPUT
	    && ev->kind != KG_EVENT_PROCESS_EXIT) {
		return -1;
	}
	if (resolution == KG_EVENT_RESOLVED_GONE) {
		return -1;
	}
	slot = slot_of(ev->payload.process.process);
	if (slot < 0 || table[slot].has_filter || table[slot].output_len == 0) {
		return -1;
	}
	return slot;
}

static enum kg_event_cb_status process_drain_cb(
    const struct kg_event *ev, enum kg_event_resolution resolution, void *ctx)
{
	static const char truncation_marker[] = "kg: output truncated\n";
	int slot = process_drain_slot(ev, resolution);
	struct kg_process_table_entry *e;
	struct editor_buffer *b;
	size_t end;
	struct kg_edit edit;

	(void)ctx;
	if (slot < 0) {
		return KG_EVENT_CB_CONTINUE;
	}
	e = &table[slot];

	b = buf_resolve(ev->payload.process.buffer);
	if (!b) {
		/* The buffer is gone: the process keeps running (or stays
		 * finished and queryable) regardless, but nothing is left to
		 * append to. */
		e->output_len = 0;
		e->output_truncated = false;
		return KG_EVENT_CB_CONTINUE;
	}

	if (e->output_truncated) {
		end = buffer_byte_length(b);
		edit = kg_edit_internal(b, end, end, truncation_marker,
		    sizeof(truncation_marker) - 1);
		if (kg_buffer_replace(&edit, NULL)) {
			e->output_truncated = false;
		}
	}
	end = buffer_byte_length(b);
	edit = kg_edit_internal(b, end, end, e->output, e->output_len);
	if (kg_buffer_replace(&edit, NULL)) {
		e->output_len = 0;
	}
	return KG_EVENT_CB_CONTINUE;
}

void kg_process_table_init(void)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;
	for (int i = 0; i < KG_PROCESS_TABLE_MAX; i++) {
		/* A never-claimed slot's descriptor must read as -1, not the
		 * 0 a static initializer leaves it at -- 0 is stdin, and
		 * release_entry()'s kg_close_fd() would otherwise close it
		 * the first time a slot is reused. */
		table[i].output_fd = -1;
	}
	kg_event_subscribe(
	    (1u << KG_EVENT_PROCESS_OUTPUT) | (1u << KG_EVENT_PROCESS_EXIT),
	    process_drain_cb, NULL);
}
