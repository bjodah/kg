# kg Reliability and Maintainability Implementation Plan

**Status:** Reviewed against `bjodah/kg` branch `stricter-emacs-adherence` on 2026-07-17  
**Audience:** Junior engineer, with senior review at the escalation points called out below  
**Scope:** Reliability, data safety, error handling, performance, cleanup, tests, and low-risk maintainability work in `kg`  
**Out of scope:** Redesigning the regular-expression dialect or Fe API; those have dedicated plans under `.meta-docs/plans/`

---

## 1. Purpose

This plan turns a collection of review findings into a sequence of small, testable pull requests.

The desired end state is:

1. Terminal resize handling performs no unsafe work in a signal handler.
2. Integer and allocation-size calculations fail safely instead of wrapping.
3. Opening, reloading, and saving files do not destroy the current buffer or the previous on-disk file on failure.
4. Large yank and replacement undos do not delete one byte at a time.
5. Screen-rendering allocation failures are detected and handled consistently.
6. Normal and early exits release editor-owned memory cleanly.
7. Important command, macro, file-I/O, resize, and window behavior has regression coverage.
8. Large refactors happen only after behavior is protected by tests.

The phases are ordered by risk and dependency. Complete them in order unless a senior maintainer explicitly approves a different sequence.

---

## 2. Repository topology and source of truth

The checked-in repository topology is:

```text
bjodah/kg                 branch: stricter-emacs-adherence
└── fe/                   repository: bjodah/fe
    branch metadata: analyzers-etc
    └── tiny-regex-c/     repository: bjodah/tiny-regex-c
        branch metadata: adapt-to-fe
```

Important details:

- The nested submodule is named and checked out as `tiny-regex-c`, not `regex-tiny-c`.
- Its repository is `bjodah/tiny-regex-c`, not `bjodah/regex-tiny-c`.
- A submodule branch entry in `.gitmodules` is convenience metadata. The build uses the exact commit recorded by the parent repository's gitlink.
- Do not advance either submodule pointer as part of an unrelated `kg` hardening pull request.
- If a submodule update is necessary, make it a separate reviewed change and include the complete submodule commit range in the pull-request description.
- `kg` uses `fe/tiny-regex-c` even when built with `WITH_LISP=0`; recursive submodule initialization is therefore required in both build modes.

Existing regex work is already coordinated by:

- `.meta-docs/plans/100-REGEX-MASTER-ROADMAP.md`
- `.meta-docs/plans/102-REGEX-EDITOR-INTEGRATION.md`
- `.meta-docs/ideation/103-REGEX-STACK-LOW-HANGING-FRUIT.md`

Do not duplicate that work here.

---

## 3. Review verification summary

### 3.1 Confirmed findings

| Area | Verified condition | Priority |
|---|---|---:|
| `SIGWINCH` | `handle_sig_winch()` calls window-layout and screen-refresh code from the signal handler | P0 |
| Interrupted reads | terminal readers treat any `read()` failure as editor shutdown; resize handling must account for `EINTR` | P0 |
| Buffer serialization | `editor_rows_to_string()` accumulates byte counts in `int` without checked addition | P0 |
| Kill ring | `kill_ring_set()` and `kill_ring_append()` can overflow their allocation expressions | P0 |
| File open | `editor_open()` mutates active state before confirming that the file can be opened and exits the process on some errors | P0 |
| Reload | `buf_reload_from_disk()` frees current rows before a replacement buffer has been loaded successfully | P0 |
| File save | saving truncates the destination before writing, assumes one `write()` completes everything, and ignores some close/durability failures | P0 |
| Save-as | the buffer filename can be changed before the write succeeds | P0 |
| Undo performance | `UNDO_YANK_TEXT` and `UNDO_REPLACE_TEXT` delete byte-by-byte | P1 |
| Display append buffer | `ab_append()` silently ignores allocation failure and does not check size overflow | P1 |
| Process cleanup | shutdown does not visibly free all buffer, undo, kill-ring, window, and editor-owned allocations | P1 |
| Test coverage | command dispatch, macro recording/replay, file-I/O failure paths, and resize behavior need focused coverage | P1/P2 |
| Keyboard dispatcher | `editor_process_keypress()` remains large, but already contains useful helper boundaries | P2 |
| Syntax tables | syntax extension and keyword arrays can be made read-only after dependent types are checked | P3 |

### 3.2 Corrections to the earlier draft

The following points should **not** be assigned as written in the earlier draft:

1. **Sorting rows is not currently a demonstrated ownership bug.**  
   `editor_sort_lines()` copies `erow` values to a temporary array, sorts the values, copies each value back once, and frees only the temporary array. That permutes ownership; it does not by itself duplicate a final owner or free an aliased row allocation. Add an invariant test or explanatory comment if useful, but do not rewrite it without a reproducer.

2. **Changing only `totlen` to `size_t` is not a complete fix.**  
   The surrounding serialization, undo, row-size, kill-ring, and file-write interfaces still use `int`. Either preserve the current `int` API with explicit checked limits, or perform a deliberate repository-wide type migration. Do not mix the two approaches accidentally.

3. **Replacing `exit(1)` with `return 1` is not a complete open-error fix.**  
   The active filename and some buffer state are changed before `fopen()` succeeds. Opening and reloading need transactional staging so failure leaves the current buffer intact.

4. **Display/window tests should use the existing PTY harness where behavior depends on a real terminal.**  
   Do not first build a large mock-terminal framework. Extract and unit-test pure geometry helpers only when that creates a clear seam.

5. **A broad keyboard dispatch-table rewrite is premature.**  
   Increase behavior coverage first, then extract one coherent prefix or command family at a time without changing behavior.

6. **The previously mentioned `editor_comment_dwim()` fixed-array issue was not verified during this review.**  
   Re-locate the exact code and provide a reproducer or analyzer diagnostic before creating an implementation task for it.

---

## 4. Working rules for every phase

### 4.1 Pull-request discipline

- One behavioral concern per pull request.
- Prefer a test commit followed by an implementation commit when practical.
- Keep source changes local and match the repository's C23 style.
- Do not introduce a new dependency without explicit approval.
- Do not combine source hardening with a submodule-pointer update.
- Do not weaken an analyzer, sanitizer, Valgrind, format, complexity, or PTY expectation to make a change pass.
- Do not use a wall-clock timing assertion when an operation count, allocation count, or bounded stress test can prove the property more reliably.

### 4.2 Test selection

Use the lowest appropriate layer:

- **Native C test:** pure state transitions, checked arithmetic, buffer transformations, command lookup, macro state, and helper functions.
- **PTY test:** terminal input, resize, screen layout, prompts, windows, cursor behavior, save results, and Emacs-oracle comparisons.
- **Fuzz target:** parsers, key streams, length boundaries, and state-machine interactions.

### 4.3 Required checks before requesting review

Run the checks relevant to the change, then run the complete gate before declaring a phase complete:

```sh
git submodule update --init --recursive
make format-check
make check
make clean
make check WITH_LISP=0
make clean
.ci/run-ci-steps.sh
```

For a narrow iteration, run the relevant unit binary, PTY YAML file, or numbered `.ci/ci-*.sh` script first.

When changing `fe` or `tiny-regex-c`, validate bottom-up before validating `kg`.

### 4.4 Definition of done for a phase

A phase is complete only when:

- the specified regression tests exist and fail on the old implementation where feasible;
- the implementation passes both `WITH_LISP=1` and `WITH_LISP=0` configurations;
- format, sanitizer, analyzer, Valgrind, PTY, and complexity gates relevant to the change pass;
- error paths preserve documented ownership and state invariants;
- user-visible behavior or limitations are documented where necessary;
- no generated artifacts are included in the diff;
- the pull-request description states what was tested and what remains out of scope.

---

## 5. Phase 0 — Establish a reproducible baseline

**Goal:** Make the exact repository state and validation commands unambiguous before changing behavior.

**Suggested size:** Small documentation/CI pull request.

### 5.1 Record the exact dependency state

From the `kg` repository root, capture:

```sh
git submodule sync --recursive
git submodule update --init --recursive

git rev-parse HEAD
git submodule status --recursive
git -C fe rev-parse HEAD
git -C fe/tiny-regex-c rev-parse HEAD
```

Add the command output to the pull-request description, not to a permanent source file unless maintaining a generated dependency manifest is explicitly desired.

### 5.2 Reconcile submodule documentation

Check these against the actual gitlinks:

- `kg/.gitmodules`
- `fe/.gitmodules`
- `README.md`
- `doc/fe-upstream.md`
- regex planning documents

`doc/fe-upstream.md` currently names a specific Fe tag and commit. Confirm that it still matches the `fe/` gitlink. If not, update the documentation or the pin in a dedicated, reviewed submodule update; do not silently make them agree inside an unrelated fix.

### 5.3 Make clean-clone CI recursive

The GitHub Actions workflow checks out the parent repository but does not explicitly request recursive submodules. Update active checkout workflows to initialize recursively, or document why another mechanism guarantees the nested checkout.

Add a cheap pre-build assertion such as:

```sh
test -f fe/fe.c
test -f fe/tiny-regex-c/re.c
```

The Makefile already gives useful errors when these files are absent; retain those messages.

### 5.4 Run the baseline bottom-up

When the submodule worktrees correspond to their intended commits:

```sh
# Nested regex engine
make -C fe/tiny-regex-c clean
make -C fe/tiny-regex-c check
make -C fe/tiny-regex-c format-check

# Fe
make -C fe clean
make -C fe check
make -C fe format-check

# kg
make distclean
make check
make distclean
make check WITH_LISP=0
make format-check
.ci/run-ci-steps.sh
```

If the local environment cannot run a particular CI tool, record that fact and rely on the corresponding CI job. Do not report an unrun check as passing.

### 5.5 Acceptance criteria

- A clean recursive clone reaches the same dependency commits.
- Both `kg` build modes find `fe/tiny-regex-c`.
- Active CI workflows initialize nested submodules.
- `doc/fe-upstream.md` and the actual `fe/` gitlink no longer contradict each other.
- Baseline failures, if any, are recorded separately before implementation work begins.

---

## 6. Phase 1 — Make terminal resize handling signal-safe

**Goal:** Restrict `SIGWINCH` handling to async-signal-safe operations and keep input robust when syscalls are interrupted.

**Suggested size:** One focused pull request.

### 6.1 Add a pending-resize flag

In the terminal module:

- declare a `static volatile sig_atomic_t` pending flag;
- make `handle_sig_winch()` set only that flag;
- preserve `errno` in the handler if the implementation or coding standard requires it;
- do not call `update_window_size()`, `win_reflow()`, `editor_refresh_screen()`, allocation functions, stdio, or status-message code from the handler.

Use `sigaction()` rather than `signal()` so handler installation and restart behavior are explicit.

### 6.2 Process pending resize in normal control flow

Add a small helper, for example:

```c
int editor_process_pending_signals(void);
```

The helper should:

1. snapshot and clear the coalesced resize flag;
2. call `update_window_size()` in normal process context;
3. reflow windows and clamp views through existing paths;
4. report whether a redraw is required.

Call it from the top-level idle/input loop at a point where editor state is consistent. Also decide how resize is handled while a minibuffer or yes/no prompt is active; do not leave prompts permanently drawn with stale dimensions.

### 6.3 Make input readers `EINTR`-safe

Audit:

- `read_input_byte()`
- `editor_read_key()`
- `editor_read_raw_byte()`
- `editor_read_key_idle()`
- cursor-position query reads
- relevant `poll()` calls

A syscall interrupted by `SIGWINCH` must not set `running = 0`. Retry on `EINTR`, process pending signals in an appropriate normal-context location, and distinguish EOF from error.

### 6.4 Tests

Add PTY coverage for:

- one resize while the editor is idle;
- repeated/coalesced resize signals;
- resize in a split-window layout;
- resize while a path or text prompt is visible;
- continued editing and saving after resize;
- dimensions small enough to exercise clamping.

Where possible, add a native test proving that invoking the handler changes only the pending flag.

### 6.5 Acceptance criteria

- The signal handler performs only flag assignment and other explicitly async-signal-safe work.
- `SIGWINCH` cannot make a terminal read look like fatal EOF.
- Repeated resizes do not crash, hang, or corrupt window/cursor state.
- PTY resize tests pass under the ordinary build and sanitizer build.

---

## 7. Phase 2 — Introduce checked length and allocation arithmetic

**Goal:** Establish one consistent policy for byte counts and allocation sizes before fixing callers piecemeal.

**Suggested size:** Two pull requests: checked helpers/API policy, then caller migration.

### 7.1 Define the type policy

Use this default unless a maintainer approves a larger migration:

- keep editor row/column coordinates as `int` for now;
- use `size_t` for allocation sizes and capacities;
- before converting a `size_t` length back to an `int` field or API, reject values greater than `INT_MAX`;
- report `EOVERFLOW` for representational overflow and `ENOMEM` for allocator failure where the caller uses `errno`;
- leave the destination object unchanged on failure.

Do not merely cast a larger type to `int`.

### 7.2 Add reusable checked-arithmetic helpers

Create small internal helpers for operations such as:

- checked `size_t` addition;
- checked `int` addition with a non-negative operand;
- conversion from `size_t` to non-negative `int`;
- allocation of `count + terminator` bytes.

Keep helpers simple enough to test directly. Avoid compiler-specific overflow builtins unless portability has been checked for all supported compilers.

### 7.3 Fix buffer serialization

Update `editor_rows_to_string()` so it checks all components:

- every row byte length;
- newline separators;
- final NUL byte;
- conversion to the public output-length type.

Choose one of these explicit approaches:

1. preserve the current `int *buflen` API and reject serialized buffers larger than `INT_MAX`; or
2. migrate the API and every caller to `size_t` in one reviewed change.

The first approach is smaller and is recommended for this phase.

On failure:

- return `NULL`;
- provide a deterministic error indication;
- set the output length to zero;
- do not partially mutate editor state.

### 7.4 Fix kill-ring calculations

Audit both:

- `kill_ring_set()` — `len + 1`;
- `kill_ring_append()` — `killring.len + len + 1`.

On overflow or allocation failure, preserve the old kill-ring contents and length.

### 7.5 Audit adjacent allocation paths

At minimum inspect:

- `editor_row_append_string()`;
- append-buffer growth in `display.c`;
- undo text allocation;
- raw/bulk insertion helpers;
- rectangle text storage;
- path/minibuffer append calculations;
- any expression combining an `int` field with a `size_t` argument.

Do not expand the phase into a repository-wide style cleanup. Fix concrete unchecked allocation expressions and add tests.

### 7.6 Tests

Add boundary tests that do not require allocating multi-gigabyte buffers:

- exercise checked helpers near `INT_MAX` and `SIZE_MAX`;
- verify serialization rejects an unrepresentable aggregate length;
- verify kill-ring contents remain unchanged after a rejected append;
- verify row and append-buffer objects retain their original pointers/lengths after injected failure;
- verify successful ordinary and empty-buffer cases remain unchanged.

A small allocator or syscall seam may be added for tests, but keep it private and narrow.

### 7.7 Acceptance criteria

- No reviewed allocation expression can wrap before allocation.
- No narrowing conversion is implicit at a length boundary.
- Failure leaves the destination object valid and owned exactly once.
- Tests cover success, overflow, and allocator failure separately.
- Sanitizer and analyzer jobs stay clean.

---

## 8. Phase 3 — Make file opening and reloading transactional

**Goal:** A failed open or reload must not destroy or rename the current buffer, terminate the whole editor, or discard another buffer's unsaved work.

**Suggested size:** One or two pull requests, depending on how much helper extraction is needed.

### 8.1 Separate loading from committing editor state

Introduce a temporary load result that owns:

- duplicated filename;
- row array and row count;
- newline-ending state;
- disk metadata needed for the initial snapshot;
- any syntax-selection input required after success.

Load and parse the file into this temporary object first. Only after all reads and allocations succeed should the active buffer adopt it.

Provide one cleanup helper for a partially built load result.

### 8.2 Handle open outcomes deliberately

Define behavior for:

- existing readable file — load and commit;
- missing file (`ENOENT`) — create an empty visiting buffer without treating it as a fatal error;
- permission denied, directory, I/O error, or allocation failure — show an error and retain the previous buffer unchanged;
- read error after some lines have been read — discard the staged result and retain the previous buffer;
- `fclose()` error where relevant — report failure rather than silently treating it as a clean load.

Do not call `exit()` from ordinary file-open failure paths.

### 8.3 Fix reload ordering

`buf_reload_from_disk()` must not free the existing rows until a replacement has loaded successfully.

On reload failure preserve:

- filename;
- rows and row count;
- dirty flag;
- cursor and viewport;
- undo stack;
- local variables/read-only state;
- disk-change indication, unless there is a documented reason to update it.

On success, replace the old state once, then release the old ownership.

### 8.4 Tests

Add native and/or PTY tests for:

- opening a missing file as an empty visiting buffer;
- opening a permission-denied file;
- trying to open a directory;
- simulated read/allocation failure after one or more rows;
- reload failure preserving edited content and undo state;
- failure in one buffer not damaging another open buffer;
- exact preservation of a file with and without a final newline.

### 8.5 Acceptance criteria

- No ordinary open error terminates the editor.
- Failed open and failed reload leave the previous active buffer byte-for-byte intact.
- Temporary rows and filenames are freed on every failure path.
- Successful load behavior, syntax selection, final-newline policy, and disk snapshots remain correct.
- Valgrind and ASan/UBSan pass the new failure tests.

---

## 9. Phase 4 — Implement reliable and atomic file saving

**Goal:** A failed save must leave the previous file and in-memory buffer state intact.

**Suggested size:** Two pull requests: write primitive/tests, then editor integration.

### 9.1 Decide path-identity semantics before coding

Atomic replacement with `rename()` changes inode identity and has observable behavior for:

- symbolic links;
- hard links;
- ownership and permissions;
- ACLs and extended attributes;
- file watchers.

Before implementation, document and test the intended policy.

Recommended default for a small editor:

- write a temporary regular file in the destination directory;
- preserve the existing regular file's permission bits where possible;
- atomically replace the destination pathname;
- explicitly document that this replaces the path's inode and therefore does not preserve hard-link identity;
- decide whether saving through a symlink follows the target or replaces the symlink, and escalate this choice for maintainer review.

Do not claim atomic saving until this policy is explicit.

### 9.2 Add a robust write-all helper

The helper must:

- loop until all bytes are written;
- retry on `EINTR`;
- handle short writes;
- treat a zero-byte write before completion as an error;
- preserve the most useful `errno`;
- make no assumption that one `write()` completes the buffer.

Test this helper through a narrow injected writer or file-descriptor seam.

### 9.3 Write through a same-directory temporary file

A safe sequence is:

1. derive the destination directory and a non-colliding temporary template;
2. create the temporary with `mkstemp()` or an equivalent secure primitive;
3. set the intended mode using existing metadata or a documented new-file default;
4. write all bytes;
5. flush file data with `fdatasync()` or `fsync()` according to the portability policy;
6. check `close()` errors;
7. atomically `rename()` the temporary pathname over the destination;
8. optionally sync the parent directory when the project chooses crash-durable rename semantics;
9. unlink the temporary file on every pre-rename failure.

Keep Linux and macOS support in mind. Isolate platform-specific durability calls behind a small helper rather than scattering preprocessor branches.

### 9.4 Commit editor state only after success

For ordinary save and save-as:

- do not clear `dirty` before the rename succeeds;
- do not call `undo_mark_clean()` before success;
- do not refresh the disk snapshot before success;
- do not permanently adopt a new filename before success;
- restore the old filename and syntax if a save-as attempt fails or is cancelled;
- report the actual number of bytes written without narrowing overflow.

Keep the existing changed-on-disk confirmation behavior and test it with the new save path.

### 9.5 Tests

Add a `test_fileio` target or equivalent focused coverage for:

- short writes and `EINTR`;
- failure before any bytes are written;
- failure after a partial write;
- sync failure;
- close failure;
- rename failure;
- old destination contents unchanged for every pre-rename failure;
- temporary file cleanup;
- mode preservation;
- save-as failure retaining the old buffer filename;
- changed-on-disk confirmation;
- empty, no-final-newline, final-newline, and multi-line files;
- the chosen symlink and hard-link policy.

Use PTY tests for prompts and user-visible save behavior; use native tests for the write transaction.

### 9.6 Acceptance criteria

- No pre-rename failure truncates or partially overwrites the old destination.
- No failed save marks the buffer clean or changes its permanent filename.
- All temporary descriptors and paths are cleaned up.
- The symlink/hard-link policy is documented and tested.
- Save tests pass on both supported operating-system families in CI.

---

## 10. Phase 5 — Add a bulk byte-range deletion primitive for undo

**Goal:** Undo a large yank or replacement in work proportional to the affected rows and bytes, not by repeatedly shifting the same tail.

**Suggested size:** One focused pull request.

### 10.1 Define the primitive

Add an internal raw operation such as:

```c
int editor_delete_text_range_raw(int start_row, int start_col, int byte_len);
```

Required semantics:

- the length is a count of serialized buffer bytes;
- newline separators count as one byte;
- deletion may stay within one row or join multiple rows;
- UTF-8 payload is stored as bytes, so the primitive must delete the exact byte span recorded by undo;
- the function does not push nested undo records;
- it updates affected rows and indexes once per structural change, not once per byte;
- it returns failure without leaving a half-applied edit where practical;
- cursor, mark, dirty state, and `suppress_undo` behavior are explicit.

Prefer reusing or extracting existing region/bulk insertion logic rather than maintaining several incompatible span implementations.

### 10.2 Use it in both slow undo cases

Replace byte-by-byte loops in:

- `UNDO_YANK_TEXT`;
- the replacement-span deletion portion of `UNDO_REPLACE_TEXT`.

Do not change the public undo record format unless a test demonstrates that it is insufficient.

### 10.3 Tests

Extend `test/test_undo.c` with:

- same-row yank undo;
- multi-row yank undo;
- deletion beginning or ending exactly at a newline;
- consecutive empty rows;
- UTF-8 byte sequences;
- replacement undo followed by restoration of the original bytes;
- cursor and dirty-state assertions;
- a large same-row and multi-row case.

Demonstrate the complexity improvement using an operation counter or a sufficiently large bounded stress test. Avoid a fragile millisecond threshold.

### 10.4 Acceptance criteria

- Neither undo case calls forward-delete once per byte.
- Existing undo ordering and dirty tracking remain unchanged.
- Large yanks undo without quadratic tail shifting.
- The native undo suite, fuzz smoke test, ASan/UBSan, and Valgrind pass.

---

## 11. Phase 6 — Make display allocation failure explicit

**Goal:** A failed screen append must not silently produce a malformed partial frame or continue performing unchecked appends.

**Suggested size:** One focused pull request.

### 11.1 Give `struct abuf` an error state

Update the append buffer so it can represent:

- byte length/capacity using allocation-safe types;
- allocation or overflow failure;
- no-op behavior after the first failure.

Prefer an API in which `ab_append()` returns success/failure, even if the buffer also stores a sticky failure flag.

### 11.2 Avoid recursive OOM reporting

The ordinary status renderer itself uses the append buffer. Do not respond to append-buffer OOM by recursively trying to render a detailed status through the same failed mechanism.

Choose a simple policy, for example:

1. abort construction of the current frame;
2. free any partial frame;
3. set a sticky editor OOM state;
4. use a minimal direct terminal write or exit the main loop cleanly after terminal restoration.

Have a senior maintainer approve the exact user-visible behavior.

### 11.3 Tests

Use a narrow allocator-failure seam to verify:

- failure on the first append;
- failure after prior successful appends;
- overflow rejection before `realloc()`;
- subsequent appends do nothing;
- partial frame memory is freed;
- no recursive refresh loop occurs;
- normal rendering output is unchanged.

### 11.4 Acceptance criteria

- Append-buffer failure is detectable by the caller.
- No overflow reaches `realloc()`.
- No malformed partial frame is deliberately written after failure.
- The terminal is restored if the chosen policy exits the editor.

---

## 12. Phase 7 — Define ownership and complete process cleanup

**Goal:** Normal and early exits release all editor-owned allocations exactly once.

**Suggested size:** One ownership-documentation/test pull request, then one cleanup implementation pull request.

### 12.1 Write an ownership map first

Document ownership for:

- the live `editor` row array and filename;
- active `buflist[]` slots;
- per-buffer undo stacks;
- global/current undo-stack transfer during buffer switching;
- kill-ring text;
- window state;
- pending terminal input;
- Lisp context and roots;
- temporary prompt/path/regex/compilation resources.

Pay particular attention to the fact that live editor state and a buffer slot can describe the same underlying allocations. Cleanup must not free both views independently.

### 12.2 Add an idempotent shutdown path

Create an explicit function such as:

```c
void editor_shutdown(void);
```

Recommended order:

1. stop command/evaluation activity;
2. shut down Lisp while editor-backed callbacks/state are still valid;
3. save the live state to its owning buffer slot if the ownership model requires it;
4. free each active buffer's rows, filename, and undo chain once;
5. free the kill ring and global auxiliary allocations;
6. release pending terminal input;
7. restore terminal mode through the existing terminal-exit path.

Keep terminal restoration safe as an `atexit()` backstop, but prefer explicit application cleanup from `main()` so ordering and errors are testable.

Make repeated calls harmless.

### 12.3 Cover early returns

Audit paths including:

- Lisp initialization failure;
- terminal raw-mode initialization failure;
- argument/file loading failure;
- ordinary `C-x C-c` exit;
- git-commit abort/non-zero exit;
- server-edit completion;
- EOF or terminal read failure;
- fatal OOM policy.

Route them through one cleanup path where possible.

### 12.4 Tests

Add tests for:

- multiple buffers with independent undo stacks;
- two windows displaying one buffer;
- special buffers and unnamed buffers;
- cleanup after partial initialization;
- cleanup called twice;
- normal and non-zero exit paths under Valgrind.

### 12.5 Acceptance criteria

- Valgrind reports no definite or possible leaks for the covered exits.
- ASan reports no double-free or use-after-free.
- Cleanup is idempotent.
- Terminal restoration remains reliable even if application cleanup encounters an internal error.

---

## 13. Phase 8 — Fill focused test gaps

**Goal:** Protect behavior before undertaking optional structural refactors.

**Suggested size:** Several small test-only or test-heavy pull requests.

### 13.1 Named command dispatch

Add a native `test_cmd` target covering:

- lookup and execution of representative named commands;
- aliases;
- unknown command handling;
- read-only command restrictions;
- commands requiring Lisp in both build modes;
- commands receiving the file descriptor they need;
- no accidental duplicate command names.

Prefer table-driven tests where the current harness makes that clear.

### 13.2 Keyboard macro state

Add a native `test_macro` target covering:

- start, stop, and replay;
- trimming the stop-key sequence;
- recording keys consumed by prompts;
- maximum recording capacity;
- replay termination and empty macro behavior;
- cancellation/error recovery;
- attempts to start recording while already recording or replay while replaying, according to the documented policy.

Add one PTY case proving an end-to-end recorded edit and save.

### 13.3 Resize, windows, and layout

Expand PTY coverage for:

- horizontal and vertical splits;
- deletion of current/other windows;
- active-window cursor preservation;
- resize after each split shape;
- visual-line mode with narrow dimensions;
- UTF-8 and tabbed rows in split windows;
- status/minibuffer rendering under constrained width.

If a geometry function can be made pure with a small extraction, unit-test it. Do not build a full terminal mock first.

### 13.4 File-I/O failure coverage

Retain the Phase 3 and Phase 4 tests as permanent targets. Add deterministic failure injection rather than relying only on filesystem permissions, which can behave differently in privileged CI containers.

### 13.5 Fuzz and seed maintenance

Add small seeds for:

- long paste followed by undo;
- resize/prefix/key combinations;
- repeated empty-line joins and splits;
- path prompt cancellation;
- read-only edit attempts;
- relevant length-boundary state transitions.

Keep CI fuzzing bounded; the purpose of the smoke gate is to prevent harness rot, not to replace longer campaigns.

### 13.6 Acceptance criteria

- New targets are included in `make check`.
- Tests are deterministic on supported CI platforms.
- PTY cases specify dimensions, backend, timing overrides, and filename only when needed.
- Known discrepancies use the existing `xfail` mechanism rather than weakening expected output.

---

## 14. Phase 9 — Low-risk maintainability improvements

**Goal:** Reduce future change risk without altering editor behavior.

**Suggested size:** Independent, reviewable pull requests after Phases 1–8.

### 14.1 Make syntax metadata read-only

Change syntax extension and keyword tables to `const` only after checking every dependent field and function signature.

Likely target shape:

```c
static const char *const c_extensions[] = { ... };
static const char *const c_keywords[] = { ... };
```

Do not cast away `const` to avoid updating a real consumer. Let the compiler identify the necessary type propagation.

### 14.2 Refactor keyboard handling incrementally

Use complexity reports and test coverage to choose one extraction at a time. Suggested order:

1. rectangle-prefix dispatch;
2. `C-c` dispatch;
3. `C-x` dispatch;
4. universal-argument consumption;
5. coherent regular-key families such as movement, editing, or search.

Each helper should return an explicit handled/not-handled result and preserve:

- prefix argument consumption;
- read-only filtering;
- macro recording/replay;
- status messages;
- dirty and shift-selection transitions;
- existing key precedence.

Do not introduce a general dispatch table unless it demonstrably simplifies these semantics and does not duplicate the named-command registry.

For each extraction:

- make no intentional behavior change;
- keep the pull request small;
- run the complete PTY suite;
- compare `pmccabe` and repository complexity results before and after.

### 14.3 Preserve or document sort-line ownership invariants

Do not rewrite `editor_sort_lines()` solely because it temporarily shallow-copies `erow` values.

A useful small improvement is to add a regression test that:

- sorts rows with allocated `chars`, `render`, and `hl` storage;
- verifies each final row owns one unique set of pointers;
- verifies undo restores content;
- runs under ASan and Valgrind.

An implementation rewrite requires a concrete failing case or a separately approved simplification.

### 14.4 Re-verify unconfirmed findings

For any older review note that cannot be located or reproduced:

1. identify the exact function and current branch commit;
2. add a test or attach an analyzer diagnostic;
3. explain the violated invariant;
4. only then create an implementation task.

### 14.5 Acceptance criteria

- No user-visible behavior changes unintentionally.
- Complexity does not increase without a documented reason.
- All tests and both build modes remain green after each extraction.
- Const changes require no casts that hide mutability problems.

---

## 15. Phase 10 — Final integration and release-readiness pass

**Goal:** Prove the completed hardening series works from a clean checkout and has not accidentally changed dependencies or editor semantics.

### 15.1 Verify dependency pins

```sh
git submodule status --recursive
git diff --submodule=log <baseline>...HEAD
```

If no submodule update was intended, the gitlinks must match the baseline exactly.

If a submodule update was intended, review and test bottom-up:

1. `fe/tiny-regex-c`;
2. `fe`;
3. `kg WITH_LISP=1`;
4. `kg WITH_LISP=0`.

### 15.2 Run a clean-clone validation

From a fresh directory:

```sh
git clone --recursive <kg-repository> kg-clean
cd kg-clean
git checkout stricter-emacs-adherence
make check
make clean
make check WITH_LISP=0
make clean
.ci/run-ci-steps.sh
```

Use the actual integration branch or release candidate ref at execution time.

### 15.3 Manual smoke checklist

Manually verify:

- open an existing file and a new file;
- fail to open an unreadable path without losing the active buffer;
- edit and atomically save;
- cancel and fail save-as without changing the buffer name;
- detect an externally changed file;
- resize repeatedly with one and multiple windows;
- paste and undo a large multi-line block;
- record and replay a keyboard macro;
- use named commands and read-only mode;
- exit with clean, dirty, multiple, and special buffers;
- start once with Lisp and once without Lisp.

### 15.4 Documentation review

Update only where behavior actually changed:

- `README.md`;
- `doc/kg.1`;
- built-in help in `src/help.c`;
- `doc/FUZZING.md`;
- `doc/TODO.md`;
- `doc/fe-upstream.md`.

### 15.5 Acceptance criteria

- Clean recursive clone passes the complete gate.
- Manual smoke checklist has no unexplained discrepancy.
- No unintentional submodule-pointer change is present.
- Error and durability policies are documented.
- The implementation plan's completion checklist reflects reality.

---

## 16. Recommended pull-request sequence

| PR | Phase | Main deliverable | Depends on |
|---:|---|---|---|
| 1 | 0 | Clean-clone/submodule and baseline CI consistency | — |
| 2 | 1 | Signal-safe resize plus `EINTR`-safe input | 1 |
| 3 | 2 | Checked arithmetic helpers and serialization/kill-ring fixes | 1 |
| 4 | 3 | Transactional open and reload | 3 |
| 5 | 4A | Robust write-all primitive and failure tests | 3 |
| 6 | 4B | Atomic save and transactional save-as integration | 4, 5 |
| 7 | 5 | Bulk undo range deletion | 3 |
| 8 | 6 | Append-buffer OOM handling | 3 |
| 9 | 7 | Ownership map and complete shutdown | 4, 6, 8 |
| 10+ | 8 | Command, macro, layout, and fuzz coverage | Earlier relevant phase |
| Later | 9 | Const propagation and incremental keyboard extraction | 8 |
| Final | 10 | Clean integration and release-readiness validation | All intended work |

PRs 4–8 may be developed in parallel only when they do not touch the same ownership or length APIs. Rebase and rerun the full gate before merging.

---

## 17. Junior engineer task template

Use this template for each implementation issue or pull request:

```markdown
## Objective
One sentence describing the invariant or behavior to establish.

## Current behavior
Exact function names and a minimal reproducer, failing test, or analyzer output.

## Intended behavior
Observable result, including failure behavior and state that must remain unchanged.

## In scope
Small list of files/functions expected to change.

## Out of scope
Related redesigns that must not be included.

## Test first
Native test, PTY case, fuzz seed, or injected failure that demonstrates the problem.

## Implementation notes
Ownership, error propagation, portability, and compatibility constraints.

## Validation
Exact commands run and their results.

## Escalation points
Any decision that needs maintainer approval.
```

In the pull-request description, include the before/after invariant rather than only describing the code edits.

---

## 18. Escalation rules

Stop and request senior review before proceeding when a task would:

- advance the `fe` or `tiny-regex-c` gitlink;
- change `FE_API_VERSION` assumptions;
- migrate public/internal length fields broadly from `int` to `size_t`;
- choose or change save behavior for symbolic or hard links;
- change crash-durability guarantees or filesystem sync policy;
- add a global allocator/syscall interception framework;
- alter Emacs-compatible key or editing semantics;
- change final-newline policy;
- redesign the command registry or keyboard dispatcher;
- weaken or delete an existing regression expectation;
- require a new runtime dependency.

When escalating, provide two or three concrete options, their compatibility implications, and the smallest test that distinguishes them.

---

## 19. Completion checklist

### Baseline and dependencies

- [ ] Recursive submodule checkout is verified from a clean clone.
- [ ] Active CI initializes `fe/tiny-regex-c` recursively.
- [ ] `doc/fe-upstream.md` matches the actual `fe/` gitlink or is deliberately updated with it.
- [ ] Baseline test results are recorded.

### Safety and correctness

- [ ] `SIGWINCH` handler only sets a signal-safe flag.
- [ ] Terminal reads distinguish `EINTR`, EOF, and fatal errors.
- [ ] Serialization and allocation sizes use checked arithmetic.
- [ ] Kill-ring set/append preserve old state on failure.
- [ ] Failed open/reload leaves the current buffer intact.
- [ ] Failed save leaves the old file and buffer metadata intact.
- [ ] Save symlink/hard-link and durability policies are documented.

### Performance and resilience

- [ ] Yank and replacement undo use bulk deletion.
- [ ] Append-buffer OOM/overflow is explicit and tested.
- [ ] Shutdown frees all owned state exactly once.

### Tests and maintainability

- [ ] Named command dispatch has native tests.
- [ ] Macro recording/replay has native and PTY coverage.
- [ ] Resize and split-window layouts have PTY coverage.
- [ ] File-I/O failure injection is deterministic.
- [ ] Relevant fuzz seeds and smoke targets are maintained.
- [ ] Optional keyboard refactors were performed only after coverage was in place.
- [ ] Sort-line ownership was tested or documented rather than speculatively rewritten.

### Final validation

- [ ] `make format-check` passes.
- [ ] `make check` passes.
- [ ] `make check WITH_LISP=0` passes from a clean build.
- [ ] `.ci/run-ci-steps.sh` passes.
- [ ] Clean-clone validation passes.
- [ ] No unintended submodule-pointer change is present.
- [ ] Manual smoke checklist is complete.
