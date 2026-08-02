# Sub-plan 06-D — Bounded process table, package hygiene, and proofs

Phases 7 and 8 of Plan 06.  Requires sub-plan C (unwind for
filters/sentinels, hooks for proof packages).

---

## Phase 7 — Bounded process table and Lisp process APIs

### Problem

`src/process.[ch]` provides low-level fork/reap/signal primitives used
by `compile.c` (async non-blocking) and `shell.c` (synchronous
blocking).  There is no multi-process table, no process handle, no
output streaming into the event queue, and no Lisp API for spawning
processes.

### Design

#### Step 1 — Explicit argv vs. shell interpretation

Add a spawn variant that distinguishes:

```c
struct kg_process_spawn_opts {
    const char *const *argv;    /* explicit argv, or NULL for shell */
    const char *shell_command;  /* only when argv is NULL */
    const char *cwd;
    int stdin_fd;               /* -1 for /dev/null */
    int stdout_fd;              /* -1 for pipe to process table */
    int stderr_fd;              /* -1 for merge with stdout */
    bool new_process_group;
};
```

Explicit `argv` is **never** interpreted by `/bin/sh`.

#### Step 2 — Bounded process table

```c
#define KG_PROCESS_TABLE_MAX 8

struct kg_process_entry {
    struct kg_process_handle handle;   /* generation-checked */
    pid_t pid;
    pid_t pgid;
    int output_fd;
    enum kg_process_status status;     /* running, exited, signalled */
    int exit_code;
    struct kg_buffer_handle target_buffer;
    /* Queued output: bounded ring buffer */
    char *output_queue;
    size_t output_queue_len;
    size_t output_queue_cap;           /* per-process cap */
    /* Filter and sentinel roots (Phase 5 hooks) */
    /* FeRoot *filter_root; */
    /* FeRoot *sentinel_root; */
};
```

**Policies:**
- Slot reuse: a finished process keeps its terminal status queryable
  until the slot is explicitly released or reused by a new spawn.
- Output cap: per-process output queue cap (e.g. 1 MiB).  Overflow
  truncates oldest output and appends a truncation message.
- Process-buffer ownership: the process holds a buffer handle; if the
  buffer is killed, output is discarded and the process keeps running
  (can be deleted explicitly).
- Cancellation escalation: `delete-process` sends SIGTERM to the
  process group, waits briefly, then SIGKILL.
- Shutdown cleanup: `kg_process_table_shutdown()` kills all children
  and reaps.
- Root release: filter and sentinel roots are released when the process
  is deleted.
- Output is committed through `kg_buffer_replace()` with
  `KG_EDIT_INTERNAL_LIVE` intent.

#### Event integration

Add new event kinds to `src/event.h`:

```c
KG_EVENT_PROCESS_OUTPUT,   /* payload: process handle, byte range */
KG_EVENT_PROCESS_EXIT,     /* payload: process handle, exit code */
```

Output arrives in the main poll loop (via `select`/`poll` on the output
fd).  Events are published to the queue.  Filters and sentinels run only
from the Phase 5 drain — never from inside `read_key_byte()`.

#### Lisp API

| Native | Semantics |
|--------|-----------|
| `(start-process name buffer program &rest args)` | Explicit argv spawn; returns process object |
| `(start-shell-command name buffer command)` | Intentional `/bin/sh -c`; returns process object |
| `(process-live-p proc)` | Generation check + status check |
| `(delete-process proc)` | SIGTERM → wait → SIGKILL; release slot |
| `(process-buffer proc)` | Return the target buffer object |
| `(set-process-filter proc fn)` | Set the filter function (runs on output event) |
| `(set-process-sentinel proc fn)` | Set the sentinel function (runs on exit event) |
| `(process-status proc)` | Return `run`, `exit`, `signal` |

Never expose a PID as identity.  The process handle is the only
identity token.

### Tasks

1. **Add explicit-argv spawn** to `src/process.[ch]`.  Test: spawn
   `/bin/echo hello`, verify output without shell interpretation.

2. **Build the process table** in `src/process_table.c` /
   `src/process_table.h`.  Test: spawn, reap, slot reuse, full table
   refusal.

3. **Add event integration**: output fd polling, event publication.
   Test: process produces output, event arrives, subscriber sees it.

4. **Add Lisp natives** one per commit.

5. **Add filter/sentinel support** (requires Phase 5 hooks).

6. **Shutdown cleanup**: verify all children are reaped on `kg -Q` /
   normal exit.

### Tests

- **Explicit argv**: `/bin/echo` without shell interpretation; verify
  no shell metacharacter expansion.
- **cwd**: process runs in the specified directory.
- **Process groups / grandchildren**: SIGTERM to process group kills
  grandchild.
- **Two concurrent children**: both produce output; both are reaped.
- **Output cap / queue overflow**: process produces > 1 MiB; truncation
  message appears.
- **Cancellation**: delete-process sends signals in order.
- **Prompt idle delivery**: output event is delivered at the safe point,
  not during a prompt.
- **Stale handles / PID reuse**: old process handle does not resolve to
  a new process with the same PID.
- **Sentinel error / queued sentinel**: sentinel errors do not prevent
  other sentinels; sentinel queued during another sentinel runs at the
  next drain.
- **Shutdown**: all processes are killed and reaped on exit.
- Both `WITH_LISP` configurations.

---

## Phase 8 — Package hygiene and proof packages

### Problem

kg has `load`, XDG init-file resolution, and a basic package directory
search, but no `provide`/`require`, no load-path control, no
docstrings, and no published API contract.

### Design

#### Package infrastructure

| Feature | Semantics |
|---------|-----------|
| `(provide feature)` | Register a symbol as provided |
| `(require feature &optional filename)` | Load if not already provided; error if still not provided after load |
| `(featurep feature)` | Check without loading |
| `load-path` | Bounded list of directories searched by `require` |
| Load-cycle detection | Error if `require` is reentered for the same feature |
| Source byte offsets | Error messages report file:line (Fe must provide line info) |
| Docstrings | Optional string after `(defun name args doc ...)` — stored, queryable by `describe-function` |

#### API documentation

Publish `doc/lisp-api.md` covering:

- Supported forms and their Emacs analogues.
- Object lifetimes and generation checking.
- Position units (1-based codepoints externally, bytes internally).
- Safe-point and callback ordering rules.
- Error handling and budget limits.
- Emacs differences (explicit, not aspirational).
- Trust model: init files and packages are trusted code, not a sandbox.

#### Proof packages (in order)

Each package gets:
- Its own file under a `pkg/` or `lisp/` directory.
- An isolated-HOME PTY test (using `config_files:` in the YAML).
- A `WITH_LISP=0` non-regression test (the package is absent, the
  editor works).

| # | Package | Demonstrates | Dependencies |
|---|---------|-------------|--------------|
| 1 | **whitespace-mode** | Decorations + buffer-local option + mode hook | Phase 5 hooks, Plan 03 decorations |
| 2 | **conf-mode** (small derived config mode) | Syntax/comment options + local keymap | Phase 6 mode APIs (when available) |
| 3 | **auto-fill-mode** | `after-change-functions` + one editing transaction | Phase 4 unwind, Phase 5 hooks |
| 4 | **grep** (optional) | Process-backed async grep | Phase 7 process table |

**Rule:** if a proof package needs private C knowledge, improve the
public adapter rather than adding a package-specific native.

### Tasks

1. **Add `provide` / `require` / `featurep`.**

2. **Add bounded `load-path` management.**  Default: XDG config dir +
   `~/.config/kg/lisp/`.

3. **Add load-cycle detection.**

4. **Add docstring storage and `describe-function` integration.**

5. **Write `doc/lisp-api.md`.**

6. **Implement proof packages in order**, each with its own PTY test.

### Tests

- `require` loads a file; second `require` is a no-op.
- `require` of a file that does not `provide` → error.
- Load-cycle detection: A requires B, B requires A → error.
- Docstrings: `describe-function` shows the docstring.
- Each proof package:
  - PTY test with isolated HOME and `init.fe` that loads the package.
  - `WITH_LISP=0` test verifying the package's absence is harmless.
  - whitespace-mode: visible trailing-whitespace decoration.
  - auto-fill: line breaks at fill-column after typing past it.

---

## Completion gate for sub-plan D

- Explicit-argv spawn never passes through `/bin/sh`.
- Process table is bounded; handles are generation-checked.
- Output is committed through the edit gateway; filters/sentinels run
  from the event drain.
- `provide`/`require`/`featurep` with load-cycle detection.
- Versioned `doc/lisp-api.md` is published.
- At least two proof packages (whitespace-mode and one other) are green
  with isolated-HOME PTY tests.
- Both Lisp configurations, native/PTY suites, docs check, static/
  sanitizer lanes, and the full CI runner pass without ratchet raises.
