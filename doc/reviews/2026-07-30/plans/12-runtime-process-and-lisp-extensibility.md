# Plan 12 — Runtime adapter, bounded processes, and richer kg Lisp

## Goal

Expose stable editor services to optional Fe packages without moving editor
ownership into Fe or weakening `WITH_LISP=0`.

## Verified baseline

Checked against the tree at `906e48f`. Re-read before implementing.

| Thing | Where | Shape today |
| --- | --- | --- |
| Public Lisp API | `src/lisp.h` (26 lines) | 11 functions, **no Fe types**; the "no `FeObject *` in core headers" rule already holds |
| Native registration | `src/lisp.c:1588 native_bindings[]`, loop at `:1633 register_natives` | 40 natives, `{const char *name; FeNativeFn *fn;}` |
| Prelude | `src/lisp.c:1669-1909 lisp_prelude[]` | ~50 Emacs-shaped forms evaluated at init (`defun`, `let`, `dolist`, `quasiquote`, `interactive`, `thing-at-point`, …) |
| Runtime commands | `src/lisp.c:1462 native_define_command`, `:1504 native_remove_command` | already exist; `lisp_max_commands = 32`, `lisp_command_name_max = 64` |
| Command invocation | `src/lisp.c:2111 kg_lisp_run_command` | `setjmp` frame, then a **source-string trampoline** `"(internal--run-pending-command)"` at `:2113` |
| Budget | `src/lisp.c:150 eval_options` | `step_limit` 1<<20, `poll_interval` 256, `interrupt` → `editor_check_quit_pending` |
| Arena | `src/lisp.c:47` | 1 MiB, `KG_LISP_ARENA_SIZE`-overridable |
| Recovery frame | `src/lisp.c:66-85 struct lisp_state` | exactly **one** `struct lisp_frame` (one `jmp_buf`), `frame_active` guard reporting `"Lisp is busy"` |
| Hooks | — | **none exist**; no `add-hook`, no `run-hooks`, no mode/save/change hook anywhere in `src/` |
| Compilation process | `src/compile.c:16 static struct compilation_state g_compilation` | one at a time; struct at `src/compile.h:17-51` |
| Other spawners | `src/shell.c:199 shell_run`, `src/shell.c:309 shell_run_capture` | two more `fork`/`execl` paths (`:227/:251`, `:346/:379`) |

Corrections to the previous draft of this plan:

- **`FeCallWithOptions()` does not exist and Plan 05 does not add it.** What
  exists is `FeCall(ctx, callable, args, count)` (`fe/fe.h:132`), which takes no
  `FeEvalOptions`, and `FeEvaluateWithOptions` / `FeEvaluateStringWithOptions` /
  `FeEvaluateFileWithOptions`, which do. The trampoline at `src/lisp.c:2145`
  exists *precisely* to borrow the options path: kg stashes the callable in
  `state.pending_command`, evaluates a constant source string, and
  `native_run_pending` (`src/lisp.c:1572`) calls `FeCall` from inside. Adding
  `FeCallWithOptions()` is therefore a **new Fe-side item on kg's pinned
  branch**, not something Plan 05 already delivers. Schedule it explicitly and
  record it in `doc/fe-upstream.md` as another kg divergence.
- **Runtime-defined commands already exist.** `define-command` / `remove-command`
  root the callable via `FeCreateRoot`, refuse to shadow builtins through
  `cmd_static_exists()`, and release the old root only after the new one is
  created. This plan extends that mechanism; it does not invent it.
- The 32-command ceiling is **not silent** — `native_define_command` raises
  `"too many Lisp commands"`. The real complaint is that it is a hardcoded
  `constexpr` duplicated in spirit by `KEYBIND_MAX` 32 (`src/keybind.c:15`),
  with no way to raise either.
- **Compilation does not use `select`/`poll`.** It sets `O_NONBLOCK` on the read
  end (`src/compile.c:195`) and drains with a bounded non-blocking `read()` loop
  (`COMPILATION_READ_CHUNK` 4096, `COMPILATION_TICK_BUDGET` 64 KiB), driven from
  `src/main.c:162` and from `read_key_byte` in `src/tty.c:429`/`:435` so it keeps
  draining while a prompt is blocked. `src/shell.c` is the module that uses
  `poll()`, in `pump_io` (`src/shell.c:94`).
- **A second process client already exists**, so the "only when a second client
  needs it" gate in FINAL.md is already satisfied — see phase 9.
- `cmd_eval_last_sexp` is declared in `src/lisp.h` but implemented in
  `src/cmd.c:579`. Fix that layering wart in phase 1.

## Dependencies

- Plan 05 for Fe correctness/unwind foundations, **plus** the new
  `FeCallWithOptions` item this plan introduces.
- Plan 09 stable buffer handles, before any handle-typed API.
- Plan 10 transactions/markers/deferred events, before callbacks.
- Plan 11 registries, before Lisp-facing commands/modes/variables/completion.

Phases 1 and 9 need none of the above and can start once Plan 04's compilation
cap is fixed.

## Architectural rule

Core editor modules depend on a runtime-neutral adapter interface:

```c
struct runtime_adapter {
	void (*shutdown)(void);
	void (*buffer_killed)(kg_buffer_handle);
	void (*dispatch_events)(void);
	int (*invoke_command)(runtime_command_handle, struct command_context *);
};
```

Implementations: a no-op C adapter for `WITH_LISP=0`, and the Fe adapter for
`WITH_LISP=1`. This is a *preservation* requirement, not a new achievement —
`src/lisp.h` is already Fe-free and `src/lisp.c` already carries a full `#else`
stub block (`src/lisp.c:2179-2230`). Do not regress it.

## Phase 1 — Split `src/lisp.c` internally

`src/lisp.c` is 2232 lines: state, error/frame handling, ~40 natives, string
conversion, the command registry, the prelude, and package loading.

### Files

`src/lisp.c`, `src/lisp.h`, optional private modules `src/lisp_runtime.c`,
`src/lisp_editor.c`, `src/lisp_string.c`, `src/lisp_package.c`, `Makefile`
(`SRCS`, `EXTRA_lisp`).

### Changes

Keep one public `lisp.h` facade. Split by responsibility: context/error/eval
lifecycle; value conversion and string formatting; editor natives; command and
root registry; package/load/prelude.

`AGENTS.md` says "only `src/lisp.c` may include `fe.h`". If the split creates
real `.c` files, that line must be revised **deliberately** to "only
`src/lisp*.c` adapter modules may include `fe.h`", in the same commit, with a
grep-able CI check. The alternative — private `.inc` files included by
`lisp.c` — keeps the rule literal but defeats per-file complexity accounting
(`SCC_FILE_COMPLEXITY_MAX` is 520 and `lisp.c` is the largest `.c` in `src/`).
Pick one and say why.

Also in this phase:

- move `cmd_eval_last_sexp`'s declaration out of `lisp.h` to match its `cmd.c`
  implementation;
- factor the repeated native argument-validation and cleanup idioms
  (`copy_fe_string` + `free` + `command_error`) into shared helpers; the
  `state.scratch` field exists only because those idioms leak on `longjmp`.

## Phase 2 — Controlled callback invocation

### Fe dependency

Add `FeCallWithOptions(FeContext *, FeObject *callable, FeObject *const *args,
size_t count, const FeEvalOptions *)` to `fe/fe.c` on kg's pinned branch,
mirroring `FeEvaluateWithOptions`. Test it standalone in the submodule, commit
there, then bump the pin in a separate kg commit, per the submodule workflow in
the master roadmap.

### Changes in kg

Delete the trampoline once the API lands: `state.pending_command`,
`native_run_pending`, `internal--run-pending-command` and the constant source
string all go away, and `kg_lisp_run_command` calls the callable directly under
the same `eval_options`.

Every callback invocation gets: the top-level step budget and interrupt polling;
a source/callback label for diagnostics; a balanced `FeSaveGC`/`FeRestoreGC`
checkpoint; one recovery frame; event-origin metadata.

### Recursion

`struct lisp_state` holds a **single** `struct lisp_frame` and guards re-entry
with `frame_active` → `"Lisp is busy"`. Nested callbacks therefore need an
explicit decision, made in this phase:

- runtime events are drained only from a top-level safe point;
- a callback may call ordinary editor natives;
- callbacks generated during a callback are queued for the next drain;
- no callback runs while an editor transaction is partial.

Either keep the single frame and rely on queueing (simplest, and consistent with
today's behavior), or make frames a bounded stack. Do not leave it implicit.

### Tests

`test/test_lisp.c` for callback success/error/`C-g`/arena exhaustion, root
redefinition and removal, callback-queues-callback, and re-entry refusal. PTY
with `requires_feature: lisp` and `config_files:` planting `init.fe` for the
interactive paths. A `WITH_LISP=0` build must still link and run.

## Phase 3 — Stable editor object handles

### Files

Fe adapter, buffer/marker/process registries, tests.

Represent objects as opaque Fe values (or conservative tuples) carrying kind,
ID, and generation. Never expose slot indices, raw pointers, or PIDs — note
that today's `native_command` already avoids this by passing names, and the
process API in phase 10 must not regress to PIDs.

Helpers: resolve and type-check; stable printed form; fail with
`wrong-type-argument` or a stale-object condition; release Fe roots on lifecycle
callback.

Objects: buffer; window/view only if useful; marker; process; compiled regex.
Move to Fe per-context type descriptors when Plan 05 phase 10 lands.

## Phase 4 — Editing and search primitives

### Existing surface

The 40 natives already cover positions (`point`, `point-min`, `point-max`,
`goto-char`, `goto-line`, `line-number-at-pos`, `current-column`), marks and
region (`mark`, `set-mark`, `deactivate-mark`, `region-beginning`,
`region-end`), reading (`buffer-substring`, `char-after`,
`bounds-of-thing-at-point`), one mutation (`insert`), motion (`forward-word`,
`backward-word`), strings, type predicates, and `command-execute`. There is no
deletion, no replacement, no regexp, and no `save-excursion`.

### Add in dependency order

1. `delete-region`
2. `replace-region`, or `insert` extended with transaction grouping
3. `save-excursion` — needs markers and unwind
4. `looking-at`
5. `re-search-forward` / `re-search-backward`, surfacing kg's too-complex status
   distinctly from no-match (Plan 03 phase 3 makes that status truthful)
6. `match-beginning` / `match-end` over per-evaluation match data
7. `undo-boundary` or a small `atomic-change-group`

All positions exposed to Lisp stay 1-based codepoint offsets, converted at the
adapter boundary. Do not let Lisp touch `erow` or suppress undo.

Tests: UTF-8 positions, read-only refusal, stale markers, error rollback, one
undo per logical edit, search captures, complexity and cancellation.

## Phase 5 — Buffer APIs

Add `current-buffer`, `buffer-name` (exists, at `src/lisp.c:416` — extend to
take a handle), `buffer-list`, `get-buffer`, `get-buffer-create`, `set-buffer`,
`with-current-buffer`, `kill-buffer`, `buffer-live-p`.

`with-current-buffer` restores unwind-safely over stable handles. Selecting a
buffer for Lisp need not display it; distinguish the runtime's selected buffer
from the active window and define which one hooks see.

Tests: create/switch/kill; stale handle after slot reuse (`MAX_BUFFERS` is 20,
`src/def.h:391`); error and `C-g` inside `with-current-buffer`; two windows on
one buffer; hidden-buffer mutation and undo; `WITH_LISP=0` unaffected.

## Phase 6 — Minibuffer and command APIs

Add `read-string`, `completing-read`, `yes-or-no-p`, the typed interactive specs
from Plan 11, and a `commandp` / `documentation` / `where-is-internal` subset.

`yes-or-no-p` maps onto `editor_confirm_yn()` (`src/def.h:810`), now the single
y/n prompt; `completing-read` maps onto Plan 11 phase 7's session. Do not add a
fourth picker loop.

A runtime callback delivered while the minibuffer is active must not open a
nested prompt: queue it, or reject with a clear error. Note that
`compilation_poll()` already runs from inside `read_key_byte` during prompts
(`src/tty.c:429`), so "an event arrived while a prompt is open" is a real,
reachable state today, not a hypothetical.

Convert completion candidates in bounded batches; do not root an unbounded list.

## Phase 7 — Variables, modes, keymaps, hooks

### Variables

- `symbol-value` / `set` for Lisp globals, after Plan 05 phase 6's unbound
  sentinel (today Fe treats unbound as nil);
- `buffer-local-value` / `setq-local` through Plan 11 phase 6's C registry;
- Lisp-only buffer-local values stored as roots keyed by stable buffer handle,
  released on buffer kill.

### Keymaps

Create a sparse keymap; `keymap-set` / `keymap-unset`; install mode or global
maps through the Plan 11 registry; preserve emergency keys and the `kg -Q`
bypass. This replaces `global-set-key`'s `"C-c <key>"`-only restriction; update
`AGENTS.md` and `doc/kg.1` when it does.

### Modes

`define-derived-mode` in the prelude over a registered mode descriptor;
activation and deactivation through the core registry; declarative syntax and
comment options first. No Lisp callback per displayed byte.

### Hooks

This is entirely new — kg has no hook mechanism at all. Add `add-hook` /
`remove-hook` with a buffer-local/global distinction, safe-point delivery from
the Plan 10 event queue, per-callback budget and error isolation, and a
deterministic order.

Initial hooks: `after-change`, `find-file`, `before-save`, `after-save`,
`major-mode`. Add `post-command` only after measuring it — it fires on every
keystroke and `editor_process_keypress` is already at the complexity cap.

## Phase 8 — Package hygiene

Add `load-path`; `provide`, `require`, `featurep`; docstring storage and
`documentation`; load-cycle detection. `load` already exists
(`native_load`, `src/lisp.c:1367`) with `lisp_max_load_depth` 8 and per-depth
source buffers freed by frame recovery — extend that, do not replace it.

Diagnostics should report file and byte offset. Packages are trusted code with
full editor and process privileges, bounded for responsiveness, **not**
sandboxed; say so in the docs. No network package installation in core.

## Phase 9 — Generalize the process lifecycle

### The second client already exists

kg has three `fork`/`execl` sites, not one:

| Path | Function | Style | Group | Reap |
| --- | --- | --- | --- | --- |
| `M-x compile` | `compilation_spawn` (`src/compile.c:146`) | async, `O_NONBLOCK` + bounded `read()` loop | `setpgid` both sides | `waitpid(WNOHANG)` in `compilation_poll` |
| `M-!`, `M-\|` | `shell_run` (`src/shell.c:199`) | synchronous, `poll()` in `pump_io` | none | blocking, via `shell_waitpid_fn` |
| transcript capture | `shell_run_capture` (`src/shell.c:309`) | synchronous | none | blocking |

All three duplicate pipe creation, `FD_CLOEXEC` handling, `fork`, `execl("/bin/sh",
"sh", "-c", …)`, fd-close discipline and `EINTR` retry. Only compilation puts the
child in its own process group, so `M-!` cannot be group-killed and a shell
command that spawns children can outlive its parent.

So the first deliverable is **not** concurrency: it is one spawn/reap/fd-hygiene
core shared by all three, which is strictly a deduplication and a bug fix.
Concurrency comes second, when phase 10 or Plan 13's project-grep needs it.

### Extraction, not invention

`struct compilation_state` (`src/compile.h:17-51`) already contains most of the
process record. Split it:

```c
/* moves out of compilation_state, unchanged in meaning */
struct editor_process {
	process_id id;
	uint32_t generation;
	pid_t pid;
	pid_t process_group;
	int stdout_fd;
	int stdin_fd;
	enum process_state state;	/* was enum compilation_phase */
	size_t stored_output;
	size_t output_budget;		/* was maximum_output, 8 MiB */
	bool truncated;
	bool pipe_eof;
	bool child_reaped;
	int wait_status;
	kg_buffer_handle output_buffer;
	process_filter_id filter;
	process_sentinel_id sentinel;
};
```

`compile.c` keeps what is genuinely its own: the ANSI state machine, `pending_cr`,
the partial-line buffer, the truncation marker, and the deferred-restart fields
(`restart_pending`, `pending_command`, `pending_directory`,
`pending_source_buffer`) — i.e. it becomes the first *filter*.

Lifecycle contract: spawn with CLOEXEC pipes; non-blocking drain independent of
child exit; cap retained output while draining; cancel by process group; reap
exactly once; close every fd exactly once; queue filter/sentinel events at safe
points; stale handles fail; shutdown drains, cancels and reaps per policy.

Preserve today's observable behavior exactly: finalize only when
`pipe_eof && child_reaped` (`src/compile.c:602`); the escalating
`SIGINT`-then-`SIGKILL` in `editor_kill_compilation` (`src/compile.c:706`); the
bounded 100 × 1 ms reap loop in `compilation_shutdown`; and the deliberate
choice *not* to launch a queued restart from inside `compilation_poll`
(`src/compile.c:677-681`).

No threads.

### Tests

`test/test_compile.c` already stubs `shell_run_capture`, and `shell_waitpid_fn`
(`src/shell.c:75`) plus the `editor_write_fn`/`fsync`/`close`/`rename` seams
(`src/fileio.c:48-51`) are the established injection idiom — reuse it rather
than inventing a mock framework.

Cases: two concurrent children; exit-before-EOF and EOF-before-exit; large and
truncated output; cancellation and repeated kill; buffer killed while running;
callback error; editor shutdown; injected `fork`/`pipe`/`fcntl`/`waitpid`
failures; no zombies or fd leaks under Valgrind (`.ci/ci-03-*`).

## Phase 10 — Bounded process APIs for Lisp

`start-process` with explicit argv and cwd — **not** an implicit shell; a
separate `start-shell-command` for shell interpretation, since all three current
spawn sites hardcode `/bin/sh -c` and Lisp callers deserve the safer default.
Plus `process-live-p`, `delete-process`, `process-buffer`, and filter/sentinel
registration.

Callbacks receive process handles and output chunks or committed-buffer events.
Limit chunk size and queued bytes. Trusted-code note still applies; the bounds
protect responsiveness and memory, not privilege.

## Phase 11 — First-party acceptance packages

1. `whitespace-mode`: decorations + buffer-local option + mode hook.
2. `auto-fill-mode`: after-change hook + transaction.
3. a small config/make derived mode: derived mode + comment/syntax + local map.
4. optionally a process-backed grep mode, after phase 9.

If a package needs private C knowledge, improve the API rather than adding a
package-specific native. Ship each as a PTY case using `config_files:` to plant
the package under the isolated HOME, with `requires_feature: lisp`.

## What not to implement

Full ELPA compatibility; dynamic scope by default; arbitrary C pointer values;
synchronous hooks inside low-level operations; unrestricted overlays or text
properties; a `dlopen` plugin ABI; threads; wholesale Fex import; package
download or verification; bytecode before profiling real packages.

## Documentation

Create a versioned kg Lisp API document covering supported forms, object and
handle lifetime, position units, hook safe points and order, process and
resource limits, errors and conditions, differences from Emacs Lisp, and the
trusted-code statement. It should supersede the ad-hoc list in `README.md`'s
Lisp section (line 154 onward).

Update `doc/fe-upstream.md` for every new Fe divergence (`FeCallWithOptions` at
minimum). Update `AGENTS.md` if the `fe.h` inclusion rule or the `C-c <key>`
binding restriction changes.

## Commit sequence

1. `lisp.c` split and shared native helpers; `cmd_eval_last_sexp` declaration fix.
2. `FeCallWithOptions` in the Fe submodule; pin bump as its own kg commit.
3. delete the trampoline; document the recursion policy.
4. shared spawn/reap core; compile and shell as its first two clients.
5. process group for the shell paths (behavior fix, own commit).
6. stable handles.
7. editing/search primitives, one at a time.
8. buffer APIs.
9. minibuffer/command APIs over Plan 11's session.
10. variables, keymaps, modes.
11. hooks and the event queue.
12. package hygiene.
13. process handles and filters/sentinels for Lisp.
14. first-party packages.

## Acceptance

- no core header includes Fe types (already true — keep it true);
- the same registry, process and marker code works with Lisp off;
- callbacks cannot observe partial edits;
- stale handles fail safely;
- every Fe root, process and fd has one owner and one release point;
- one spawn/reap implementation, used by compilation and both shell paths;
- two first-party packages run without private C hooks.

```sh
make check
make WITH_LISP=0 clean all check
make WITH_LISP=1 clean all check
.ci/run-ci-steps.sh --parallel
```

## Landed / deferred

Phases 9 and 10 were scoped as one session on `stricter-emacs-adherence`,
in five commits (`b640625` .. `4752aed`), deliberately without the Fe
runtime work: phases 1-8 and 11 wait on foundations that are not there
yet (plan 10's event queue is its phase 8, plan 11's phases 3 and 8),
and phase 10's process API for Lisp is downstream of phase 3's handles.

### Landed

- **Phase 9, the deduplication.**  The baseline said three `fork`/`execl`
  sites; there are **two**.  `shell_run_capture()` — the transcript
  capture the table lists at `src/shell.c:309` — was deleted as dead
  code in plan 11's oversight pass, and left behind a `CAP_DEFAULT`
  `#define`/`#undef` pair with nothing between them, now also gone.  The
  two survivors, `compilation_spawn()` and `shell_run()`, duplicated
  pipe creation, `FD_CLOEXEC`, `fork`, the `/dev/null` redirections,
  `execl("/bin/sh", "sh", "-c", …)` and the reap.
  `src/process.h` is the shared core: a `struct kg_spawn_request`
  (command, directory, stdin descriptor, where stderr goes, whether the
  read end is non-blocking), `kg_process_spawn()`, the two reaps, the
  group signal, and the CLOEXEC pipe and fd-close helpers.  Each caller
  keeps its own I/O strategy, which is what actually differs:
  compilation's `O_NONBLOCK` drain and shell.c's `poll()` pump were not
  touched by the extraction.
- **Phase 9, the process-group gap.**  Only compilation called
  `setpgid`, so a shell command could not be group-killed and anything
  it started outlived it.  The shared spawner does it on both sides of
  the fork, so `M-!` and `M-|` children lead their own groups now.
  kg's one disposal path for such a child — the allocation failure in
  `pump_io()`, after which both pipes are closed and the reap would
  otherwise block behind a `sleep 60` — kills the group first.
  `test/test_shell.c` pins the rest: a grandchild is in the group and
  dies with it, a group id of 0 is never passed to `kill()`, both pipe
  ends are CLOEXEC, stdin defaults to `/dev/null`, an unenterable
  directory exits 127 after the fork, and the non-blocking reap reports
  running / gone / already-gone.  `test/test_compile.c` has the
  end-to-end case: `C-c C-k` on a compilation that backgrounded a
  `sleep` kills the sleep too — in two presses, because a
  non-interactive shell starts a background job with SIGINT ignored, so
  the escalation to SIGKILL is what ends the run.
- **The reap seam moved with the reaping.**  `shell_waitpid_fn` is
  `kg_process_waitpid_fn` and serves both callers; test_shell's EINTR
  and permanent-failure injections are unchanged in substance.  Raw wait
  statuses no longer leave `process.c`: `struct kg_process_status`
  (exited, exit_code, signal_number) is decoded in one place instead of
  in the two `WIFEXITED` ladders, which is also what let both callers
  drop `<sys/wait.h>` honestly rather than because glibc happens to
  re-export the macros through `<stdlib.h>`.
- **Self-funded.**  scc 4238 → 4223 with `SCC_COMPLEXITY_MAX` lowered at
  each commit and never raised: compile.c 149 → 129, shell.c 123 → 98,
  process.c 30.  Two simplifications inside the subject paid for the new
  module: `shell_run()` no longer dups its pipe ends before pumping them
  (they were CLOEXEC copies of CLOEXEC fds), and `pump_io()` fills both
  `pollfd` slots every round instead of carrying a count and an index —
  `poll()` ignores a negative fd, which is exactly the "this side is
  closed" case the count existed for.  The second was found by
  `gcc -fanalyzer`, which lost track of the count once the fds came from
  another translation unit and reported an over-read of `pfd[2]`.  With
  the descriptor bookkeeping gone, so are shell.c's two
  `-Wanalyzer-fd-*` suppressions.

### Deferred, with the pickup point

- **Phase 10 (process APIs for Lisp), and phases 1-8 and 11.**  All of
  them need the Fe-side foundations this session was scoped out of:
  `FeCallWithOptions` on kg's pinned branch (phase 2), plan 10 phase 8's
  event queue for filters and sentinels, plan 11 phase 3 for keymaps and
  built-in key names, and phase 3's stable handles before any handle-
  typed API.  Nothing in phases 9's result blocks them; `process.h` is
  the interface a `start-process` native would extend.
- **Concurrency (a process table).**  Not built, and deliberately: the
  plan's own gate is "a second client", and the second client is still a
  *second call site*, not a second simultaneous child.  `compile.c` still
  owns exactly one `struct compilation_state`, and `shell_run()` is
  still synchronous.  The record to split out when that changes is the
  one the plan lists — the process fields of `compilation_state` — and
  the fields are already the same names.
- **A cancel path for `M-!`.**  There is none: kg sits inside
  `pump_io()`'s `poll()` and does not read the keyboard, so `C-g` cannot
  reach it, and no PTY case could assert a cancellation that has nothing
  to cancel it with.  The process group is the prerequisite for adding
  one — a group-directed signal from a child that shared kg's group
  would have signalled the editor — but the pump would also have to poll
  kg's input fd, which is a bigger change than a deduplication.  kg(1)
  now says the command cannot be interrupted rather than leaving it to
  be found out.
- **An injected `fork`/`pipe`/`fcntl` failure.**  The plan asks for
  these; only the reap has a seam today.  Adding three more function
  pointers to exercise error paths that are three lines each was judged
  not worth the surface, so `kg_process_spawn()`'s failure return is
  covered by inspection and by the callers' `goto fail`, not by a test.
