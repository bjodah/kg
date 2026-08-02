# Sub-plan 06-C — Fe unwind, hooks, and registry-dependent APIs

Phases 4, 5, and 6 of Plan 06.  Requires sub-plan B (runtime context and
editing primitives).

---

## Phase 4 — Implement Fe unwind before unwind-safe forms

### Problem

`save-excursion`, `with-current-buffer`, and `atomic-change-group` all
need a guarantee that cleanup code runs on every exit path: normal
return, error, `C-g`, and budget exhaustion.  Fe has a design document
(`fe/doc/unwind-design.md`) but no implementation.  Shipping the Lisp
forms without unwind means they silently leak state on errors.

### Design — Fe side

Turn `fe/doc/unwind-design.md` into tested cleanup records inside Fe:

```lisp
(unwind-protect BODY CLEANUP...)
```

Host cleanup callback:
```c
typedef void (*FeCleanupFn)(void *data);
int FeProtectWithCleanup(FeContext *ctx, FeCleanupFn fn, void *data);
```

**Invariants:**
- Cleanup runs exactly once: on normal return, on error, on interrupt
  (`C-g`), and on budget exhaustion.
- Cleanup failure has a defined diagnostic (printed, not silently
  swallowed) without skipping remaining cleanups in the chain.
- Nested cleanups run in LIFO order.
- GC roots are safe during cleanup.
- File descriptors opened during the body are closed by cleanup.

**Tests (Fe side, committed in Fe before the pin moves):**
- Normal return: cleanup runs.
- Error: body signals error, cleanup runs, error propagates.
- Interrupt: host interrupt fires mid-body, cleanup runs.
- Budget exhaustion: steps run out, cleanup runs.
- Nested: three levels of unwind-protect, inner error, all three
  cleanups run in LIFO order.
- Cleanup error: cleanup itself errors — diagnostic emitted, outer
  cleanups still run.
- Root survival: a root created in the body is still valid in the
  cleanup.
- File/resource: open a "file" (via host native), body errors, cleanup
  closes it.

### Design — kg side

Only after Fe unwind is green, implement:

#### `save-excursion`

```lisp
(save-excursion BODY...)
```

Saves point and current buffer (as runtime context state), evaluates
body, restores both on any exit.  Uses temporary markers for point so
edits during the body relocate the saved position.

Implementation: `FeProtectWithCleanup` with a closure that restores the
`kg_lisp_exec_ctx` fields saved at entry.

#### `with-current-buffer`

```lisp
(with-current-buffer BUF BODY...)
```

Sets the runtime execution context's buffer to `BUF`, evaluates body,
restores previous buffer on any exit.  Never selects a window.

#### `atomic-change-group` (conditional)

Only after Plan 02 supplies a genuinely staged multi-edit or rollback
contract.  If Plan 02's current gateway does not support rollback, defer
this form — do not fake it.

### Tasks

1. **Implement Fe unwind** in the Fe submodule.  Commit there, run Fe's
   suite.
2. **Document the divergence** in `doc/fe-upstream.md`.
3. **Move the Fe pin.**
4. **Implement `save-excursion`** using `FeProtectWithCleanup`.
5. **Implement `with-current-buffer`** using `FeProtectWithCleanup`.
6. **Evaluate `atomic-change-group`** against Plan 02's contract;
   implement or explicitly defer with a documented reason.

### Tests

- `save-excursion`: body moves point, after return point is restored;
  body errors, point is restored; body kills the buffer, diagnostic
  emitted and no crash; body inserts before saved point, restoration
  follows the marker.
- `with-current-buffer`: body operates on a non-displayed buffer,
  displayed window unchanged; body errors, context is restored; buffer
  argument is stale, error before body runs.
- Both `WITH_LISP` configurations.
- PTY cases exercising error-during-save-excursion and
  with-current-buffer on a hidden buffer.

---

## Phase 5 — Hooks through the typed event queue

### Problem

There is no general hook mechanism.  `src/event.[ch]` has a bounded
typed event queue and a safe-point drain, but no Lisp subscriber.
Ad-hoc C function pointers exist in a few places (`editor_pre_rename_hook_fn`,
`compile_diag_hooks`) but they are not a general facility.

### Design

#### Hook registries

Global and buffer-local hook lists, stored as bounded arrays of rooted
Fe callables:

```c
struct kg_hook_entry {
    FeRoot *root;          /* the callable */
    uint32_t id;           /* for remove-hook */
    uint32_t generation;   /* buffer generation for buffer-local hooks */
};
```

Maximum entries per hook: bounded (e.g. 16 per hook name per scope).

#### Hook → event subscription

Register one C-level event subscriber (via `kg_event_subscribe()`) that,
on each event, looks up matching hooks and calls them.  Each callback
gets:

- Its own guarded Fe frame.
- Its own step budget (fraction of the per-keystroke budget).
- A diagnostic label (hook name + function name) for errors.
- Handle re-resolution: the buffer handle in the event is re-resolved
  before calling the hook, so a hook that runs after a kill sees an
  error, not a reused slot.
- Error isolation: one hook's error does not prevent the next from
  running.

#### Callback ordering and reentrancy

- Callbacks generated during a hook callback queue for the next drain
  — no reentrant hook calls.
- A prompt-active safe point defers hooks that could prompt; nested
  minibuffers are rejected with a clear message.
- Queue wakeup returns control from the input wait to the main safe
  point; Fe is never called inside `read_key_byte()`.

#### Initial hooks

| Hook | Event trigger | Arguments to callback |
|------|--------------|----------------------|
| `after-change-functions` | `KG_EVENT_CHANGED` | buffer, start, end, old-length |
| `find-file-hook` | `KG_EVENT_OPENED` (for file-visiting buffers) | (none; buffer is current) |
| `before-save-hook` | `KG_EVENT_BEFORE_SAVE` | (none) |
| `after-save-hook` | `KG_EVENT_AFTER_SAVE` | (none) |
| `change-major-mode-hook` | `KG_EVENT_MODE_CHANGED` (when mode registry exists) | (none) |

#### What is deferred

- `post-command-hook`: not until its every-keystroke cost is measured
  and its semantics are specified.  The plan explicitly says do not add
  it yet.

### Lisp API

```lisp
(add-hook 'hook-name #'function)
(remove-hook 'hook-name #'function)
(run-hooks 'hook-name)              ; for C callers via adapter
```

### Tasks

1. **Define the hook registry** in `src/lisp_hooks.c` /
   `src/lisp_hooks.h` (private to adapter).

2. **Register a C-level event subscriber** that dispatches to hooks.

3. **Add hook natives**: `add-hook`, `remove-hook`.

4. **Add `after-change-functions`** first — it has the most complex
   argument passing.

5. **Add remaining initial hooks** one per commit.

6. **Test error isolation, reentrancy refusal, prompt deferral.**

### Tests

- Hook runs on save, on file open, on buffer change.
- Hook error: one hook errors, the next still runs; error message names
  the hook and function.
- Reentrancy: hook that triggers another change event does not recurse;
  the nested event queues for the next drain.
- Prompt deferral: hook tries to read from minibuffer while a prompt is
  active — rejected with a message.
- Buffer-local hooks: hook on buffer A does not fire for buffer B.
- Hook removal: removed hook does not run.
- Budget: hook that exhausts its budget is stopped; other hooks still
  run.
- `WITH_LISP=0` builds; event subscriber is not registered.
- PTY cases: `after-save-hook` that inserts a timestamp into another
  buffer.

---

## Phase 6 — Registry-dependent editor APIs

### Problem

Several Lisp APIs depend on registries that do not yet exist: mode
registration, typed variables, and a unified minibuffer session.

### Entry criteria (must each exist before this phase starts)

- **Plan 01 layered keymaps and runtime command references**: ✅ Done.
- **Explicit mode registry**: ❌ Not yet.  Modes are currently hardcoded
  in `syntax.c` and `kbd.c`.
- **Typed variable registry with per-variable file-local safety**: ❌
  Not yet.  `localvars.c` parses file-local values but there is no
  general variable metadata table.
- **Unified minibuffer session** (Plan 04 Phase 4): ❌ Deferred.
  Minibuffer input is still synchronous blocking read loops.

### APIs gated on keymaps (can proceed now)

| Native | Semantics |
|--------|-----------|
| `(define-key map key command)` | Bind in a named map |
| `(lookup-key map key)` | Return command name or nil |
| `(current-local-map)` | Return the active mode's map name |

These use the existing Plan 01 keymap infrastructure and can land as
soon as sub-plan B is complete.

### APIs gated on mode registry

| Native | Semantics |
|--------|-----------|
| `(define-derived-mode name parent ...)` | Register a new major mode |
| `(add-to-list 'auto-mode-alist ...)` | Associate file patterns with modes |

### APIs gated on variable registry

| Native | Semantics |
|--------|-----------|
| `(defvar name value doc)` | Define a typed variable |
| `(setq-default name value)` | Set default |
| `(make-variable-buffer-local name)` | Scoping |
| `(buffer-local-value name buf)` | Read |

Each variable has a file-local safety decision: safe, unsafe, or
"safe if predicate".

### APIs gated on minibuffer session

| Native | Semantics |
|--------|-----------|
| `(read-string prompt)` | Single-line input |
| `(completing-read prompt collection)` | Input with completion |

### Constraints

- Preserve emergency keys (`C-g`, `C-x C-c`) and `kg -Q` (no init).
- Keymap bindings use canonical parsed sequences (Plan 01's parser).
- Mode activation performs no Lisp callback per displayed byte.
- Completion candidates and roots are batched/bounded.

### Tasks

1. **Implement keymap APIs** (gated only on Plan 01, which is done).
2. **Document the mode/variable/minibuffer gates** explicitly so this
   phase's remaining items are not attempted prematurely.
3. **When each registry lands**, add its APIs in separate commits.

### Tests

- `define-key` / `lookup-key` with the existing keymap infrastructure.
- Emergency keys survive any keymap mutation.
- `WITH_LISP=0` build and tests pass.
- PTY case: `define-key` a mode-local key, verify it shadows the global
  binding in that mode only.

---

## Completion gate for sub-plan C

- Fe unwind is tested in Fe's own suite; cleanup runs on every exit
  path.
- `save-excursion` and `with-current-buffer` restore state on error,
  interrupt, and budget exhaustion.
- Hooks run through the event queue's safe-point drain, not inside
  `read_key_byte()`.
- Hook errors are isolated; no hook prevents another from running.
- Keymap APIs use Plan 01 infrastructure and preserve emergency keys.
- Phase 6 APIs that need missing registries are documented as blocked,
  not faked.
- Both Lisp configurations, native/PTY suites, docs check, and
  complexity ratchets pass.
