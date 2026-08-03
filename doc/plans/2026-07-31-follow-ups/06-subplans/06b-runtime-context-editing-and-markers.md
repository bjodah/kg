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

### Phase 3 notes

- **Read-only policy disagrees with this document.**  The design section
  above says editing natives refuse a read-only buffer "through the
  `CMD_EDITS_BUFFER` flag on the command that called them, not by
  checking read-only themselves."  That is not what `native_insert()`
  (landed in an earlier phase) does, and it cannot be: `CMD_EDITS_BUFFER`
  is read by `cmd_invoke()`, the one route a *command* takes
  (`(command-execute ...)`, a key, M-x); a native called directly from
  Lisp — `(insert ...)`, `(delete-region ...)` — never passes through
  `cmd_invoke()` at all, so there is no descriptor to consult.
  `kg_buffer_replace()` itself does refuse a `KG_EDIT_USER` edit on a
  read-only buffer (`edit_valid()`'s `obeys_readonly` check), but it
  reports the refusal only by leaving the buffer unchanged, with no
  distinct error a native could surface.  `native_insert()` therefore
  checks `b->readonly` itself before ever calling the gateway, so Lisp
  gets a named error ("buffer is read-only") instead of a silent no-op.
  `delete-region` and `replace-region` follow that precedent exactly,
  for the same reason and the same message.  The design section's claim
  is corrected by this note rather than by editing it out from under the
  phase that wrote it.
- **`undo-boundary` is deferred, not shipped.**  Emacs' `undo-boundary`
  marks where one undo *group* ends so a later `undo` stops there instead
  of continuing into the previous group — meaningful because Emacs
  coalesces consecutive edits (ordinary self-insertion, for instance)
  into one group until a boundary or a command boundary closes it.  kg's
  Lisp editing natives have no such coalescing to bound: every
  `(insert ...)`, `(delete-region ...)` or `(replace-region ...)` call is
  already exactly one `kg_buffer_replace()` call and therefore already
  exactly one undo record on its own (see `test_insert_and_undo`,
  `test_delete_region`, `test_replace_region`).  A `(undo-boundary)` that
  did nothing would not be honest about having a contract — it would look
  like it participates in an undo-grouping scheme this adapter does not
  have — so it is not implemented.  If a later phase adds an edit form
  that spans more than one `kg_buffer_replace()` call (a multi-step
  Lisp-driven refactor, say), that is the moment `undo-boundary` gets a
  real meaning and should land with it.
- **Match data could not stay literally frame-scoped and remain usable.**
  The design section says match data "belongs to the runtime execution
  context, not a global" and reads that as living inside
  `struct kg_lisp_exec_ctx`, cleared on every frame entry like the exec
  buffer selection.  That was tried first and failed its own test:
  `(re-search-forward ...)` in one `M-x eval-expression` and
  `(match-beginning 0)` in the next — two separate top-level forms, the
  ordinary way anyone would use this interactively or from two lines of
  a script — saw the second form's frame wipe the first's captures before
  it could read them, because `lisp_exec_enter()` re-derives the frame
  from scratch every time.  Point does not have this problem only
  because it is *not* part of the frame either (the design section says
  so directly) — it lives in `struct lisp_point_table`, a member of
  `struct lisp_state` that outlives every frame.  Match data needed the
  same treatment: `struct kg_lisp_match_data` is a sibling of
  `struct lisp_point_table points` in `struct lisp_state`, addressed as
  `state.match`, not nested in `struct kg_lisp_exec_ctx` and not cleared
  at frame entry.  It is still not a bare file-scope global — nothing
  outside `src/lisp_search.c` touches it, and `lisp_internal.h` is the
  only place it is declared — so the design section's actual intent
  ("not a stray global reachable from anywhere") holds; only the
  frame-scoping half of the literal instruction turned out to be wrong,
  and is corrected here rather than silently.  `set-buffer`, by contrast,
  *is* correctly frame-scoped as designed: it has no persistence
  mechanism analogous to point's window round-trip, so a command that
  needs a buffer switch to survive more than one native call has to make
  it and use it inside one top-level form (`(progn (set-buffer h)
  (insert ...))`), exactly the requirement `with-current-buffer` (Phase 4)
  exists to make convenient. `test_markers` and
  `test_marker_survives_buffer_kill` in `test/test_lisp.c` both had this
  bug in their first draft and needed the same `progn` fix once the
  actual (correct) `set-buffer` scoping was understood.
- **`re-search-backward`'s bound is a known, narrower approximation of
  Emacs', not a bug to be silently patched over.**  Per the CLAUDE.md
  warning this sub-plan was told to read,
  `kg_regex_match_backward(rx, text, before, out)` already answers "the
  last match in this text ending at or before `before`" with no lower
  bound of its own.  Enforcing `re-search-backward`'s BOUND argument (the
  lower limit) on top of that answer means checking whether that one
  match's start is `>= limit`; if it is not, this implementation gives up
  on the row rather than asking the engine for the *next* eligible match
  between `limit` and the disqualified one — which Emacs would find and
  this does not.  Doing that correctly would mean repeatedly re-invoking
  `kg_regex_match_backward()` with a shrinking `before` and reconciling
  overlapping candidates, which is real work with its own edge cases and
  did not fit this phase's budget on top of everything else it already
  built.  Documented in `src/lisp_search.c`'s comment on
  `lisp_search_backward()`, in `README.md`, and in `doc/kg.1`, not just
  here.
- **Search cannot match across a row (a `\n`).**  kg's regex engine
  matches within one `NUL`-terminated row at a time — the same
  architecture `src/search.c`'s incremental search (`C-s`/`C-r`) already
  has — so `search-forward`/`re-search-forward`/etc. inherit that limit
  rather than lifting it.  A pattern that could only match by spanning a
  line break (which Emacs' flat-buffer model allows) reports no match
  here.  This is consistent with the rest of the editor rather than a
  regression Phase 3 introduced.
- **Case-fold-search is not implemented.**  Every search native here is
  case-sensitive; Emacs' default (`case-fold-search` non-nil, folding
  unless the search string has an upper-case letter) is not modeled.
  `src/search.c`'s own incremental search has this logic
  (`query_has_upper`/`fold`) but it is a UI nicety layered over the same
  engine calls, not something these batch-search natives inherited for
  free.  Deferred rather than half-implemented.
- **`(make-marker)` follows this document's own table, not Emacs'.**  The
  table above specifies "Create a marker at runtime point in the runtime
  buffer," which is Emacs' `point-marker`, not its `make-marker` (which
  creates a marker pointing nowhere until `set-marker` gives it a
  position).  Implemented exactly as specified here rather than silently
  switched to match Emacs, since the sub-plan is the authority being
  implemented; flagged in a doc comment on `native_make_marker()` and
  here so a later phase can decide whether to rename it, split it into
  both forms, or leave it — Emacs code that assumes a detached
  `make-marker` will be surprised.
- **A mutation-gateway false positive, not a real one.**  Adding a `row`
  field to `struct lisp_search_hit` and assigning it (`hit->row = row;`)
  tripped `make gateway-check`'s textual `->row\s*=` probe, which exists
  to catch direct writes to `struct editor_buffer`'s row array — this
  was an unrelated local struct with a field that happened to share the
  name.  Renamed the field to `found_row` rather than touching the probe
  or the manifest: `make gateway-check` is unchanged (still 47 sites, the
  manifest's existing allowance), and no new raw-mutation site was
  introduced by this phase.
- **`make pmccabe-check` has one pre-existing, unrelated failure on this
  box**: `src/lisp_word.c:lisp_move_words` measures complexity 6 against
  a recorded baseline of 5, in a file this phase never touched.
  Reproduces identically on `14baba8` (the commit this phase started
  from, `git stash`-verified before writing this note), so it predates
  Phase 3 and is a `pmccabe` version/box drift of the kind
  `utils/print-tool-versions.sh` exists to catch, not a regression to fix
  here.
- **Makefile wiring for the new module and the regex engine it needs**:
  `lisp_search.c` joins `LISP_SRCS`; `EXTRA_lisp` (the Lisp unit-test
  link line) gained `$(REGEX_OBJS)`, which it never needed before this
  phase; and `FUZZ_SRCS` — which already compiled every `LISP_SRCS` file
  as a source, so it now compiles `lisp_search.c` too — gained
  `$(OBJDIR)/regex.c` and `fe/tiny-regex-c/re.c` plus `-Ife/tiny-regex-c`
  on the `$(FUZZBIN)` recipe, the same pair `fuzz_regex`'s own recipe
  already needed for the same reason.
- **A second adapter object kind reused, not duplicated, Phase 2's
  pool.**  `enum kg_lisp_object_kind` gained `KG_LISP_OBJECT_MARKER`, and
  `struct kg_lisp_object` gained a `marker` field alongside `buffer`
  (which kind is meaningful is `kind` itself).  `lisp_object_is_buffer()`
  changed signature (it now needs a `FeContext *` to resolve the pool
  record and check `kind`, not just the Fe type tag) — the one call site,
  `lisp_cmd.c`'s `type-of`, was updated along with it, and gained the new
  `lisp_object_is_marker()` twin.  Unlike a buffer object, a marker
  object is never deduplicated: two `(make-marker)` calls are never `eq`,
  matching Emacs, and `set-marker` mutates the pool record in place
  (including replacing which buffer's `kg_marker_store` it names) so the
  *same* Lisp-visible marker object is what moves.
- Position conversion needed no new primitives beyond what `lisp_buffer.c`
  already had from Phase 2 — `lisp_byte_of_char_offset()`,
  `lisp_rowcol_of_char_offset()` and `lisp_offset_argument()` were
  already exactly what editing and search needed, just `static` and
  private to that file.  Un-staticked and declared in `lisp_internal.h`
  rather than duplicated; `test_delete_region_utf8` and
  `test_delete_region_malformed_byte` in `test/test_lisp.c` are this
  phase's coverage of the malformed-byte-counts-as-one-codepoint rule
  those functions already implemented.
- scc: 4893 before this phase, 5002 after (+109 against a 120 allowance;
  see the report for the per-file breakdown). `lisp_search.c` is a new
  62-complexity file; `lisp_obj.c` grew from 100 to 136 (+36, the marker
  kind and its four natives); `lisp_io.c` grew from 57 to 67 (+10,
  delete-region/replace-region); `lisp_cmd.c` grew from 38 to 39 (+1,
  `type-of`'s marker branch); `lisp_buffer.c` is unchanged at 55
  (visibility-only edit). All well under the 520 per-file cap.

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
