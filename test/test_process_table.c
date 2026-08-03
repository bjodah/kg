#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* test_process_table.c — pure table logic for src/process_table.c.
 *
 * Real children are used where a status has to come from somewhere (a
 * short-lived /bin/sh -c or an argv'd /bin/true), but every assertion is
 * about the table's own bookkeeping: slot reuse, generation checks, a full
 * table's refusal, the output cap's drop-oldest/truncation-flag policy, and
 * an exit event's retry-not-drop rule.  Real children, real timing and the
 * Lisp-facing natives are PTY territory; nothing here needs either.
 *
 * The process table's drain subscriber is registered once, by
 * kg_process_table_init(), and this file never calls
 * kg_event_queue_init()/kg_event_queue_set_capacity_for_test() -- both wipe
 * the subscriber registry, and process_table.c's own init is a one-shot
 * that would never re-register it.  Ring pressure (the exit-publication
 * retry test) is arranged instead by holding reservations directly. */

#include "../src/bufhandle.h"
#include "../src/def.h"
#include "../src/event.h"
#include "../src/process.h"
#include "../src/process_table.h"
#include "test.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct kg_process_handle spawn_cmd(
    const char *command, struct kg_buffer_handle buf)
{
	struct kg_spawn_request req = {
		.command = command,
		.stdin_fd = -1,
		.nonblocking_output = true,
	};
	return kg_process_table_spawn(&req, buf);
}

static struct kg_process_handle spawn_argv(
    const char *const *argv, struct kg_buffer_handle buf)
{
	struct kg_spawn_request req = {
		.argv = argv,
		.stdin_fd = -1,
		.nonblocking_output = true,
	};
	return kg_process_table_spawn(&req, buf);
}

/* Poll until `h` is no longer running, or `max_iters` 1ms ticks pass. */
static void poll_until_finished(struct kg_process_handle h, int max_iters)
{
	struct kg_process_table_info info;

	for (int i = 0; i < max_iters; i++) {
		kg_process_table_poll();
		if (kg_process_table_query(h, &info)
		    && info.status != KG_PROCESS_RUNNING) {
			return;
		}
		usleep(1000);
	}
}

static void setup(void)
{
	/* Flush whatever an earlier test left queued, so ring-capacity
	 * arithmetic in this test starts from a known baseline -- without
	 * ever calling kg_event_queue_init(), which would also unregister
	 * the process table's drain subscriber. */
	kg_event_drain_safe();
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
	/* reset_current_buffer() activates buflist[buf_current] directly,
	 * bypassing the counted open path -- buf_kill_buffer() (used by
	 * test_killed_buffer_discards_queued_output) refuses below
	 * buf_count 2, so this test's one counted buffer has to be told
	 * about the one reset_current_buffer() just made. */
	buf_count = 1;
	kg_process_table_init();
}

/* ---- generation-checked handles and slot reuse ------------------------- */

static void test_generation_checked_handles(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle zero = { 0, 0 };
	struct kg_process_handle bad_slot = { KG_PROCESS_TABLE_MAX, 1 };
	struct kg_process_handle h;

	setup();
	buf = buf_handle(buf_current);

	CHECK(!kg_process_table_resolves(zero));
	CHECK(!kg_process_table_resolves(bad_slot));

	h = spawn_cmd("exit 0", buf);
	CHECK(kg_process_table_resolves(h));
	CHECK(h.generation != 0);

	/* A handle to the same slot but the wrong generation never
	 * resolves, whether or not that generation ever existed. */
	CHECK(!kg_process_table_resolves(
	    (struct kg_process_handle) { h.slot, h.generation + 1 }));

	poll_until_finished(h, 2000);
	CHECK(kg_process_table_release(h));
	CHECK(!kg_process_table_resolves(h));
}

static void test_slot_reuse_bumps_generation(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle h1, h2;

	setup();
	buf = buf_handle(buf_current);

	h1 = spawn_cmd("exit 0", buf);
	CHECK(kg_process_table_resolves(h1));
	poll_until_finished(h1, 2000);
	CHECK(kg_process_table_release(h1));

	h2 = spawn_cmd("exit 0", buf);
	CHECK(kg_process_table_resolves(h2));
	/* The freed slot is the first free slot, so it is what gets reused
	 * -- but the point of the generation check is that a caller does
	 * not have to know that: h1 must never resolve to h2's process,
	 * slot reuse or not. */
	CHECK(h2.slot == h1.slot);
	CHECK(h2.generation != h1.generation);
	CHECK(!kg_process_table_resolves(h1));
	CHECK(kg_process_table_resolves(h2));

	poll_until_finished(h2, 2000);
	kg_process_table_release(h2);
}

/* ---- full-table refusal and reclaim ------------------------------------ */

static void test_full_table_of_running_processes_refuses_spawn(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle handles[KG_PROCESS_TABLE_MAX];
	struct kg_process_handle refused;

	setup();
	buf = buf_handle(buf_current);

	/* Never polled, so the table still counts each as running even
	 * though a short-lived child may already have exited in the
	 * kernel -- "running" is this table's own bookkeeping, not a
	 * fresh look at the process. */
	for (int i = 0; i < KG_PROCESS_TABLE_MAX; i++) {
		handles[i] = spawn_cmd("exit 0", buf);
		CHECKF(kg_process_table_resolves(handles[i]), "slot %d", i);
	}

	refused = spawn_cmd("exit 0", buf);
	CHECK(!kg_process_table_resolves(refused));
	CHECK(refused.generation == 0);

	kg_process_table_shutdown();
	for (int i = 0; i < KG_PROCESS_TABLE_MAX; i++) {
		CHECK(!kg_process_table_resolves(handles[i]));
	}
}

static void test_spawn_reclaims_oldest_published_finished_entry(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle handles[KG_PROCESS_TABLE_MAX];
	struct kg_process_handle reclaimer;

	setup();
	buf = buf_handle(buf_current);

	/* One at a time, each brought all the way to finished-and-published
	 * before the next is spawned.  Spawning all eight and then waiting
	 * would leave the *reap* order -- which is what finish_seq records,
	 * and so which slot counts as oldest -- up to the kernel's
	 * scheduling of eight concurrent children, while the assertion
	 * below names slot 0 specifically.  That race is not theoretical:
	 * this test failed under .ci/ci-11's -funsigned-char build, where
	 * the timing shifted just enough for a later slot to be reaped
	 * first. */
	for (int i = 0; i < KG_PROCESS_TABLE_MAX; i++) {
		struct kg_process_table_info info;

		handles[i] = spawn_cmd("exit 0", buf);
		CHECKF(kg_process_table_resolves(handles[i]), "slot %d", i);
		poll_until_finished(handles[i], 5000);
		CHECKF(kg_process_table_query(handles[i], &info)
			&& info.status == KG_PROCESS_EXITED,
		    "slot %d finished", i);
	}

	/* Every slot is finished and published, so the table is full but
	 * reclaimable: the next spawn must succeed by taking the oldest
	 * one rather than refusing. */
	reclaimer = spawn_cmd("exit 0", buf);
	CHECK(kg_process_table_resolves(reclaimer));
	CHECK(reclaimer.slot == handles[0].slot);
	CHECK(!kg_process_table_resolves(handles[0]));
	for (int i = 1; i < KG_PROCESS_TABLE_MAX; i++) {
		CHECKF(
		    kg_process_table_resolves(handles[i]), "slot %d kept", i);
	}

	kg_process_table_shutdown();
}

/* ---- status decoding ---------------------------------------------------- */

static void test_status_decoding_exit_code(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle h;
	struct kg_process_table_info info;

	setup();
	buf = buf_handle(buf_current);
	h = spawn_cmd("exit 7", buf);

	CHECK(kg_process_table_query(h, &info));
	CHECK(info.status == KG_PROCESS_RUNNING);

	poll_until_finished(h, 2000);
	CHECK(kg_process_table_query(h, &info));
	CHECK(info.status == KG_PROCESS_EXITED);
	CHECK(info.code == 7);

	kg_process_table_release(h);
}

static void test_status_decoding_signal(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle h;
	struct kg_process_table_info info;
	static const char *const argv[]
	    = { "/bin/sh", "-c", "kill -TERM $$", NULL };

	setup();
	buf = buf_handle(buf_current);
	h = spawn_argv(argv, buf);

	poll_until_finished(h, 2000);
	CHECK(kg_process_table_query(h, &info));
	CHECK(info.status == KG_PROCESS_SIGNALED);
	CHECK(info.code == SIGTERM);

	kg_process_table_release(h);
}

static void test_release_refuses_a_still_running_process(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle h;

	setup();
	buf = buf_handle(buf_current);
	h = spawn_cmd("exit 0", buf);

	CHECK(!kg_process_table_release(h));
	CHECK(kg_process_table_resolves(h));

	poll_until_finished(h, 2000);
	CHECK(kg_process_table_release(h));
}

/* ---- output cap and truncation flag ------------------------------------- */

static void test_output_cap_drops_oldest_and_flags_truncation(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle h;
	char *big;
	size_t big_len = KG_PROCESS_OUTPUT_MAX + 100;
	struct kg_event_reservation res;
	int before_rows, after_rows;
	char *text;
	int len;
	static const char *const argv[] = { "/bin/true", NULL };

	setup();
	buf = buf_handle(buf_current);
	h = spawn_argv(argv, buf);
	poll_until_finished(h, 2000);

	/* More than the cap, with a distinctive tail so the surviving bytes
	 * can be told apart from the dropped ones. */
	big = malloc(big_len);
	CHECK(big != NULL);
	if (!big) {
		return;
	}
	memset(big, 'x', big_len);
	memcpy(big + big_len - 10, "0123456789", 10);

	kg_process_table_test_append_output(h, big, big_len);
	free(big);

	/* Feed it through the queue exactly the way a real read would have:
	 * publish one OUTPUT event for the bytes already queued. */
	res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	kg_event_publish_lifecycle(&res, kg_event_make_process_output(h, buf));

	before_rows = bcur()->numrows;
	kg_event_drain_safe();
	after_rows = bcur()->numrows;
	CHECK(after_rows >= before_rows);

	text = editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
	CHECK(text != NULL);
	if (text) {
		/* Exactly one truncation marker, immediately followed by
		 * exactly KG_PROCESS_OUTPUT_MAX surviving bytes running to
		 * the end of the buffer and ending in the tail tag -- not
		 * the dropped 'x' filler beyond it. */
		static const char marker_text[] = "kg: output truncated\n";
		char *marker = strstr(text, marker_text);

		CHECK(marker != NULL);
		if (marker) {
			CHECK((size_t)(text + len - marker)
			    == strlen(marker_text) + KG_PROCESS_OUTPUT_MAX);
		}
		CHECK((size_t)len >= 10
		    && strncmp(text + len - 10, "0123456789", 10) == 0);
		free(text);
	}

	/* A second, non-overflowing delivery must not repeat the marker. */
	kg_process_table_test_append_output(h, "more", 4);
	res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	kg_event_publish_lifecycle(&res, kg_event_make_process_output(h, buf));
	kg_event_drain_safe();

	text = editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
	CHECK(text != NULL);
	if (text) {
		static const char marker_text[] = "kg: output truncated\n";

		CHECK((size_t)len >= 4
		    && strncmp(text + len - 4, "more", 4) == 0);
		/* No marker immediately precedes this delivery: the
		 * overflow-free second append got no truncation line of
		 * its own. */
		CHECK((size_t)len < 4 + strlen(marker_text)
		    || strncmp(text + len - 4 - strlen(marker_text),
			   marker_text, strlen(marker_text))
			!= 0);
		free(text);
	}

	kg_process_table_release(h);
}

/* ---- killed target buffer: output discarded, process unaffected -------- */

static void test_killed_buffer_discards_queued_output(void)
{
	struct kg_buffer_handle target;
	struct kg_process_handle h;
	struct kg_event_reservation res;

	setup();
	target = buf_create_named("*process-table-test-victim*");
	CHECK(target.id != 0);
	if (target.id == 0) {
		return;
	}

	h = spawn_cmd("exit 0", target);
	kg_process_table_test_append_output(h, "hello", 5);

	CHECK(buf_kill_buffer(target));
	CHECK(!buf_resolve(target));

	res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	kg_event_publish_lifecycle(
	    &res, kg_event_make_process_output(h, target));
	kg_event_drain_safe(); /* must not crash, and must discard, not
				* write, the queued bytes */

	/* The process itself is unaffected: still tracked, and its
	 * eventual exit still resolves once reaped. */
	CHECK(kg_process_table_resolves(h));
	poll_until_finished(h, 2000);
	kg_process_table_release(h);
}

/* ---- exit-publication retry --------------------------------------------- */

static void test_exit_event_retried_not_dropped_under_ring_pressure(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle h;
	struct kg_event_reservation holds[KG_EVENT_RING_MAX];
	int n = 0;
	struct kg_event ev;

	setup();
	buf = buf_handle(buf_current);
	h = spawn_cmd("exit 0", buf);

	/* Exhaust the ring's reservation capacity so publish_exit()'s own
	 * reservation attempt is refused on the first poll after the child
	 * is reaped. */
	while (n < KG_EVENT_RING_MAX) {
		holds[n] = kg_event_reserve_lifecycle();
		if (!holds[n].valid) {
			break;
		}
		n++;
	}
	CHECK(n == KG_EVENT_RING_MAX);

	poll_until_finished(h, 2000);
	/* Reaped, but nothing could have been queued: the ring had no
	 * room, and a refused KG_EVENT_PROCESS_EXIT must not be dropped,
	 * only left for the next poll to retry. */
	CHECK(kg_event_queue_pop(NULL) == false);
	CHECK(kg_process_table_resolves(h)); /* still tracked either way */

	/* Free the ring back up; the very next poll must succeed where the
	 * earlier ones could not. */
	for (int i = 0; i < n; i++) {
		kg_event_release_reservation(&holds[i]);
	}
	kg_process_table_poll();

	CHECK(kg_event_queue_pop(&ev) == true);
	CHECK(ev.kind == KG_EVENT_PROCESS_EXIT);
	CHECK(ev.payload.process.process.slot == h.slot);
	CHECK(ev.payload.process.process.generation == h.generation);
	CHECK(ev.payload.process.exited == true);
	CHECK(ev.payload.process.code == 0);

	/* No second exit event follows: the retry flag is one-shot once it
	 * succeeds. */
	CHECK(kg_event_queue_pop(NULL) == false);

	kg_process_table_release(h);
}

/* An argv with no argv[0] is refused before the fork: execvp() would
 * dereference the NULL name looking for it on PATH and die, turning a bad
 * request into a child killed by SIGSEGV. */
static void test_empty_argv_is_refused(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle h;
	const char *argv[] = { NULL };
	struct kg_spawn_request req = {
		.argv = argv,
		.nonblocking_output = true,
		.stdin_fd = -1,
		.stderr_to_output = true,
	};

	setup();
	buf = buf_handle_of(bcur());
	h = kg_process_table_spawn(&req, buf);
	CHECK(!kg_process_table_resolves(h));
}

/* ---- process groups: the whole tree dies, not just the child ----------
 * The completion gate says no child of kg's outlives it.  A shell that
 * backgrounds a grandchild is what makes that a claim about a process
 * *group* rather than about one pid: signalling only the child would leave
 * the grandchild running, reparented to init, still holding kg's pipe.
 * Both exits from the table are asserted, because they signal the group
 * from different call sites.  Both assertions fail if the group signal is
 * replaced by kill(e->pid, ...), which is how they were checked. */

/* The pid a spawned shell printed for the grandchild it backgrounded, or 0
 * if it never arrived.  Drains through the same drain the editor uses, then
 * reads the pid back out of the target buffer's first row. */
static pid_t grandchild_pid_from_buffer(struct kg_buffer_handle buf)
{
	struct editor_buffer *b;
	pid_t pid = 0;

	for (int i = 0; i < 3000 && pid <= 0; i++) {
		kg_process_table_poll();
		kg_event_drain_safe();
		b = buf_resolve(buf);
		if (b && b->numrows > 0 && b->row[0].size > 0) {
			pid = (pid_t)strtol(b->row[0].chars, NULL, 10);
		}
		if (pid <= 0) {
			usleep(1000);
		}
	}
	return pid;
}

/* Whether `pid` is gone: kill(pid, 0) failing with ESRCH.  Polled, because
 * the grandchild is reparented to init on the way out and init's reap is
 * not synchronous with our signal. */
static bool pid_is_gone(pid_t pid)
{
	for (int i = 0; i < 2000; i++) {
		if (kill(pid, 0) < 0 && errno == ESRCH) {
			return true;
		}
		usleep(1000);
	}
	return false;
}

static void test_delete_process_kills_the_whole_group(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle h;
	pid_t grandchild;

	setup();
	buf = buf_handle_of(bcur());
	/* The grandchild outlives the shell's own foreground work on
	 * purpose: only a group signal reaches it. */
	h = spawn_cmd("sleep 300 & echo $!; wait", buf);
	CHECK(kg_process_table_resolves(h));
	grandchild = grandchild_pid_from_buffer(buf);
	CHECK(grandchild > 0);

	CHECK(kg_process_table_terminate_and_release(h));
	CHECK(!kg_process_table_resolves(h));
	CHECK(pid_is_gone(grandchild));
}

static void test_shutdown_leaves_no_survivor(void)
{
	struct kg_buffer_handle buf;
	struct kg_process_handle h;
	pid_t grandchild;

	setup();
	buf = buf_handle_of(bcur());
	h = spawn_cmd("sleep 300 & echo $!; wait", buf);
	CHECK(kg_process_table_resolves(h));
	grandchild = grandchild_pid_from_buffer(buf);
	CHECK(grandchild > 0);

	kg_process_table_shutdown();
	CHECK(!kg_process_table_resolves(h));
	CHECK(pid_is_gone(grandchild));
}

int main(void)
{
	RUN(test_empty_argv_is_refused);
	RUN(test_generation_checked_handles);
	RUN(test_slot_reuse_bumps_generation);
	RUN(test_full_table_of_running_processes_refuses_spawn);
	RUN(test_spawn_reclaims_oldest_published_finished_entry);
	RUN(test_status_decoding_exit_code);
	RUN(test_status_decoding_signal);
	RUN(test_release_refuses_a_still_running_process);
	RUN(test_output_cap_drops_oldest_and_flags_truncation);
	RUN(test_killed_buffer_discards_queued_output);
	RUN(test_exit_event_retried_not_dropped_under_ring_pressure);
	RUN(test_delete_process_kills_the_whole_group);
	RUN(test_shutdown_leaves_no_survivor);
	return test_summary();
}
