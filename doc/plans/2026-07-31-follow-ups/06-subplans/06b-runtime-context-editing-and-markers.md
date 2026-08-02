# Sub-plan 06-B — Runtime context, buffer objects, and editing primitives

Phases 2 and 3 of Plan 06.  Requires sub-plan A (decomposed adapter and
direct Fe calls).

---

## Phase 2 — Explicit runtime execution context and buffer objects

### Problem

Every Lisp native today calls `bcur()` — the active window's buffer.
There is no way to operate on a buffer that is not displayed in the
current window.  `with-current-buffer` needs a temporary binding of a
different buffer, but doing that by swapping `bcur()` or `buf_current`
would move the displayed cursor, confuse the renderer, and violate
Plan 04's handle invariants.

### Design

#### Object representation

Expose editor objects to Fe as opaque adapter-owned values, never as raw
C pointers, slot indices, or PIDs:

```c
struct kg_lisp_object {
    enum kg_lisp_object_kind kind;  /* BUFFER, MARKER, PROCESS, … */
    uint32_t id;
    uint32_t generation;
};
```

Each object resolves through the existing generation-checked handle
tables (`buf_resolve`, `kg_marker_resolve`, etc.).  A stale
handle signals a Lisp error; the old slot is never silently reused.

Print representation: `#<buffer 3 "foo.c">` — human-readable, never
parsed back.

If Fe gains per-context custom types later, migrate the Fe-visible
wrapper without changing the adapter representation.

#### Runtime execution context

```c
struct kg_lisp_exec_ctx {
    struct kg_buffer_handle buffer;    /* selected buffer for this Lisp frame */
    struct kg_marker_handle point;     /* runtime point, not the window's */
    /* optional: mark state, match data — later phases */
};
```

- **Entry from interactive command**: initialise from the active window
  (copy the window's buffer handle and current point).
- **Hidden-buffer operation**: resolve the explicit handle; do not select
  a window, move displayed point, or change `buf_current`.
- **Sync back**: on successful return from an interactive command,
  synchronise the runtime point back to the window.  Specify the exact
  moment this happens.

#### Where point lives — decided 2026-08-02

A single `point` field in the exec context is not enough, because point
in Emacs is a property of the *buffer*, not of the frame.  Two behaviors
fall out of a single field, and both are wrong:

1. `(goto-char 4)`, `(set-buffer other)`, `(set-buffer back)` loses the
   4: coming back re-derives point from the window cursor, which never
   moved.
2. A hidden buffer edited by one command starts the next command at
   position 1, because nothing remembered where the previous frame left
   off in it.

So the runtime point is **per buffer**, held in a bounded adapter-owned
table keyed by the generation-checked buffer handle:

```c
struct kg_lisp_point_entry {
    struct kg_buffer_handle buffer;
    struct kg_marker_handle point;     /* right gravity, per Plan 03 */
};
/* MAX_BUFFERS entries: a frame cannot select more distinct live buffers
 * than exist.  An entry whose handle stops resolving is evicted, which is
 * what makes room after a kill. */
```

The rules, stated so they can be tested one by one:

- **Frame entry** takes the active window's buffer as the exec buffer and
  *overwrites* that buffer's table entry from the window's cursor — the
  user may have moved point since the last frame, and the window is the
  authority for the buffer it displays.
- **`set-buffer`** resolves the handle, looks up (or creates) that
  buffer's entry, and makes it the exec buffer.  It never selects a
  window, never touches `buf_current`, and never re-derives point for a
  buffer that already has an entry.
- **A buffer's entry outlives the frame.**  Markers are buffer-owned, so
  the entry dies with the buffer and needs no frame-scoped cleanup.
- **Successful frame exit** syncs only the *entry* buffer's point back to
  the active window, and only while that window still shows it.  Every
  other visited buffer keeps its runtime point in the table and its
  window, if any, stays where it was — the "hidden work never moves a
  displayed window" invariant outranks Emacs' redisplay-follows-point
  behavior here, and the two-window PTY case is what pins it.
- **Failed frame exit** syncs nothing.  Runtime points stay where the
  body left them (point in Emacs does not roll back on error either);
  only the window is protected.

`b->last_point` stays what its comment says it is — written by
`buf_remember_view()` alone, for view detach — and the adapter neither
writes it nor reads it.

#### Buffer APIs (incremental)

| Native | Semantics |
|--------|-----------|
| `(current-buffer)` | Return the runtime context's buffer object |
| `(buffer-name [buf])` | Already exists; extend to take an optional buffer argument |
| `(buffer-list)` | Return a list of live buffer objects |
| `(get-buffer name)` | Find by name, return buffer object or nil |
| `(get-buffer-create name)` | Find or create |
| `(buffer-live-p buf)` | Generation check → t/nil |
| `(set-buffer buf)` | Set the runtime context's buffer (not the window's) |
| `(kill-buffer buf)` | Kill; refuse if modified and unsaved without prompting |

#### What is deferred

- `with-current-buffer` → Phase 4 (needs unwind for safe restoration).
- Marker objects → Phase 3.
- Process objects → Phase 7.

### Tasks

1. **Define `struct kg_lisp_object` and the adapter registry.**
   Generation-checked, bounded, type-tagged.  Put in `src/lisp_obj.c` /
   `src/lisp_obj.h` (private to `src/lisp_*.c`).

2. **Define `struct kg_lisp_exec_ctx`.**  Put in `lisp_internal.h`.
   Initialise from the window on interactive entry, do not touch window
   state from hidden-buffer operations.

3. **Refactor existing buffer natives** (`point`, `goto-char`,
   `buffer-name`, etc.) to read from the runtime context instead of
   `bcur()`.  This must be behavior-neutral for interactive commands
   where the context is the active window's buffer.

4. **Add buffer object natives** incrementally, one per commit:
   `current-buffer`, `buffer-list`, `get-buffer`, `get-buffer-create`,
   `buffer-live-p`, `set-buffer`, `kill-buffer`.

5. **Test every edge case** listed below.

### Tests

- **Slot reuse**: create buffer, kill it, create another — old Lisp
  object must not resolve to the new one.
- **Kill during queued work**: kill a buffer while Lisp closures
  referencing it are queued; closures that run later must see an error,
  not a different buffer.
- **Hidden edits / undo**: set-buffer to a hidden buffer, insert text,
  undo — the displayed window must not move.
- **Two windows on same buffer**: runtime context for a hidden-buffer
  operation must not confuse either window's point.
- **Stale handle**: buffer-live-p returns nil for a killed buffer;
  operations on it signal an error.
- **`WITH_LISP=0`**: build and link without Lisp; all non-Lisp tests
  pass.
- PTY cases: at least one case exercising `set-buffer` + `insert` into
  a non-displayed buffer, then switching to verify the text is there.
- **Point round-trip**: `(goto-char N)`, `set-buffer` away and back,
  insert — the insertion lands at N, not at point-min.
- **Point across frames**: one command inserts into a hidden buffer, a
  second command `set-buffer`s to it and inserts — the second insertion
  lands where the first left point.

### Phase 2 notes

- Fe has no per-context custom type facility, so a buffer object is a
  `FeTFex0` value made with `FeMakePtr` over a record in a bounded
  adapter pool (`src/lisp_obj.[ch]`), released by the `FeSetGCFn`
  callback when Fe collects the wrapper.  Only the adapter mints
  `FeTFex0`, so a buffer object cannot be forged from Lisp; resolution
  re-checks the record *and* the generation-checked handle.
- Two non-prompting bufmgr entry points back the natives:
  `buf_create_named()` (create without selecting or attaching a window)
  and `buf_kill_buffer()` (kill an arbitrary handle, refusing a modified
  buffer and the last live one).
- `buf_kill_commit()` freed `bcur()`'s undo stack rather than the dying
  slot's — latent while only the interactive `buf_kill()` reached it, a
  real leak-plus-corruption once Lisp can kill a hidden buffer.  Fixed
  with the phase, called out here because it is a behavior change that
  the split otherwise hides.
- Word motion is re-expressed over the exec buffer rather than the
  window, so it must not re-derive (row, col) from a flat byte position
  per scanned byte: both conversions are O(rows).  Walk rows and columns
  directly.

---

## Phase 3 — Editing, search, and marker primitives

### Problem

Lisp code today can read positions and insert text at point, but cannot
delete, replace, search, or create markers.  All mutation must go
through Plan 02's edit gateway (`kg_buffer_replace()`), and read-only
policy must be honored.

### Design

#### Editing natives (one per commit)

| Native | Gateway call | Notes |
|--------|-------------|-------|
| `(delete-region start end)` | `kg_buffer_replace(buf, start, end, "", 0, …)` | 1-based codepoint positions; convert at adapter seam |
| `(insert text)` | Already exists; verify it uses the gateway | |
| `(replace-region start end text)` | Single `kg_buffer_replace` | One edit, one undo step |
| `(undo-boundary)` | Only if semantics are honest: "this is where a logical undo step ends" | Defer if the contract is unclear |

All editing natives refuse in a read-only buffer through the
`CMD_EDITS_BUFFER` flag on the command that called them, not by
checking read-only themselves — the command descriptor is the single
read-only verdict.

#### Search natives

| Native | Semantics |
|--------|-----------|
| `(re-search-forward pattern &optional bound)` | Bounded regex search from runtime point; return position or nil |
| `(re-search-backward pattern &optional bound)` | Likewise, backward |
| `(search-forward string &optional bound)` | Literal search |
| `(search-backward string &optional bound)` | Literal search |
| `(match-beginning n)` | Capture group start from last search |
| `(match-end n)` | Capture group end |

Search must return truthful statuses: no-match (nil), too-complex
(error), and cancellation (C-g).  Match data belongs to the runtime
execution context, not a global.

#### Position conventions

External Lisp positions are **1-based codepoint offsets** (Emacs
convention).  Internal core is byte-positioned.  Conversion happens at
the adapter seam:
- `lisp_pos_to_byte(buf, codepoint_pos) → byte_offset`
- `byte_to_lisp_pos(buf, byte_offset) → codepoint_pos`

Malformed UTF-8 bytes count as one codepoint each.

#### Marker primitives

Depend on Plan 03 marker handles (already landed).

| Native | Semantics |
|--------|-----------|
| `(make-marker)` | Create a marker at runtime point in the runtime buffer |
| `(set-marker marker pos [buf])` | Move marker |
| `(marker-position marker)` | Resolve; nil if stale |
| `(marker-buffer marker)` | Buffer object; nil if stale |

#### What is deferred

- `save-excursion` → Phase 4 (needs unwind for safe restoration of
  point, mark, and current buffer).
- `save-match-data` → Phase 4.

### Tasks

1. **Add position conversion helpers** in `lisp_buffer.c`.  Test with
   ASCII, multi-byte UTF-8, malformed bytes.

2. **Add editing natives** one per commit.  Each one adds a native test
   case and a PTY case.

3. **Add search natives** with the regex engine already in
   `src/regex.[ch]`.  Test: simple match, no match, too-complex,
   capture groups, cancellation.

4. **Add marker natives.**  Wrap `kg_marker_create`, `kg_marker_resolve`,
   `kg_marker_delete`.

5. **Add match-data to the runtime execution context.**  One set of
   capture registers per frame, not global.

### Tests

- **UTF-8 / malformed bytes**: `delete-region` across a multi-byte
  character; `goto-char` to a codepoint in the middle of a multi-byte
  sequence.
- **Stale handles/markers**: delete-region on a killed buffer; marker
  operations after the buffer is killed.
- **Hidden buffer editing**: set-buffer, delete-region, verify the
  displayed window didn't move.
- **Read-only refusal**: editing in a read-only buffer signals an error
  before any partial work.
- **One undo**: delete-region is one undo step; replace-region is one
  undo step.
- **Regex exhaustion / cancellation**: search that exceeds the regex
  complexity limit; search interrupted by C-g.
- **Captures**: `match-beginning`/`match-end` return correct 1-based
  codepoint positions.
- **Error rollback**: an error mid-operation leaves the buffer unchanged.
- `WITH_LISP=0` builds and links.

---

## Completion gate for sub-plan B

- Buffer objects are generation-checked adapter values, never raw
  pointers or slot indices.
- Runtime execution context is separate from active-window globals;
  hidden-buffer operations never move the displayed cursor.
- All edits go through `kg_buffer_replace()`.
- External positions are 1-based codepoints; conversion is at the
  adapter seam.
- Search returns truthful statuses (nil / error / cancel) and match
  data belongs to the execution context.
- Marker primitives wrap Plan 03 handles.
- Both Lisp configurations, native/PTY suites, docs check, and
  complexity ratchets pass.
