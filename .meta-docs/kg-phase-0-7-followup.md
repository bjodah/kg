# kg Phase 0–7 Follow-Up Plan

**Status:** Written 2026-07-17, reviewing commits `bc3512f`–`2d4a0ba` on branch `stricter-emacs-adherence`
**Test baseline:** `make check` passes 99/99 PTY tests and all native tests as of HEAD.
**Audience:** Junior engineer, with senior review at escalation points called out below.
**Purpose:** Review what each of the eight completed phases actually implemented, identify gaps
between intent and implementation, and prescribe the narrowest follow-up tasks to close them.
**Out of scope:** Regex dialect work, Fe API changes, Phase 8 (test gaps), and Phase 9
(maintainability) are separate tasks; they appear here only when required as a prerequisite.

Read `kg-implementation-plan.md` first. This document refers to its section numbers throughout.

---

## How to use this document

Each section follows:

1. **What was done** — a faithful summary of the commit.
2. **What is missing or wrong** — concrete code-level issues with file and function names.
3. **Tasks** — atomic, individually reviewable units of work.

Task identifiers use the form `F<phase><letter>`, e.g. `F1a`, `F2b`.
Cross-cutting gaps use `FC<n>`.

Required checks before requesting review on any task:

```sh
git submodule update --init --recursive
make format-check
make check
make clean && make check WITH_LISP=0
.ci/run-ci-steps.sh
```

---

## Phase 0 — Baseline

### What was done

`bc3512f` fixed formatting drift in `test/test_compile.c` and `test/test_localvars.c`, added
`--recurse-submodules` to both GitHub Actions workflows, and corrected the tag and commit hash
in `doc/fe-upstream.md`.

### What is missing or wrong

**F0a — No pre-build assertion for submodule files.**
The plan (§5.3) called for a cheap sanity check such as:

```sh
test -f fe/fe.c
test -f fe/tiny-regex-c/re.c
```

The workflow adds `--recurse-submodules` at checkout, but nothing asserts the resulting tree
is correct before compilation. A misconfigured gitmodule URL would produce a confusing compiler
error. The Makefile already emits a readable error when `fe/fe.c` is absent; strengthen it
with an explicit `$(if $(wildcard fe/fe.c),,$(error …))` guard, or add a
`.ci/ci-00-submodules.sh` pre-step that runs those two `test -f` assertions.

**F0b — Baseline test results were never recorded.**
The plan (§5.4) explicitly required recording what failed before implementation began so that
regressions can be distinguished from pre-existing failures. No such record exists.

### Tasks

| ID | File | Action |
|---|---|---|
| F0a | `Makefile` or `.ci/ci-00-submodules.sh` (new) | Add `test -f fe/fe.c` and `test -f fe/tiny-regex-c/re.c`; emit a clear error with the remedy command if either is missing. |
| F0b | `doc/BASELINE.md` (new, documentation only) | Record `make check` and `.ci/run-ci-steps.sh` output on the commit immediately before `bc3512f`, annotating any known pre-existing failures. |

**Escalation:** None.

---

## Phase 1 — Signal-safe resize

### What was done

`56366f4` introduced `static volatile sig_atomic_t pending_resize` and made `handle_sig_winch()`
set only that flag (correctly saving/restoring `errno`). `editor_process_pending_signals()` was
added; it clears the flag in normal context and calls `update_window_size()`. `sigaction()`
replaced `signal()` for handler installation. All four read paths (`editor_read_key`,
`editor_read_raw_byte`, `editor_read_key_idle`, `get_cursor_position`) retry on `EINTR`,
calling `editor_process_pending_signals()` inside the retry loop and then
`editor_refresh_screen()` when the function returns 1. Two PTY YAML cases cover
resize-while-idle and resize-while-split-window. A native test (`test_tty.c`) verifies the
signal flag lifecycle.

### What is missing or wrong

**F1a — `editor_process_pending_signals()` does not call `win_reflow()` after `update_window_size()`.**
`handle_sig_winch()` previously called both `update_window_size()` and `win_reflow()`. The new
version calls only `update_window_size()`, which reads terminal dimensions but does not update
window pane boundaries or clamp cursors. The callers compensate by calling
`editor_refresh_screen()` when the function returns 1, but that function does not necessarily
call `win_reflow()`. Verify that a resize while a split window is displayed leaves each pane with
correct row/column geometry, not stale values. The existing PTY `resize-split` test only checks
that a subsequent keystroke is saved correctly; it does not exercise pane boundary correctness
after the resize.

**F1b — Resize is processed inside the input readers, including during prompt reads.**
The plan (§6.2) explicitly required deciding "how resize is handled while a minibuffer or yes/no
prompt is active". `editor_read_key()` is used by all prompts. When it receives `EINTR` it calls
`editor_refresh_screen()` immediately, which rerenders the full editor body from inside the
prompt loop. This can produce a partial or inconsistent screen if the prompt's own redraw
happens after. The preferred fix is to store the "needs redraw" state inside
`editor_process_pending_signals()` and let the nearest safe redraw site (the main loop or the
prompt's own loop) issue the refresh. This is a non-trivial design change — **escalate before
coding**.

**F1c — The main loop placement of `editor_process_pending_signals()` is unexplained.**
A resize signal that arrives while `editor_process_keypress()` executes a long command (e.g.
query-replace) will not be processed until the next loop iteration. This is intentional
(signals are coalesced) but there is no comment saying so. The screen may appear garbled for
one frame after a resize during a long command; this is acceptable but should be documented.

**F1d — PTY resize tests are minimal.**
The plan (§6.4) listed six scenarios: one resize while idle, repeated/coalesced resizes, resize
in a split-window layout, resize while a prompt is visible, continued editing and saving after
resize, and dimensions small enough to trigger clamping. Only the first two exist.

**F1e — The native test is incomplete.**
`test_tty.c` tests only that `handle_sig_winch()` sets the flag and `editor_process_pending_signals()`
clears it and returns 1. It does not test coalesced signals (handler called twice before the
function), that `errno` is preserved across the handler, or that a second call returns 0.

### Tasks

| ID | File | Action |
|---|---|---|
| F1a | `src/tty.c` → `editor_process_pending_signals()` | Add a `win_reflow()` call after `update_window_size()`. Add a PTY case with a narrow resize followed by a cursor move across a pane boundary; verify the save result is correct. |
| F1b | `src/tty.c` | **Escalate design** before coding. Once approved: move `editor_refresh_screen()` out of the `EINTR` recovery branch in each reader. Have `editor_process_pending_signals()` return a richer status (e.g. `PENDING_RESIZE`) that the caller propagates; let the main loop or the prompt's own loop act on it. |
| F1c | `src/main.c` | Add a comment next to the `editor_process_pending_signals()` call explaining that resize during a long command is intentionally deferred to the next iteration. |
| F1d | `test/pty/` | Add four PTY YAML cases: (1) two consecutive resizes before any keystroke, (2) resize while a `C-x C-f` path prompt is open, (3) resize to dimensions smaller than any open window (triggers clamping), (4) resize to a larger size after a vertical split. Each must assert a saved-file or `expected_screen_contains` outcome. |
| F1e | `test/test_tty.c` | Add: coalesced-signal test (call handler twice, call `editor_process_pending_signals()` once — must return 1, then 0); `errno`-preservation test; second-call-returns-0 assertion. |

**Escalation:** F1b — redraw-from-prompt design during resize.

---

## Phase 2 — Checked arithmetic

### What was done

`99844fc` added three `static inline` helpers to `src/def.h`: `checked_add_size_t`,
`checked_add_int_size`, `checked_size_to_int`. `editor_rows_to_string()` in `src/buffer.c`
accumulates in `size_t` and uses all three. `editor_row_append_string()` gained
overflow-checked allocation. `kill_ring_set()` and `kill_ring_append()` in `src/yank.c` both
use `checked_add_int_size` to guard `malloc`/`realloc` sizes. Three unit tests were added in
`test/test_buffer.c`.

### What is missing or wrong

**F2a — Callers of `editor_rows_to_string()` are not verified or documented.**
On failure the function sets `*buflen = 0` and returns `NULL`. `editor_save()` passes this
`out_len` value to `editor_write_rows_to_file()`. Verify that `editor_write_rows_to_file()`
treats a `NULL` buffer as an error and does not report a positive byte count on failure. Add a
comment in `editor_rows_to_string()` stating the invariant, and add a test that an overflow
failure leaves `*out_len == 0` at the `editor_save()` level.

**F2b — `kill_ring_set()` destroys the old ring before allocation succeeds.**
`kill_ring_free()` is called *before* `malloc()`. If `checked_add_int_size` succeeds but
`malloc` fails, the old kill-ring content is gone. The plan (§7.4) required: *"On overflow or
allocation failure, preserve the old kill-ring contents and length."* Fix: allocate into a
local variable first; call `kill_ring_free()` only after allocation succeeds; then swap.
Update the overflow test to also cover a successful-arithmetic, malloc-failing injection for
`kill_ring_set()`.

**F2c — Unchecked allocation sites from §7.5 remain.**
Phase 6 addressed `ab_append()` in `display.c`. The following were not touched:

- `src/undo.c` `undo_push()`: `malloc(len + 1)` — wraps for `len == INT_MAX`.
- `src/rect.c` `rect_kill()`: `malloc(len + 1)`.
- `src/rect.c` `rect_yank()`: `malloc(total + 1)`.
- `src/rect.c` region kill: `malloc(killed_total + 1)`.

Each should use `checked_add_int_size` (or `checked_add_size_t`). On overflow the function
must fail gracefully and leave the relevant state unchanged.

**F2d — No allocator-failure test for `editor_row_append_string()`.**
The overflow check is present but there is no test that verifies `row->chars` and `row->size`
remain unchanged when the size arithmetic succeeds yet `realloc` fails. Add a narrow
`realloc`-failure injection local to `test_buffer.c`.

**F2e — Helper placement is unexplained.**
The three helpers are `static inline` in `src/def.h`, which is unconventional for a large
header. Add a comment explaining the placement (avoidance of an extra compilation unit). If the
helpers ever need to be used from a `.c` file that does not include `def.h`, move them to a
dedicated `src/arith.h`.

### Tasks

| ID | File | Action |
|---|---|---|
| F2a | `src/buffer.c`, `src/fileio.c`, `test/test_buffer.c` | Add a comment in `editor_rows_to_string()` documenting the invariant. Verify all callers handle `NULL`. Add a test confirming that `out_len == 0` after an overflow failure. |
| F2b | `src/yank.c`, `test/test_buffer.c` | Fix `kill_ring_set()` to allocate before freeing. Add a test with an injected allocator failure verifying `killring.text` and `killring.len` are unchanged. |
| F2c | `src/undo.c`, `src/rect.c` | Apply `checked_add_int_size` or `checked_add_size_t` to all four sites. Add a unit test per site verifying state is preserved on overflow. |
| F2d | `test/test_buffer.c` | Add a test with injected `realloc` failure in `editor_row_append_string()`; verify `row->chars` and `row->size` are unchanged. |
| F2e | `src/def.h` | Add a comment explaining why the helpers live in the header. |

**Escalation:** F2c for `src/rect.c` — confirm with senior whether `len` or `total` in
rectangle operations can legitimately exceed `INT_MAX`.

---

## Phase 3 — Transactional file opening and reloading

### What was done

`b754b19` rewrote `editor_open()` to use a staging struct (`temp_load_result`) populated by
`load_file_transactional()`, committed by `commit_load_result()`, and cleaned up by
`free_load_result()`. `buf_reload_from_disk()` in `bufmgr.c` also uses this pair.
`load_file_transactional()` treats `ENOENT` as "new empty buffer" and returns error on
permission, I/O, and allocation failures without touching `editor` state. Tests use a new
`test/stubs_buffer.c`.

### What is missing or wrong

**F3a — `load_file_transactional()` temporarily mutates live `editor` state per row.**
During each row append, it saves `editor.row`, `editor.numrows`, and `editor.syntax`, installs
the partially-built `res->row`, calls `editor_select_syntax_highlight()` and
`editor_update_row()`, then restores the saved values. This works because the code is
single-threaded and `editor_update_row()` only reads `editor.syntax` and the single passed row.
However, if `editor_update_row()` ever calls a function that uses `editor.numrows` as a bound,
it could read beyond the staged array. Add a comment in `load_file_transactional()` documenting
the invariant that `editor_update_row()` must not use `editor.numrows`.

**F3b — The `suppress_undo = 1` guard in `buf_reload_from_disk()` may be redundant.**
`commit_load_result()` calls `editor_update_syntax()` on every row. Confirm whether
`editor_update_syntax()` can push undo records. If not, the `suppress_undo` guard is
unnecessary. Remove it and add a comment, or keep it and explain why.

**F3c — No test for open-a-permission-denied-path leaves buffer intact.**
The plan (§8.4) required this case. Add a PTY test (using a `chmod 000` file) or a native test
with a stub `fopen` returning `NULL` with `errno = EACCES`, verifying that
`editor.filename`, `editor.row`, `editor.numrows`, and `editor.dirty` are unchanged.

**F3d — `free_load_result()` idempotence is not tested.**
On `fclose()` failure, the function returns -1 without zeroing `res->filename` or `res->row`.
The caller then calls `free_load_result()` again. Verify that `free_load_result()` handles
`res->filename == NULL` and `res->row == NULL` without double-free, and add a test calling it
twice on the same struct.

**F3e — Five PTY scenarios from §8.4 do not exist.**
Missing: open-a-directory, reload-failure-preserves-edited-content, failure-in-one-buffer-
does-not-damage-another, round-trip with and without a final newline.

### Tasks

| ID | File | Action |
|---|---|---|
| F3a | `src/fileio.c` | Add a comment near the `editor.row = res->row` temporary install explaining the safety invariant. |
| F3b | `src/bufmgr.c` → `buf_reload_from_disk()` | Verify `editor_update_syntax()` does not push undo records. Remove the `suppress_undo` guard if it is unused, or document why it is needed. |
| F3c | `test/test_buffer.c` or `test/pty/` | Add a test that opening a permission-denied path leaves the current buffer byte-for-byte intact. |
| F3d | `src/fileio.c` → `free_load_result()`, `test/test_buffer.c` | Confirm or add NULL-field guards. Add a double-free test. |
| F3e | `test/pty/` | Add three PTY cases: open-a-directory, reload-failure, final-newline round-trip. |

**Escalation:** F3a refactoring (passing syntax directly to avoid aliasing) — discuss with
senior before implementing; it touches an internal API boundary.

---

## Phase 4 — Atomic file saving

### What was done

`ff50ce6` added `write_all()` (retries `EINTR`, handles short writes, treats a zero-byte write
as `EIO`) and rewrote `editor_write_rows_to_file()` to use `mkstemp()` → `write_all()` →
`fsync()` → `close()` → `rename()`, with full cleanup (`close()` + `unlink()`) on every
pre-rename failure path. Symlink targets are resolved via `lstat()` + `realpath()`. Permissions
are applied with `fchmod()`. `editor_save()` now commits the new filename and syntax only after
the rename succeeds.

### What is missing or wrong

**F4a — `write_all()` is untested in isolation.**
`write_all()` is an internal function with no `static` qualifier. It is testable via the
existing `editor_write_fn` function pointer hook, but there are no tests for the `EINTR` retry
loop, the short-write loop, or the zero-byte → `EIO` path. Expose its declaration in `src/def.h`
(or a new `src/fileio.h`) and add three narrow unit tests.

**F4b — Directory sync policy is undocumented.**
The plan (§9.3 step 8) noted that directory sync is optional. The code omits it. Add a comment
in `editor_write_rows_to_file()` stating that directory sync is intentionally omitted.

**F4c — `temp_template` buffer size rationale is undocumented.**
The buffer is `PATH_MAX + 32`. Add a comment explaining that `/.kgtempXXXXXX` is 14 bytes and
the `+ 32` provides margin. The `snprintf` truncation check is correct; this is documentation
only.

**F4d — `fsync()` failure on non-durable filesystems (tmpfs, FAT) is treated as fatal.**
`fsync()` on a tmpfs fd returns `EINVAL`. The current code treats all `fsync()` failures as
hard errors, which would cause saves to fail in CI containers or on FAT volumes. Add a comment
documenting this policy. **Escalate to senior** before deciding whether to tolerate `EINVAL` or
`ENOTSUP`.

**F4e — `editor.dirty` preservation on rename failure is not tested.**
The code path preserves `dirty` correctly (`undo_mark_clean()` is called only after success),
but there is no test that verifies `editor.dirty` is still set after a rename failure.

**F4f — Missing tests from §9.5.**
Not tested: sync failure, close failure, rename failure (each need a function pointer hook),
symlink policy, save-as failure retaining old buffer filename, changed-on-disk confirmation.

### Tasks

| ID | File | Action |
|---|---|---|
| F4a | `src/def.h` or `src/fileio.c`, `test/test_buffer.c` | Declare `write_all()`. Add three unit tests: EINTR-retry, short-write retry, zero-byte → EIO. |
| F4b | `src/fileio.c` | Add a comment stating directory sync is intentionally omitted. |
| F4c | `src/fileio.c` | Add a comment on `PATH_MAX + 32`. |
| F4d | `src/fileio.c` | Add a comment on `fsync()` failure policy. Escalate the `EINVAL`/`ENOTSUP` question before any code change. |
| F4e | `test/test_buffer.c` | Add a comment on dirty-flag ordering. Add a test with injected rename failure verifying `editor.dirty` is still set. |
| F4f | `test/test_buffer.c`, `test/pty/` | Add function pointer hooks for `fsync`, `close`, and `rename`. Add tests for each failure. Add a PTY case for save-as failure and changed-on-disk confirmation. |

**Escalation:** F4d — `fsync()` policy on non-durable filesystems.

---

## Phase 5 — Bulk byte-range deletion for undo

### What was done

`7460cf7` added `editor_delete_text_range_raw()` to `src/yank.c`. It walks newlines to compute
the end position, then calls `editor_delete_region_range()` once. Both `UNDO_YANK_TEXT` and
`UNDO_REPLACE_TEXT` in `src/undo.c` now use this primitive. Tests in `test/test_undo.c` cover
single-row, multi-row, newline boundary, large, and undo-of-yank/replace cases.

### What is missing or wrong

**F5a — `editor_delete_text_range_raw()` silently truncates when `byte_len` overruns the buffer.**
If the recorded byte count is larger than the remaining buffer bytes, the walk terminates with
`row >= editor.numrows`. The function clamps to the last position and returns 1 (success).
The plan (§10.1) required: *"it returns failure without leaving a half-applied edit where
practical."* Either return 0 when the range overruns the buffer, or add a comment and an
assertion stating that callers must guarantee in-bounds ranges.

**F5b — The walk loop has an off-by-one on the final row.**
The loop deducts `row_len - col + 1` when crossing into the next row (the `+1` represents the
newline separator). The last row in the buffer has no trailing newline in the serialized format.
If the deletion span reaches the very last row, the `+1` overestimates the byte cost of
joining it. Write a test: two rows `"hello"` + `"world"`, delete `byte_len = 6` (all of row 0
plus the newline). Verify the result is a single row `"helloworld"` (10 bytes). Compare against
the existing `test_delete_text_range_newline` second sub-case.

**F5c — No complexity proof.**
The plan (§10.3) required demonstrating O(rows) not O(bytes). The existing
`test_delete_text_range_large` tests correctness only. Add a structural-operation counter to
`editor_delete_region_range()` (via a static counter reset in the test binary) and assert it is
called at most once per spanned row.

**F5d — No fuzz seed for paste-then-undo.**
The plan (§10.3) required a fuzz seed for "long paste followed by undo". None was added.

### Tasks

| ID | File | Action |
|---|---|---|
| F5a | `src/yank.c` → `editor_delete_text_range_raw()` | Either return 0 on overrun, or add a comment and assertion that callers must guarantee in-bounds. Add a test for an overrun range. |
| F5b | `src/yank.c`, `test/test_undo.c` | Write the `byte_len = row0.size + 1` test. Fix the loop if the test fails. |
| F5c | `test/test_undo.c` | Add a counter in the test binary for calls to `editor_delete_region_range`. Assert the count equals the number of spanned rows. |
| F5d | `test/fuzz/` or `test/seeds/` | Add a fuzz seed for 1000-byte paste followed by undo. Follow the existing seed format. |

**Escalation:** None. All changes are local to `src/yank.c` and `test/test_undo.c`.

---

## Phase 6 — Display allocation failure

### What was done

`fdd424f` added an `oom` field to `struct abuf` and changed `ab_append()` to return `int`
(1 = success, 0 = failure), set `ab->oom = 1` on overflow or allocation failure, and become a
no-op once `oom` is set. `editor_refresh_screen()` checks `ab.oom` after the final append and
calls `editor_at_exit()` + `fprintf(stderr, …)` + `exit(1)`. A test in `test_basic.c`
verifies sticky-fail behavior.

### What is missing or wrong

**F6a — OOM response calls `editor_at_exit()` directly, bypassing the `atexit` chain.**
Phase 7 registered `editor_cleanup` via `atexit` (after `editor_at_exit` was registered inside
`enable_raw_mode()`). Because `atexit` handlers run in reverse order, calling `exit(1)` will
run `editor_cleanup` first, then `editor_at_exit`. However, the OOM path in
`editor_refresh_screen()` calls `editor_at_exit()` directly *before* calling `exit(1)`, which
means `editor_at_exit()` runs twice (once explicitly, once via atexit). This double restoration
is harmless today but fragile. Remove the direct `editor_at_exit()` call from the OOM path and
let `exit(1)` trigger the registered chain. Add a comment documenting the registration order.

**F6b — `ab_append()` return value is ignored at nearly every call site.**
The sticky `oom` flag makes per-call checking optional, but static analyzers will warn.
Add `__attribute__((warn_unused_result))` (guarded by `#ifdef __GNUC__`) to the `ab_append()`
declaration in `src/def.h`. Fix any resulting warnings in `src/display.c`.

**F6c — `ab_append()` with `len == 0` returns 1 but does nothing.**
Correct behavior but a semantic change from the old `void` version. Add a comment.

**F6d — No test for the OOM → exit path in `editor_refresh_screen()`.**
The existing test verifies `ab_append` in isolation. Add a test that injects an allocator
failure after N calls, calls `editor_refresh_screen()`, and verifies that the `editor_at_exit`
stub is invoked (replace it with a flag setter for the test).

### Tasks

| ID | File | Action |
|---|---|---|
| F6a | `src/display.c` → `editor_refresh_screen()` | Remove the direct `editor_at_exit()` call; replace with `exit(1)` only, relying on `atexit`. Add a comment documenting the registration order. |
| F6b | `src/def.h` | Add `__attribute__((warn_unused_result))` to `ab_append`. Fix resulting warnings. |
| F6c | `src/display.c` → `ab_append()` | Add a comment for `len == 0`. |
| F6d | `test/test_basic.c` | Add a test with injected allocator failure; verify `editor_at_exit` stub is called exactly once. |

**Escalation:** F6a — review alongside Phase 7 to confirm there is no double-restore risk.

---

## Phase 7 — Centralized cleanup

### What was done

`2d4a0ba` added `editor_cleanup()` to `src/bufmgr.c`. It: (1) flushes live state to
`buflist[buf_current]` via `buf_save_to_slot()`; (2) zeroes the global `undostack`;
(3) iterates all active buffers, freeing rows, filename, and undo chain; (4) clears
`editor.row`, `editor.numrows`, `editor.filename`; (5) calls `kill_ring_free()`, `undo_free()`,
and `kg_lisp_shutdown()`. It is registered via `atexit(editor_cleanup)` in `init_editor()`.
`kg_lisp_shutdown()` was removed from `main()`.

### What is missing or wrong

**F7a — `buf_save_to_slot()` may leak the old undo chain when called from `editor_cleanup()`.**
`buf_save_to_slot()` copies the live `undostack` struct pointer into the slot. If the slot
already had an active undo chain from a previous `buf_switch()` call, the old chain pointer
is simply overwritten without freeing it. The cleanup loop then frees the newly installed chain.
The old chain is leaked. Inspect `buf_save_to_slot()` (lines 44–80 of `src/bufmgr.c`) to
confirm whether it frees the previous chain before overwriting. If not, either fix
`buf_save_to_slot()` to free the old chain first, or restructure `editor_cleanup()` to free
each slot's existing chain before calling `buf_save_to_slot()`.

**F7b — `undo_free()` at the end of `editor_cleanup()` is a no-op.**
`undostack.head` was set to `NULL` at the top of `editor_cleanup()`, so `undo_free()` does
nothing. Remove the redundant call and add a comment.

**F7c — `editor_cleanup()` is not idempotent.**
The plan (§12.2) required *"Make repeated calls harmless."* On a second call,
`buf_save_to_slot(buf_current)` is invoked with `editor.row == NULL`. If the slot's `active`
flag is still set, the cleanup loop will attempt to free already-freed rows and undo chains.
Add a static `cleaned_up` guard at the top of `editor_cleanup()` that returns immediately on
the second call. Add a test that calls it twice under ASan.

**F7d — Pending terminal input is not freed.**
The plan (§12.2 step 6) listed "release pending terminal input". `pending_input` (a
`static unsigned char *` in `src/tty.c`, freed by an internal function at lines 79–83) is
never called during cleanup. Add a `tty_cleanup()` function to `src/tty.c` that frees
`pending_input` and resets the associated size variables. Call it from `editor_cleanup()`
before terminal mode restoration. Declare it in `src/def.h`.

**F7e — None of the cleanup scenarios from §12.4 are tested.**
Missing: multiple buffers with independent undo stacks, two windows sharing one buffer, special
and unnamed buffers, cleanup after partial initialization, cleanup called twice, Valgrind clean
exit.

**F7f — Lisp shutdown may occur after row memory is freed.**
The plan (§12.2) required shutting down Lisp *while editor-backed callbacks are still valid*,
i.e., before freeing rows. The current order in `editor_cleanup()` frees rows first and calls
`kg_lisp_shutdown()` last. If any Lisp GC or finalizer touches `editor.row`, this is a
use-after-free. Add a comment flagging the ordering risk. **Escalate to senior** before
reordering.

### Tasks

| ID | File | Action |
|---|---|---|
| F7a | `src/bufmgr.c` → `buf_save_to_slot()` and `editor_cleanup()` | Audit whether the old undo chain is freed before overwrite. Fix the leak. |
| F7b | `src/bufmgr.c` → `editor_cleanup()` | Remove the redundant `undo_free()` call. Add a comment. |
| F7c | `src/bufmgr.c` → `editor_cleanup()` | Add a static idempotence guard. Add a double-cleanup test under ASan. |
| F7d | `src/tty.c`, `src/def.h`, `src/bufmgr.c` | Add `tty_cleanup()`. Call it from `editor_cleanup()`. |
| F7e | `test/test_buffer.c` or new `test/test_cleanup.c` | Add: two-buffer cleanup, two-window-one-buffer cleanup, partial-init cleanup, double-cleanup. Run all under Valgrind. |
| F7f | `src/bufmgr.c` → `editor_cleanup()` | Add a comment flagging Lisp shutdown ordering risk. Escalate before any reorder. |

**Escalation:** F7f — Lisp shutdown ordering relative to row cleanup.

---

## Cross-cutting gaps

### FC1 — `WITH_LISP=0` coverage for new tests is unverified

Run `make clean && make check WITH_LISP=0` and confirm all tests introduced in Phases 1–7
pass. If any test unconditionally links a Lisp-only symbol, add a stub or a compile-time guard.

### FC2 — No Valgrind baseline was recorded

The plan required *"Valgrind reports no definite or possible leaks."* Add a `make valgrind`
target (or CI step) that runs `test_buffer`, `test_tty`, `test_undo`, and `test_basic` under
Valgrind and saves a clean baseline output.

### FC3 — `atexit` registration order is undocumented

`editor_at_exit` is registered inside `enable_raw_mode()` (called during terminal setup).
`editor_cleanup` is registered in `init_editor()` (called later). `atexit` handlers run in
reverse order, so cleanup runs first and terminal restoration runs second. This is the correct
order, but it is not documented. Add a comment near each `atexit` call stating the intended
firing order relative to the other handler.

### FC4 — Phase 8 (test gaps) has not started

The plan's Phase 8 provides named-command, macro, resize/window, file-I/O injection, and fuzz
coverage that must exist before optional Phase 9 refactoring. Do not begin Phase 9 until at
least the portions of Phase 8 relevant to the area being refactored are complete.

---

## Task priority table

| ID | Phase | Severity | Effort | Blocker for |
|---|---|---|---|---|
| F7f | 7 | Critical | Trivial (comment) | Any future Lisp cleanup |
| F7a | 7 | High | Medium | F7e tests |
| F6a | 6 | High | Low | Correct atexit ordering |
| F2b | 2 | High | Low | kill-ring invariant |
| F5a | 5 | Medium | Low | Undo truncation semantics |
| F5b | 5 | Medium | Low | Undo last-row correctness |
| F7c | 7 | Medium | Low | Double-cleanup safety |
| F7d | 7 | Medium | Low | Complete cleanup |
| F3d | 3 | Medium | Low | Transactional open correctness |
| F1a | 1 | Medium | Low | Post-resize geometry |
| F1b | 1 | Medium | High | Prompt-during-resize safety |
| F2c | 2 | Medium | Medium | Unchecked alloc sites |
| FC1 | All | Medium | Low | WITH_LISP=0 CI |
| FC2 | All | Medium | Low | Valgrind baseline |
| FC3 | All | Medium | Trivial | Shutdown documentation |
| F4a | 4 | Low | Low | write_all coverage |
| F4f | 4 | Low | Medium | Full save coverage |
| F3c | 3 | Low | Low | Open-error buffer test |
| F3e | 3 | Low | Medium | PTY coverage |
| F7b | 7 | Low | Trivial | Code clarity |
| F7e | 7 | Low | Medium | Cleanup evidence |
| F6b | 6 | Low | Trivial | Analyzer warnings |
| F6d | 6 | Low | Medium | OOM exit test |
| F5c | 5 | Low | Low | Complexity proof |
| F5d | 5 | Low | Trivial | Fuzz coverage |
| F1c–e | 1 | Low | Low | Documentation/minor tests |
| F2a, F2d, F2e | 2 | Low | Low | Documentation/minor gaps |
| F4b–e | 4 | Low | Trivial | Documentation |
| F3a, F3b | 3 | Low | Low | Code clarity |

---

## Junior engineer task template

Use for every issue or PR:

```markdown
## Objective
One sentence: the invariant or behavior to establish.

## Current behavior
Exact file path, function name, and line range.
Failing test output or analyzer diagnostic.

## Intended behavior
Observable result, error state, and state that must remain unchanged.

## In scope
List of files and functions expected to change.

## Out of scope
Related work not included.

## Test first
Native test, PTY case, or fuzz seed that demonstrates the problem.

## Implementation notes
Ownership, error propagation, portability constraints.

## Validation commands
Exact commands and expected output.

## Escalation points
Decisions requiring senior approval.
```

---

## Escalation summary

Stop and request senior review before proceeding on:

| ID | Topic |
|---|---|
| F1b | Redraw-from-prompt design during SIGWINCH |
| F2c | Integer type of `len` / `total` in `src/rect.c` |
| F4d | `fsync()` failure policy on non-durable filesystems |
| F6a | atexit ordering for OOM exit path |
| F7f | Lisp shutdown ordering relative to row/undo cleanup |
