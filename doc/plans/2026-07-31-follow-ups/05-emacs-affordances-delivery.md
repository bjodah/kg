# Plan 05 — Dependency-ready Emacs affordances

## Status (2026-08-02, after Bundle E)

The first delivery train is complete: Bundles A, B, C, D and E have all
landed, in that order.

- **A — bounded kill ring, coalescing, `M-y`.**  16 entries and 8 MiB,
  refusing an oversize entry and evicting oldest-first otherwise.
  Coalescing is a command *class* (`cmdstate.h`'s `kill_coalesce_class`),
  so a kill that follows an unrelated motion starts a new entry — the
  characterization commit pinned the old behaviour first, and the fix is
  its own documented commit.  `M-y` replaces the previous yank's span,
  repeat count included, in one undo unit.
- **B — `transpose-words` on `M-t`.**  Built on `word_boundary_forward()`,
  extracted first as the interactive word-boundary policy; Unicode word
  policy is deliberately unchanged and still a separate future change.
- **C — introspection.**  `describe-key`, `describe-command`,
  `describe-bindings` and `where-is` are Plan 01's, and are bound and
  reachable.  Two items of this bundle's *polish* remain open and are
  not blocking anything: generating the packed help table's text from
  the registry still needs a second, ~15-column summary per command, and
  `docs-check` still runs one direction only (help key → man page), which
  `utils/check_help_drift.py` documents as deliberate rather than
  missing.
- **D — navigable compilation.**  A pure bounded parser
  (`compile_parse.c`), marker- and decoration-backed diagnostic records
  (`compile_nav.c`), `M-g` turned into a prefix map, and `M-g n` /
  `M-g p`.  No metadata is encoded in buffer text.
- **E — registers.**  `src/register.[ch]`: 32 slots keyed by one
  printable ASCII byte, holding either a marker-backed position or exact
  bytes (1 MiB per register, 4 MiB across the table).  `C-x r SPC`,
  `C-x r j`, `C-x r s`, `C-x r i`.  Every bound refuses rather than
  evicting — a register is named by the user, so the kill ring's
  drop-the-oldest rule would discard the one thing they asked to keep —
  and a killed buffer reports a stale register instead of jumping into
  whatever took the freed slot.

Deferred from Bundle E, deliberately: persistent bookmarks, which the
bundle already describes as a later, separately reviewed file-format
feature (atomic writes, relocation fingerprints).  Nothing in this plan
depends on it.

## Outcome

Ship high-value Emacs habits in small product slices that consume the new
registries, edit gateway, markers/decorations, and process services.  No bundle
adds a one-off key decoder, policy table, raw mutation path, or hidden
unbounded store.

## Bundle rules

Every command has a descriptor, stable identity/class where needed, keymap
binding, summary, pure-logic tests, PTY interaction tests, bounded resource
policy, read-only/undo/UTF-8 coverage, and README/man/help updates.  Check
behavior from key and M-x; add `C-c` and Lisp parity only when policy says the
command is bindable/callable.  Do not silently widen the deliberately tested
`CMD_LISP_CALLABLE` set.

The first delivery train is A–E below.  Later bundles have explicit entry
criteria and remain unscheduled until those criteria are green.

## Bundle A — Bounded kill ring, coalescing, and `M-y`

Dependencies: enough Plan 01 migration to fund code and decode/bind generic
`M-y`; Plan 02 for atomic span replacement.  Plan 03 markers are preferred for
the retained yank span but are not mandatory for a strict immediate-command
first cut: no edit is legal between yank and yank-pop.

Create self-contained `src/yank.h`; keep rectangle and minibuffer stores
separate in this bundle.  The ring owns at most 16 entries and 8 MiB total,
with `size_t` lengths and arbitrary bytes.  Reject an individual oversize entry
without changing the ring; otherwise evict oldest entries until both caps hold.
OOM/overflow preserves the old ring.  Command transient state, not the ring,
owns yank-pop's current entry and inserted span.

Characterize and then fix current coalescing.  The man page says only
consecutive `C-k` appends, but the implementation appends across an unrelated
motion.  Add PTYs for `C-k C-k C-y`, `C-k C-n C-k C-y`, and
`C-u 3 C-k C-y`, then change the second result in its own documented commit.

Use a command coalescing class rather than exact identity:

- consecutive forward kills append;
- consecutive backward kills prepend;
- a copy starts a new entry;
- unrelated/refused/undefined commands, prompt entry, `C-g`, buffer switch, or
  ring mutation break eligibility.

Refactor prefix-batched kill/yank so one top-level command owns one undo unit
without inspecting pointers/length deltas inside global ring storage.

`C-y` selects newest.  Immediate `M-y` replaces the prior inserted span with
the next older entry, wraps, stays eligible, and creates one undo.  Track buffer
handle, start byte/marker, inserted length, ring generation, ring index, and
repeat count.  A different buffer or stale marker refuses without text change.
Define prefix zero/positive/negative behavior explicitly; if `C-u N C-y`
inserts N copies, yank-pop replaces that entire repeated span with N copies of
the selected entry.

Tests: ring wrap/eviction/caps/OOM, append/prepend, embedded NUL/UTF-8, all kill
producers, multiple buffers, multiline spans, intervening command/prompt/C-g,
read-only, undo, macros, M-x, and documented prefix behavior.

## Bundle B — `transpose-words` on `M-t`

Dependencies: Plan 01 generic Meta keymap and descriptor; Plan 02 one-span
replacement.  Markers are unnecessary.

Extract a documented current interactive word-boundary helper in `word.c`.
Do not silently combine this command with Unicode word-policy unification:
today interactive word commands are ASCII-oriented while one Lisp thing-bound
helper treats non-ASCII as a constituent.  First make `M-t` follow the current
interactive policy and document deterministic non-ASCII/malformed-byte
behavior; schedule Unicode unification as a separate behavior change with
Emacs oracle evidence.

Find the two words chosen around point, then publish one replacement:

```text
second word + untouched separator + first word
```

Preserve punctuation, whitespace, and newline separators.  Pin BOB/EOB,
inside-word and between-word point, repeated prefix, read-only, one undo,
UTF-8/malformed policy, and clear no-two-words refusal.

## Bundle C — Registry-backed command introspection

Dependencies: Plan 01 through layered maps and command IDs.

Plan 01 owns the implementation of `describe-key`, `describe-command`,
`describe-bindings`, and `where-is`; this bundle owns product polish and docs.
Add completion where the shared minibuffer session exists, mode precedence,
stale runtime binding diagnostics, and readable narrow wrapping.

The packed built-in help table cannot be generated from the existing 60-column
summaries alone.  Add an explicit width-checked short/help summary before
generation; until then validate keys and keep the art static.  Extend docs
checking in both directions: help key to man page and keymap-visible documented
key to help/man policy.

## Bundle D — Navigable compilation

Dependencies: parser logic may start now; navigation waits for Plan 03 markers
and decorations.  No general process table is needed for the existing single
compilation process.

Create a pure, bounded diagnostic parser for common `file:line[:column]`
families with cwd and source-line metadata.  It must reject overflow and cap
retained diagnostics.  Add corpus-style native tests before UI wiring.

As output commits, attach marker-backed diagnostic records and a decoration to
the compilation buffer.  `next-error`/`previous-error` open or select the file,
move point, maintain a command-owned cycle cursor, report stale/missing files,
and never embed metadata in buffer text.  Define recompilation invalidation and
multi-window behavior.  Add `M-g n`/`M-g p` only through the keymap and update
help/docs.

Tests cover relative/absolute paths, spaces, UTF-8, column absent/present,
truncation, rebuild, edits before a marker, killed/reused source buffer, failed
open, read-only source, and two windows.

`M-g` is not free: it is bound directly to goto-line today (`ALT_G`).
Turning it into an Emacs-style prefix map moves goto-line to `M-g g` /
`M-g M-g`, a user-visible binding change that needs its own commit with
README/man/help updates and a PTY case, before `M-g n`/`M-g p` land.

## Bundle E — Registers

Dependencies: Plan 01 `C-x r` map; Plan 03 markers; Plan 04 stable buffer/view
handles.

Add a fixed bounded register table keyed by a character.  Start with
`point-to-register` (`C-x r SPC`) and `jump-to-register` (`C-x r j`) storing a
marker plus documented view-restoration policy.  A killed buffer/stale marker
reports refusal, never jumps to reused storage.  Then add bounded text
registers using exact bytes.  Persistent bookmarks are a later, separately
reviewed file-format feature with atomic writes and relocation fingerprints.

Tests cover edits before/through the saved point, slot reuse, two windows,
UTF-8 position conversion, full table, stale files for bookmarks, and
persistence failure if that phase is approved.

## Later bundles and entry criteria

### Backup and autosave recovery

May start after existing file-identity/save conflict semantics and a typed
variable registry define safe file-local/global policy.  Specify backup timing,
mode/ownership preservation, symlink policy, autosave naming, recovery prompt,
cleanup, conflict ordering, and atomic failure tests before code.  This must not
be hidden inside ordinary save refactoring.

### Display line numbers

Wait for Plan 07's single visual geometry/index API and a mode/variable
registry.  Extract gutter rendering from `draw_window_rows`; subtract stable
gutter width from every wrap calculation; test tiny terminals, horizontal/
vertical splits, visual wrapping, scrolling width changes, and cached number
formatting.  Never alter buffer text.

### Occur and project navigation

`occur` needs bounded regex iteration, marker/decorated result records, and
completion.  Project file discovery needs a documented root policy and bounded
results.  Async project grep waits for Plan 06's process table/events.  Report
truncation distinctly from no matches.

### First Lisp modes/packages and completion-at-point

Wait for Plan 06's unwind-safe callback, mode/keymap/variable, hook, and
decoration APIs.  First proofs are whitespace mode, a small derived config mode,
then auto-fill.  A package needing private C knowledge is evidence to improve
the public adapter, not permission for a package-specific native.

## Completion gate for the first train

- Bundles A–E meet their exact prerequisites and completion tests.
- `M-y` and `M-t` are ordinary keymap bindings and visible to introspection.
- Persistent positions use markers where edits may intervene.
- Compilation diagnostics/decorations are bounded and never encoded in text.
- README/man/help/TODO match shipped behavior.
- Both Lisp configurations, native/PTY suites, docs check, static/sanitizer
  lanes, and full CI pass without raising complexity/coverage ratchets.
