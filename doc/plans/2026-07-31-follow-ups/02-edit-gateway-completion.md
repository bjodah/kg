# Plan 02 — Complete the edit gateway

## Outcome

Every observable mutation of a live buffer publishes through one checked
gateway.  Undo replay cannot lose its record on failure.  Internal rebuilds
state their undo/dirty/event policy explicitly.  Raw row construction remains
available only for an unpublished staged buffer.  This completeness is the
precondition for converting any persistent position to a marker.

## Starting point

- `kg_buffer_replace()` stages changed text, row storage, row-array growth, and
  its `UNDO_CHANGE` record before publishing.
- Four edit paths use it.  The mutation manifest still records 209 raw
  opinions across 14 files; `buffer.c`, `word.c`, `bufmgr.c`, and `undo.c` are
  the largest groups.
- `editor_undo()` pops and frees an `UNDO_CHANGE` even when replay fails.
- `edit_publish()` destroys the replaced head row's reusable render/highlight
  capacity; measure the single-row hot path before widening gateway use.
- String rectangle accepts a quoted newline, changes row topology while
  iterating it, and cannot be restored by its coarse undo record.
- `suppress_undo` still means replay, internal/no-undo edits, and coarse command
  grouping at different call sites.
- The current mutation checker is a textual census.  It prevents growth but
  can be gamed by hiding calls in a helper; the end state must check ownership
  boundaries as well as token counts.

## Phase 0 — Close defects in the existing seam

Land each item separately with a failing test first.

### Failed `UNDO_CHANGE` replay

If `kg_buffer_replace()` refuses replay, re-link the popped record at the head,
restore `size`, leave `clean_size`/dirty/text/point unchanged, and return
without freeing it.  Add a narrowly scoped allocation-failure seam for edit
staging so the test is deterministic; do not fake this by handing replay an
invalid range.

Test both a modified and a nominally clean checkpoint.  The failed undo must
remain available and a later successful undo must work exactly once.

### String-rectangle separator policy

The first implementation refuses `\n` (and any input form that denotes a row
separator) before pushing undo or padding a row.  Report a specific message and
leave text, point, mark, undo, dirty, and generation unchanged.  Teaching a
rectangle replacement to change row count is a separate feature and requires a
different iteration/undo design.

### Preserve derived-row capacity on the hot path

Add a perf case for one-byte edit/backspace in a 1 MiB row through
`kg_buffer_replace()`.  Then preserve/reuse the replaced row's `render` and
`hl` allocations when row topology permits, starting with the common one-row
to one-row case.  Generalize reuse across multi-row replacements only when a
counter proves value.  Text atomicity and derived-state consistency remain the
primary requirement.

### Allocation-failure coverage

Exercise every allocation made before publication: deleted text, replacement
rows, row-array reserve, and undo record.  A refusal leaves serialized buffer
bytes, row count/indices, undo head/size/clean marker, dirty state, generation,
and reusable capacities coherent.  Keep the allocator seam test-only and local
to the edit module.

## Phase 1 — Make edit intent explicit

Replace the overloaded interpretation of `KG_EDIT_NO_UNDO` with a small,
validated policy describing independently:

- whether to create undo;
- whether read-only is bypassed (internal owners only);
- whether modified state advances, stays clean, or resets to a new clean
  snapshot;
- whether an ordinary change event or a broad replacement event is published;
- whether the call is undo replay.

Do not expose arbitrary flag combinations.  Named constructors such as user
edit, replay, internal live edit preserving clean state, and committed reload
make invalid combinations unrepresentable.  `content_generation` advances for
every observable text change even when a special buffer deliberately remains
`dirty == 0`.

Add a no-op rule: replacing bytes with identical bytes must not create undo,
dirty the buffer, advance generation, or publish an event.  Pin empty-buffer
and final-newline policy.

## Phase 2 — Ordinary live-buffer migrations

Each migration commit deletes at least as much hand-written mutation/undo
logic as it adds, lowers the manifest, and preserves intended undo granularity.
Prefer one precomputed replacement covering the command's affected span over a
general group API.

Recommended batches:

1. **Separator edits in `buffer.c`:** newline/split, backspace join, forward
   delete join, open line, and kill-line at EOL.  These are one logical byte
   appearing/disappearing between rows.
2. **Kill and yank:** region/word/line kills, yank, repeated yank, and the raw
   helpers used by their legacy undo opcodes.
3. **Search:** literal/regexp query replacement and replace loops.  Preserve
   the characterized rule that each accepted query replacement is its own undo
   unless a separately documented behavior change chooses otherwise.
4. **Word and command transforms:** case, transpose, join, paragraph reflow,
   whitespace cleanup, sort lines, git-rebase row transforms.  Reflow/sort are
   each one outer-span replacement.
5. **Rectangles:** build the final affected row block off to the side and
   publish once.  This removes per-row partial-failure and the coarse rectangle
   undo opcode.

For every command, add/assert: exact text, one intended undo unit, read-only
refusal, UTF-8/malformed-byte behavior, point/mark result, dirty/generation
delta, and row numbering.  Run `make gateway-check` on every batch.

## Phase 3 — Do not add a fictitious edit group

The old `kg_edit_group_begin/abort/commit` sketch promised that abort would
restore a buffer after nested helpers had already published edits.  The current
transaction cannot make that true without staging the whole command or taking
a rollback snapshot.

Use these rules instead:

- If a command can compute one final replacement, use one
  `kg_buffer_replace()`.
- If several disjoint edits are required, first reconstruct the smallest outer
  span and publish it once.
- Add a general `kg_buffer_replace_many()` only after two real consumers exist.
  It must accept sorted, non-overlapping pre-edit ranges, stage the complete
  result and undo before publication, and fail with no partial changes.
- A logical undo-boundary-only API may be added later, but it must not claim
  failure atomicity.
- Runtime `atomic-change-group` waits for Fe unwind and a genuinely staged
  multi-edit implementation.

This phase is a design gate, not necessarily a code commit.

## Phase 4 — File, shell, Lisp, and special-buffer mutations

Migrate observable live-buffer paths:

1. insert-file and reload publication;
2. shell-region output replacement;
3. compilation and shell output appended to visible special buffers;
4. Dired/help/buffer-list/special-buffer rebuilds;
5. Lisp `insert` and every later mutation native;
6. remaining legacy undo replays.

Do not flatten a staged file load through the user-edit API.  Introduce a
private staged-row builder/adoption seam:

```text
unpublished row builder -> validate/finish -> one live-buffer adoption commit
```

Raw construction is legal only before the rows are observable.  Adoption or
reload of a live buffer publishes one broad-replacement event, applies explicit
clean-state policy, invalidates dependent state once, and clamps every view by
documented policy.  Compilation/special buffers are already observable while
being appended; they use an efficient internal live-edit/append gateway even
though they remain read-only and clean.

## Phase 5 — Retire legacy mutation machinery

After the last caller moves:

- delete the eleven legacy undo opcodes and their replay splices, leaving
  `UNDO_CHANGE` (renamed simply when useful);
- delete `suppress_undo` and map its former meanings to explicit policies;
- make row mutators private to the staged builder/edit implementation;
- replace the textual census with a structural check that only the edit module
  can publish changes to an active buffer; retain token counts as a useful
  secondary ratchet;
- arm a debug invariant at top-level safe points: no live buffer text changed
  without generation advancing through the gateway.

The final manifest may name constructor code, but no module outside the buffer
mutation owner may write live `erow` text fields or hand-write undo.

## Completion gate

- Failed undo replay retains its record and dirty truth.
- Every observable live-buffer mutation uses one gateway/adoption commit.
- Staged load construction is unobservable and has one publication point.
- Markers can now rely on one relocation seam.
- `suppress_undo` and legacy undo variants are gone.
- Render/highlight allocation counters do not regress on the single-row hot
  path.
- `make gateway-check`, both Lisp configurations, fuzz seed replay, sanitizers,
  and the full runner pass without raising a ratchet.
