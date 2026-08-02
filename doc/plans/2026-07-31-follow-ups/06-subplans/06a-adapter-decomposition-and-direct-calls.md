# Sub-plan 06-A — Adapter decomposition and direct Fe calls

Phases 0 and 1 of Plan 06.  No external prerequisite beyond the
already-complete Plans 01–04.

---

## Phase 0 — Decompose the adapter without behavior change

### Problem

`src/lisp.c` is 2,207 lines: 4× the 520-line per-file scc cap, and
growing.  It mixes Fe-internal lifecycle, string/conversion helpers,
editor-facing native functions, command registration, the Lisp prelude,
and the public `kg_lisp_*` API in one file.  Only `src/lisp.c` may
include `fe.h` — that rule is good and must be preserved, but the file
must be split for it to stay maintainable.

### Design

Split into self-contained adapter implementation files, each with its
own private header:

| New file | Responsibility | Approximate current lines |
|----------|---------------|--------------------------|
| `src/lisp_core.c` | Arena, context, frame enter/leave, error translation, scratch cleanup, step-budget polling, interrupt, `lisp_state`, `lisp_command` table | L1–181, L1873–2155 (~460 lines) |
| `src/lisp_prelude.c` | The Lisp-in-Fe prelude string and `register_natives()` binding table | L1546–1872 (~330 lines) |
| `src/lisp_string.c` | String/conversion natives: `format`, `concat`, `substring`, `string=`, `char-to-string`, `string-to-char`, `string-length` | L908–1159 (~250 lines) |
| `src/lisp_buffer.c` | Buffer/position/region natives: `point`, `point-min`, `point-max`, `goto-char`, `goto-line`, `line-number-at-pos`, `current-column`, `mark`, `set-mark`, `deactivate-mark`, `region-*`, `buffer-substring`, `char-after` | L424–809 (~385 lines) |
| `src/lisp_word.c` | Word/thing natives: `forward-word`, `backward-word`, `bounds-of-thing-at-point` | L810–907 (~100 lines) |
| `src/lisp_io.c` | Output natives (`message`, `insert`, `buffer-name`) and file loading (`load`, XDG path, load-depth) | L182–423, L1270–1412 (~380 lines) |
| `src/lisp_cmd.c` | Command/keybind natives: `define-command`, `remove-command`, `global-set-key`, `global-unset-key`, `command-execute`, type predicates | L1160–1545 (~385 lines) |

Each file `#include`s `fe.h` and a private `lisp_internal.h` that
exposes `lisp_state`, `lisp_frame`, argument helpers, and error
translation — but **nothing** from `fe.h` leaks into `src/lisp.h`.

### Tasks

1. **Create `src/lisp_internal.h`.**  Private header with include guard.
   Contains: `lisp_state` extern, `lisp_frame` struct, argument-reading
   helpers (`lisp_require_*`, `lisp_to_string`, `lisp_to_int`, etc.),
   scratch-buffer helpers, error/frame macros.  Only `src/lisp_*.c` may
   include it.

2. **Extract modules one at a time.**  Each extraction is a separate
   commit: move functions, add includes, verify `make check` and
   `make complexity-check` pass.  Order: `lisp_string.c` first (fewest
   dependencies), then `lisp_word.c`, `lisp_buffer.c`, `lisp_io.c`,
   `lisp_cmd.c`, `lisp_prelude.c`, and finally rename the remainder to
   `lisp_core.c`.

3. **Update the Makefile.**  Add each new `.c` to `SRCS` / the link
   line.  Add `lisp_internal.h` to the header-check list.

4. **Add a grep-able CI check.**  Enforce that only `src/lisp_*.c` files
   include `fe.h`.  Add to `.ci/ci-01-*.sh` or `make check`.

5. **Move `cmd_eval_last_sexp` declaration.**  Its declaration is in
   `lisp.h` but its home is `cmd.h` (it is a command).  Move the
   declaration, update callers, verify `make header-check`.

6. **Verify neutrality.**  No behavior change: same test suite, same
   PTY results, same `WITH_LISP=0` build, same complexity total (must
   decrease per-file).

### Tests

- `make check` (both `WITH_LISP=1` and `WITH_LISP=0`) green.
- `make complexity-check`: every new file under 520 lines.
- `make header-check`: `lisp_internal.h` compiles standalone.
- New CI grep: `fe.h` and `lisp_internal.h` appear only in `src/lisp_*.c`
  (`fe.h` additionally in `lisp_internal.h` itself).

### Phase 0 notes

The line ranges in the table above are the planning estimate; the split
differs where the estimate could not be exact:

- The `WITH_LISP=0` half of the public API is the `#else` side of
  `lisp_core.c`, exactly as in the pre-split file; `copy_result` sits
  outside the guard because both configurations call it.  A
  `WITH_LISP=0` build compiles only `lisp_core.c`.
- `evaluate_prelude()` lives in `lisp_prelude.c` with the prelude string
  it evaluates, keeping the string private to the module.
- The table lists `format` under `lisp_string.c`, but its own line ranges
  (L182–423) place the formatter with `message`/`insert`; it stays in
  `lisp_io.c` so `native_message` need not export the formatter.
- The Makefile derives `SRCS`, `EXTRA_lisp`, `FUZZ_SRCS` and the perf build
  from one `LISP_SRCS` list.  The include rule is `make
  lisp-include-check`, wired into `make check`, allowing `fe.h` and
  `lisp_internal.h` only in `src/lisp_*.c` (`fe.h` additionally in
  `lisp_internal.h` itself, which is a standalone header-check unit).
- The per-file ceiling is scc's per-file metric (`SCC_FILE_COMPLEXITY_MAX`);
  every new file sits well under it, and the scc and pmccabe totals are
  unchanged by the split.

---

## Phase 1 — Direct budgeted Fe calls

### Problem

`kg_lisp_run_command()` works by:
1. Setting `state.pending_command = FeGetRoot(cmd->root)`.
2. Evaluating the string `"(internal--run-pending-command)"` via
   `FeEvaluateStringWithOptions()`.
3. Inside `native_run_pending()`, calling
   `FeCall(context, state.pending_command, nullptr, 0)`.

This is a trampoline: the string eval exists only to get Fe's
step-budget/interrupt/GC accounting.  With `FeCallWithOptions()` in Fe,
the command can be called directly.

### Design — Fe side

Add to `fe/fe.h`:

```c
FeObject *FeCallWithOptions(FeContext *ctx, FeObject *callable,
                            FeObject **args, int count,
                            const FeEvaluateOptions *options);
```

Semantics are identical to `FeEvaluateStringWithOptions` minus the parse
step: same step-budget decrement, same interrupt polling, same GC
checkpoint save/restore, same error signalling.  Tested in Fe's own
suite before the kg pin moves.

**Tests (Fe side):**
- Success: call a lambda, verify return value.
- Wrong arity / error propagation.
- Step exhaustion: budget runs out mid-call.
- Interrupt: host interrupt check fires.
- Nested ambient budget: call within a call shares the budget.
- GC checkpoint: roots survive.

Document the divergence in `doc/fe-upstream.md`.

### Design — kg side

1. Replace `kg_lisp_run_command()` body:
   ```c
   FeCallWithOptions(ctx, rooted_fn, nullptr, 0, &opts);
   ```

2. Delete:
   - `state.pending_command`
   - `native_run_pending()`
   - `"internal--run-pending-command"` from `native_bindings[]`
   - The constant source string

3. Retain the current **top-level-only recursion policy**: if a
   Lisp-defined command triggers another Lisp-defined command, it queues
   for the next safe-point drain rather than re-entering Fe.

4. Error isolation: one command's failure does not prevent later queued
   callbacks.

### Tasks

1. **Implement `FeCallWithOptions` in the Fe submodule.**  Commit in
   Fe, run Fe's own suite.
2. **Update `doc/fe-upstream.md`** with the new divergence.
3. **Move the Fe pin** to the new branch head.
4. **Rewrite `kg_lisp_run_command()`** to call directly.
5. **Delete the trampoline infrastructure** (four items above).
6. **Verify all tests.**

### Tests

- All existing PTY Lisp tests pass (eval, command define/execute,
  budget exhaustion, interrupt).
- `WITH_LISP=0` build still links and all non-Lisp tests pass.
- `make check`, `make complexity-check`.

---

## Completion gate for sub-plan A

- Every `src/lisp_*.c` file is under 520 lines.
- `fe.h` is included only by `src/lisp_*.c` (CI-enforced).
- `lisp.h` is Fe-free (no Fe types leak).
- `FeCallWithOptions` is tested in Fe and documented in
  `doc/fe-upstream.md`.
- The string trampoline is gone.
- Both Lisp configurations, native/PTY suites, docs check, and
  `make complexity-check` pass.
