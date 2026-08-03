# Sub-plan 06-D — Bounded process table, package hygiene, and proofs

Phases 7 and 8 of Plan 06.  Requires sub-plan C (unwind for
filters/sentinels, hooks for proof packages).

**Complexity budget** (follow-ups README, "Decision — Plan 06 gets a
measured budget, cap 5500"): Phase 7 = **230** scc units, Phase 8 = **90**.
The tree stands at 5135 against the 5500 cap, so the two phases have 365
units of room for 320 units of allowance.  A phase that overruns its row
stops and reports; it does not spend the other phase's allowance.

---

## Corrections against repository reality — 2026-08-03

The first draft of this sub-plan was written before Phases 2–6 landed and
before anyone read `src/process.[ch]`, `src/event.[ch]` and `src/main.c`
against it.  Nine of its statements are wrong or unbuildable.  Each is
corrected in place below; they are collected here so a reader who knows
the old draft can see what changed.

1. **There is no `select`/`poll` loop to hook.**  kg blocks in `read()` on
   stdin.  Asynchronous output reaches the editor through
   `compilation_poll()`, a non-blocking drain called from the top of
   `main.c`'s loop (`src/main.c:168`) and from `read_key_byte()`'s idle
   path (`src/tty.c:552`).  The process table follows that pattern; it
   does not introduce an event loop.
2. **Commitment moves to the drain.**  The old draft had output committed
   wherever it was read, which — because of `src/tty.c:552` — means
   `kg_buffer_replace()` running inside `read_key_byte()`.  Polling now
   only moves bytes from the fd into the per-process queue; every buffer
   write and every Lisp callback happens at the event drain.
3. **The event payload cannot carry the output text.**  `src/event.h`'s
   opening contract forbids storing row text, filenames or any other
   runtime value in an envelope.  The payload is identity only; the text
   lives in the process table.
4. **`event_resolution()` resolves a *buffer* handle** (`src/event.c:613`),
   so process events need their own arm — see "Event integration".
5. **The intent is spelled `KG_EDIT_INTERNAL`**, not
   `KG_EDIT_INTERNAL_LIVE` (`src/edit.h:59`).  It is already documented as
   covering "a process's output".
6. **`struct kg_spawn_request` already exists** with `command`,
   `directory`, `stdin_fd`, `stderr_to_output`, `nonblocking_output`.  The
   draft's parallel `kg_process_spawn_opts` would duplicate five of its
   six fields.  Extend the existing struct instead.
7. **`conf-mode` is not buildable.**  It is priced against "Phase 6 mode
   APIs (when available)", and those are exactly the part of Phase 6 that
   stayed blocked: there is no mode registry, no `define-derived-mode` and
   no `defvar`.  It is dropped, not deferred into this sub-plan.
8. **`whitespace-mode` is not buildable either.**  It needs decorations
   from Lisp; `grep decor src/lisp_*.c` finds nothing.  Decoration natives
   are real work that no row of the budget table pays for.
9. **Fe has no property lists**, so docstrings have nowhere to live
   without a new bounded adapter table, and `src/describe.c` currently
   describes keys, commands and bindings — not functions.

---

## Phase 7 — Bounded process table and Lisp process APIs

### Problem

`src/process.[ch]` provides low-level fork/reap/signal primitives used by
`compile.c` (async non-blocking) and `shell.c` (synchronous blocking).
There is no multi-process table, no process handle, no output streaming
into the event queue, and no Lisp API for spawning processes.

### Non-goals

- **`compile.c` and `shell.c` are not migrated onto the table.**  They
  keep their own single-process state.  Migrating them is a follow-up
  worth doing and is not what this budget row bought; a Lisp process API
  is.
- No PTY allocation.  Children get pipes.
- No `process-send-string` / stdin writing.  Children get `/dev/null` on
  stdin unless the caller passes a descriptor.

### Design

#### Step 1 — Explicit argv vs. shell interpretation

Extend the existing `struct kg_spawn_request` rather than adding a second
one:

```c
struct kg_spawn_request {
	/* Passed to /bin/sh -c ... -- ignored when `argv` is set. */
	const char *command;
	/* Explicit argv, NULL-terminated, or NULL to use `command`.  When
	 * this is set the child is exec'd directly: no shell, so no word
	 * splitting, globbing, redirection or substitution can happen to
	 * an argument a caller passed as one string. */
	const char *const *argv;
	const char *directory;
	int stdin_fd;
	bool stderr_to_output;
	bool nonblocking_output;
	/* The child leads its own process group, so a signal reaches the
	 * command's own children.  Already the behaviour; named here
	 * because delete-process depends on it. */
};
```

Exactly one of `argv` and `command` is honoured, `argv` first.  A request
with both is a caller bug and is refused rather than guessed at.

#### Step 2 — Bounded process table

New module `src/process_table.[ch]`.

```c
#define KG_PROCESS_TABLE_MAX 8
/* Bytes of not-yet-delivered output one process may hold.  Reached only
 * when a process outruns the drain -- output is handed to the filter, or
 * appended to the buffer, at every safe point. */
#define KG_PROCESS_OUTPUT_MAX (256 * 1024)
```

The handle type goes in its own minimal header, `src/prochandle.h`,
following `src/bufhandle.h`'s precedent: `event.h` needs the handle and
must not be made to include the whole process table.

```c
struct kg_process_handle {
	uint32_t slot;
	uint32_t generation;
};
```

Entry state: handle, `pid`, `pgid`, `output_fd`, status, exit code or
signal number, target buffer handle, the bounded output queue, and the
filter/sentinel roots.

**Policies:**

- **Slot reuse.**  A finished process keeps its terminal status queryable
  until `delete-process` releases it, or until the table is full and the
  oldest finished entry is reclaimed to make room.  A full table of
  *running* processes refuses the spawn.
- **Output cap.**  `KG_PROCESS_OUTPUT_MAX` per process.  Overflow drops
  the **oldest** bytes and sets a truncation flag; the flag makes the next
  delivery carry a `kg: output truncated` marker exactly once per overflow
  run, so a filter cannot be told nothing happened.
- **Process-buffer ownership.**  The process holds a buffer *handle*.  If
  the buffer is killed the handle stops resolving, output is discarded,
  and the process keeps running — it can still be deleted explicitly, and
  its sentinel still fires.
- **Cancellation escalation.**  `delete-process` sends SIGTERM to the
  process group, polls the reap for a bounded number of attempts, then
  sends SIGKILL.  It never blocks the editor indefinitely.
- **Shutdown.**  `kg_process_table_shutdown()` kills every group and reaps
  every child.  Wired into the same teardown that already runs at exit.
- **Roots.**  Filter and sentinel roots are released when the entry is
  released, and on shutdown.
- **Commitment.**  Output reaches a buffer only through
  `kg_buffer_replace()` with `KG_EDIT_INTERNAL` (`src/edit.h:59`), at the
  end of the buffer, and only from the drain.

#### Polling and commitment — where each half runs

Two halves, deliberately split, because `compilation_poll()` is called
from a place that is *not* a safe point:

```
kg_process_table_poll()      fd -> per-process bounded queue.  Non-blocking
                             reads and reaps only: no buffer write, no Lisp,
                             no allocation beyond the fixed queue.  Called
                             next to compilation_poll() in main.c:168 AND
                             from tty.c's idle path, so output keeps flowing
                             while kg waits for a key.
                             Publishes events; never delivers them.

drain (subscriber)           queue -> filter, or queue -> buffer.  Runs only
                             from kg_event_drain_safe(), i.e. only at
                             main.c's three named safe points.  This is the
                             only place kg_buffer_replace() and Fe are
                             reached from.
```

That split is the whole reason a Lisp filter cannot observe a half-applied
edit or run inside `read_key_byte()`.

#### Event integration

Add to `enum kg_event_kind` in `src/event.h` — the header already reserves
the decision, saying the kinds are "that slice's ... to keep the union's
cost off this module's budget until it is needed":

```c
KG_EVENT_PROCESS_OUTPUT,   /* this process has undelivered output queued */
KG_EVENT_PROCESS_EXIT,     /* this process has been reaped */
```

```c
struct kg_event_process {
	struct kg_process_handle process;
	struct kg_buffer_handle buffer;   /* target, may no longer resolve */
	/* EXIT only; zeroed for OUTPUT. */
	bool exited;
	int code;                         /* exit code, or signal number */
};
```

Identity only: **no byte range and no text**.  A byte range would name
storage the queue is free to have overwritten by delivery time, and text
is what `event.h`'s opening contract forbids outright.  The drain reads
whatever the process's queue holds *at delivery* and empties it, which
also makes several output events for one process collapse into one
non-empty delivery for free.

`event_resolution()` (`src/event.c:613`) resolves a buffer handle.  Give
it a process arm: for both new kinds, resolution is whether the *process
handle* still resolves, since that is the identity the subscriber
dispatches on.  A `KG_EVENT_PROCESS_EXIT` for a process whose slot has
been released is still delivered as `KG_EVENT_RESOLVED_GONE`, matching
what `KG_EVENT_BUFFER_KILLED` already does.

Both kinds are **lifecycle** events, not droppable change events: an exit
that queue pressure swallowed would strand a sentinel forever.  They go
through `kg_event_reserve_lifecycle()` / `kg_event_publish_lifecycle()`.
A refused reservation for `KG_EVENT_PROCESS_OUTPUT` is harmless — the
bytes stay queued and the next poll republishes — but a refused
`KG_EVENT_PROCESS_EXIT` must be retried by the next poll rather than
dropped, so the entry keeps an "exit not yet published" flag.

#### Lisp API

| Native | Semantics |
|--------|-----------|
| `(start-process name buffer program &rest args)` | Explicit argv spawn; returns a process object |
| `(start-shell-command name buffer command)` | Intentional `/bin/sh -c`; returns a process object |
| `(process-live-p proc)` | Generation check + status check |
| `(delete-process proc)` | SIGTERM → bounded wait → SIGKILL; release slot |
| `(process-buffer proc)` | The target buffer object, or nil if it is gone |
| `(set-process-filter proc fn)` | `fn` is called `(fn proc string)` at the drain |
| `(set-process-sentinel proc fn)` | `fn` is called `(fn proc event-string)` at the drain |
| `(process-status proc)` | `run`, `exit` or `signal` |

Process objects are `FeTFex0` values over pool records, exactly as buffer
and marker objects are (`src/lisp_obj.[ch]`) — the same pool, a new record
kind.  Reuse it; do not build a second pool.

**Never expose a PID as identity.**  The handle is the only identity
token.  `process-status` reports what became of the child, not who it was.

**Filter semantics.**  When a filter is set, output is **not** appended to
the process buffer — the filter owns it, as in Emacs.  Clearing the filter
(`(set-process-filter proc nil)`) restores auto-append.  Say so in the
docs; it is the one place a reader is most likely to assume Emacs
behaviour and be right, so the code must actually match.

**Error containment.**  A filter or sentinel that errors is reported
through the status line and does not stop the other subscribers, the other
processes, or later deliveries — the same containment `run-hooks` got in
`src/lisp_hooks.c`, including saving and restoring `state.frame`.  Copy
that pattern; it is there because getting it wrong killed the editor.

### Phase 7a notes — what the review of the C slice found

The C infrastructure (tasks 1–4) landed in `22765f1..fe4217e`, +85 scc.
Reviewing it produced four things worth keeping.

1. **`execvp(NULL, ...)` segfaults**, confirmed by running it: glibc
   dereferences the name to search `PATH` before it can fail.  An `argv`
   whose first element is NULL therefore forked a child whose only act was
   to die on SIGSEGV, which a caller would read back as "the program
   crashed" rather than "the request was malformed".
   `spawn_request_is_runnable()` now refuses it alongside the
   argv-and-command case.  This matters for Phase 7b, which builds `argv`
   out of Lisp arguments.

2. **The tick budget and the pipe capacity are coupled, silently.**
   `poll_entry()` closes the output fd as soon as the child is reaped,
   where `compilation_poll()` instead waits for `pipe_eof && child_reaped`
   and never closes on reap alone.  That looks like it should lose the
   bytes still sitting in the pipe, and it does not — but only because
   `KG_PROCESS_TICK_BUDGET` (64 KiB) is greater than or equal to the
   default pipe capacity (64 KiB), so one poll always drains the pipe dry
   before the reap check runs.  Two runs of a 5 MiB producer with a drain
   between every poll delivered all 5 120 000 bytes.  **Lower the budget
   or raise the pipe size and output starts disappearing with no
   truncation marker.**  Left as-is with this note rather than
   restructured; if either constant moves, adopt compile.c's condition.

3. **Do not mistake the output cap for a bug.**  The same 5 MiB producer
   polled *without* draining delivers exactly 262 165 bytes — 256 KiB plus
   the 21-byte truncation marker.  That is `KG_PROCESS_OUTPUT_MAX` and the
   drop-oldest policy working as specified.  A test for output fidelity
   has to drain on every poll or it measures the cap instead.

4. **Exit is published as soon as the child is reaped**, which for Phase
   7b means an exit event can reach the queue while output events for the
   same process are still ahead of it.  Ordering within one drain is
   subscriber-registration order, so a sentinel registered before the
   output-committing subscriber would run before the process's last output
   was appended.  Emacs' contract is the other way round: the filter sees
   everything before the sentinel fires.  **Phase 7b must flush a
   process's remaining output before invoking its sentinel**, not rely on
   registration order to arrange it.

Separately, and not caused by this slice: `make fuzz-keypress` had been
failing to link since Phase 2, because `src/lisp_obj.c` calls
`win_buffer`, `win_shows_buffer`, `buf_create_named` and
`buf_kill_buffer`, none of which `FUZZ_SRCS` provides.  `make check` does
not build the fuzz targets, so every gate run at the time reported green
and `.ci/ci-09` was the only thing that would have caught it.  Fixed with
four stubs in `test/fuzz_stubs.c`, where `buf_open_file()` and
`win_split_horizontal()` already live.  **A slice that adds a `src/*.c`
must build the fuzz targets, not just `make check`.**

### Tasks — one commit each

1. **Explicit-argv spawn** in `src/process.[ch]`.
2. **`src/prochandle.h`** and the table in `src/process_table.[ch]`:
   spawn, poll, reap, release, shutdown, slot reuse, full-table refusal.
3. **Event kinds, payload, resolution arm** in `src/event.[ch]`.
4. **Poll wiring** in `main.c` and `tty.c`; drain subscriber that commits
   output to the buffer (no Lisp yet).
5. **Lisp natives** — process objects in the existing pool, then the eight
   natives.
6. **Filters and sentinels**, with `lisp_hooks.c`'s frame discipline.
7. **Shutdown cleanup** and `WITH_LISP=0` non-regression.

### Tests

Native (`test/test_process_table.c`), for anything that is pure table
logic: slot reuse, generation checking, full-table refusal, output cap and
truncation flag, status decoding, exit-publication retry.

PTY (`test/pty/`), for anything involving real children, real timing or
real Lisp:

- **Explicit argv is not a shell**: `(start-process "e" buf "/bin/echo" "a; touch pwned")`
  writes the metacharacters literally and creates no file.
- **`cwd`**: the child runs in the directory it was given.
- **Process groups**: SIGTERM to the group reaps a grandchild.
- **Two concurrent children**: both produce output, both are reaped, and
  neither's output lands in the other's buffer.
- **Output cap**: a child producing more than the cap yields exactly one
  truncation marker per overflow run.
- **Killed process buffer**: killing the buffer mid-run discards output,
  leaves the process running, and still fires the sentinel.
- **Stale handle / PID reuse**: a handle to a released slot never resolves
  to the new occupant.
- **Delivery is at a safe point**: output produced while a prompt is open
  is delivered after the prompt closes, not during it.
- **Filter and sentinel errors** are contained: a second process's
  callbacks still run, and the editor survives an error raised *after* a
  `(run-hooks)` in the same session (the `state.frame` regression).
- **Shutdown**: no child survives kg's exit.
- Both `WITH_LISP` configurations.

### Phase 7b notes — and what the full CI runner found

The Lisp slice landed in `8656270..f1e8b97`, +133 scc, bringing Phase 7 to
220 of its 230-unit row.  Requirements (a)–(d) are met:
`lisp_process_event_cb()` calls `deliver_filter_output()` before
`deliver_sentinel()` for both event kinds rather than relying on
subscriber order; `set-process-filter` sets a flag the auto-append
subscriber checks; `call_process_callback()` is `lisp_hooks.c`'s frame
discipline verbatim; no native takes or returns a pid.

**`make check` and every ratchet were green while five CI lanes were
red.**  That is the finding worth keeping, and it is the second time in
this plan that a slice passed the gates it ran and failed ones it did
not.

| Lane | Cause | Introduced by |
|------|-------|---------------|
| ci-03 gcc `-fanalyzer` | NULL deref of `binding_for()` | Phase 7b |
| ci-11 `-funsigned-char` | reclaim test raced the kernel's reap order | Phase 7a |
| ci-06 IWYU | `lisp_process.c` include list | Phase 7b |
| ci-04 ASan | `lisp_search()` leaks its pattern on a raise | **Phase 3** |
| ci-09 fuzz link | `lisp_obj.c`'s four undefined symbols | **Phase 2** |

Those are fixed.  The two marked in bold were confirmed against the
pre-Phase-7 commit (`0123432`), so Phases 2–6 of this plan were signed off
with those lanes already failing.  **Running `make check` plus the
ratchets is not sign-off.  `.ci/run-ci-steps.sh --parallel` is.**

#### ci-06 (IWYU) is still red beyond Phase 7's share — open

Fixing `lisp_process.c`'s own include list does not make the lane green.
IWYU reports findings in sixteen files, including `src/register.[ch]`
(Plan 05) and `src/compile_nav.[ch]`, `src/compile_parse.h` (older still),
so the lane predates Plan 06 entirely.  The rest are the Lisp adapter's:
`lisp_core.c`, `lisp_buffer.c`, `lisp_word.c`, `lisp_cmd.c`, `lisp_io.c`,
`lisp_obj.[ch]`, `lisp_hooks.c`, `lisp_search.c`, `process_table.[ch]`.

The work is mechanical but broad, and it is not all safe to apply blind —
IWYU wants `#include "lisp.h"` dropped from `lisp_core.c`, which is the
header the `WITH_LISP=0` stubs share, so each removal needs both
configurations rebuilt behind it.  Three redundant `<stddef.h>` includes
were removed as part of the Phase 7 fixes; the remaining thirteen files
are a cleanup slice of their own and are not priced in Phase 7 or 8.

#### ci-05 (MSan) is still red, and is not Phase 7's — open

`test_lisp`'s `test_recursion_depth` asserts that `(deep 5000)` fails with
Fe's `GC stack overflow`.  Under `-fsanitize=memory
-fsanitize-memory-track-origins=2 -O0` the **C stack** goes first, at
`fe/fe.c:1757` in `Evaluate`.  Confirmed red at `0123432` too, so it
predates Phase 7.

The cause is structural, not a test bug.  `fe/fe.c`'s own comment at the
`GcStackSize` declaration says it: *"A self-recursive Fe call costs
several slots, so this also bounds usable recursion depth, at roughly 450
frames."*  Recursion is bounded **incidentally**, by how many GC slots a
call happens to consume, not by depth — so the margin between Fe's limit
and the real C stack is whatever the compiler's frame size makes it.  A
build with fat frames (any sanitizer, `-O0`, a debug build) crashes where
a release build errors.

The fix is an explicit recursion-depth counter in `Evaluate`, bounded well
below the point any plausible frame size can exhaust the stack, reported
as an ordinary Fe error.  That is a `fe/` submodule change: fe's own
numbered CI, `doc/fe-upstream.md`, and a pin move in a separate kg commit.
It is not Phase 7 or Phase 8 work and is not priced in either row.

---

## Phase 8 — Package hygiene and proof packages

### Problem

kg has `load` (`src/lisp_io.c:425`), XDG init-file resolution, and a
single hard-coded package directory (`<config>/kg/lisp/NAME.fe`).  There
is no `provide`/`require`, no load-path control, no docstrings, and no
published API contract.

### Scope decision — this phase is over-subscribed at 90 units

The original task list is: `provide`/`require`/`featurep`, bounded
`load-path`, cycle detection, source line numbers in errors, docstrings
plus `describe-function`, `doc/lisp-api.md`, and four proof packages.
Two of the four packages are unbuildable (corrections 7 and 8 above), and
docstrings need a bounded symbol→string table plus a `describe.c` entry
point that does not exist.  Ninety units does not buy that.

**What Phase 8 delivers:**

1. `provide` / `require` / `featurep` with a bounded feature table.
2. Bounded `load-path`, defaulting to `<config>/kg/lisp/`.
3. Load-cycle detection.
4. `doc/lisp-api.md`, versioned, covering the whole surface Phases 2–7
   shipped.
5. **`auto-fill-mode` as the proof package** — it is the one from the
   original table that is buildable today: `after-change-functions` landed
   in Phase 5, and column arithmetic and insertion are all it otherwise
   needs.  It proves the hook path, one editing transaction per change,
   and package loading through `require`.

**What Phase 8 does not deliver, and why — record this in the plan, do
not silently drop it:**

- `whitespace-mode` — needs decoration natives (correction 8).
- `conf-mode` — needs the Phase 6 mode registry (correction 7).
- `grep` — optional in the original table; it is the natural second proof
  package once Phase 7 lands, and is the first thing to add if Phase 7
  comes in under its 230.
- Docstrings and `describe-function` — needs a bounded symbol→docstring
  table and a `describe.c` entry point.  Worth doing; not affordable here.
- Source line numbers in errors — needs Fe to carry position information,
  which is a submodule change with its own pin move.

Each of these gets a row in the plan's status section naming what it
needs, so the next slice can price it instead of rediscovering it.

### Design

| Feature | Semantics |
|---------|-----------|
| `(provide feature)` | Register a symbol in the bounded feature table |
| `(require feature &optional filename)` | No-op if already provided; else load `filename` or `feature` through `load-path`; error if the feature is still not provided afterwards |
| `(featurep feature)` | Check without loading |
| `load-path` | Bounded list of directories `require` searches, in order |
| Cycle detection | `require` re-entered for a feature already being loaded is an error naming the cycle, not a stack overflow |

`load-path` is a bounded C-side array with `LISP_MAX_LOAD_PATH` entries,
each `PATH_MAX`, manipulated by natives — not a Fe list, which the
adapter would have to re-validate on every `require`.  `load`'s existing
`LISP_MAX_LOAD_DEPTH` still caps nesting; cycle detection is about
identity, not depth, and the two limits stay separate.

### Proof package requirements

`auto-fill-mode` ships as `lisp/auto-fill.fe`, and gets:

- an isolated-HOME PTY test using `config_files:` to plant both the
  package and an `init.fe` that `require`s it;
- a test that typing past `fill-column` breaks the line, and that the
  break is **one** undo step (the editing-transaction claim);
- a `WITH_LISP=0` non-regression: the package is absent and the editor
  behaves exactly as before.

**Rule, unchanged and load-bearing:** if a proof package needs private C
knowledge, improve the public adapter rather than adding a
package-specific native.  A native named after the package is the signal
that the adapter is missing something.

### Tests

- `require` loads a file; a second `require` is a no-op (assert the file
  is evaluated once, not just that it did not error).
- `require` of a file that does not `provide` → error.
- Cycle: A requires B, B requires A → error naming the cycle.
- `featurep` is false before and true after.
- `load-path` order decides which of two same-named files wins.
- A `load-path` full of non-existent directories fails cleanly.
- `auto-fill-mode`, as above.

---

## Completion gate for sub-plan D

- Explicit-argv spawn never passes through `/bin/sh`, proven by a PTY case
  in which shell metacharacters survive as literal bytes.
- The process table is bounded, handles are generation-checked, and no PID
  is ever exposed as identity.
- Output is committed through the edit gateway with `KG_EDIT_INTERNAL`,
  from the drain only; polling touches no buffer and no Lisp.
- Filters and sentinels run only from the drain, and an error in one
  contains to that callback.
- No child survives kg's exit.
- `provide`/`require`/`featurep` with bounded `load-path` and cycle
  detection.
- Versioned `doc/lisp-api.md` is published and covers Phases 2–7.
- `auto-fill-mode` is green with an isolated-HOME PTY test and a
  `WITH_LISP=0` non-regression.
- What was cut is written down with what it needs, not silently absent.
- Both Lisp configurations, native and PTY suites, `docs-check`,
  `header-check`, `lisp-include-check`, `format-check`, the static and
  sanitizer lanes, and the full CI runner pass **without ratchet raises**.

---

## Status — sub-plan D closed 2026-08-03

Phase 7 spent 220 of its 230 scc units, Phase 8 spent 41 of its 90.  The
tree ends at 5400 against the 5500 cap, so Plan 06's budget held with no
raise.

Every completion-gate item above is met, with two clarifications:

- "No child survives kg's exit" and the process-group rule are proven by
  `test_delete_process_kills_the_whole_group` and
  `test_shutdown_leaves_no_survivor` in `test/test_process_table.c`.  Both
  fail when the group signal is replaced with `kill(e->pid, ...)`, which
  is how they were checked — neither slice had written them.
- `doc/lisp-api.md` covers Phases 2–**8**, including `require`/`provide`.

### Still open, deliberately, with what each needs

| Item | Needs |
|------|-------|
| ci-05 (MSan) | an explicit recursion-depth bound in `fe/fe.c`'s `Evaluate`; submodule change + pin move (see above) |
| ci-06 (IWYU) | a cleanup slice over ~13 files, most predating Plan 06 (see above) |
| whitespace-mode | decoration natives |
| conf-mode | the Phase 6 mode registry (`define-derived-mode`, `defvar`) |
| docstrings, `describe-function` | a bounded symbol→docstring table and a `describe.c` entry point |
| source line numbers in errors | Fe carrying position information |
| grep package | nothing — Phase 7 landed the process table it wanted |

### The lesson this sub-plan paid for twice

Both Phase 7 slices, and Phases 2–6 before them, passed `make check` and
every ratchet while leaving CI lanes red — a fuzz target that had not
linked since Phase 2, an ASan leak since Phase 3, and three lanes Phase 7
broke itself.  `make check` builds neither the fuzz targets nor the
sanitizer configurations, and `make docs-check` cannot see an
undocumented Lisp API.

**`.ci/run-ci-steps.sh --parallel` is the sign-off.  Run it at the start
of a slice too, so a lane someone else left red is not mistaken for
yours.**
