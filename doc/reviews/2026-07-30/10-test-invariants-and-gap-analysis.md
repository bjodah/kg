# Test invariants and gap analysis

Scope: a read-only comparison of kg, Fe, and tiny-regex-c tests against the
state machines they exercise. This report deliberately does not repeat the
general lint/static-analysis/fuzzing inventory. It concentrates on properties
whose violation would corrupt editor state, retain/reclaim the wrong Fe object,
or make an interactive operation fail to make progress.

## Executive triage

| Priority | Finding / proposed model | Status | Cost | Expected signal |
|---|---|---:|---:|---:|
| P0 | Regexp iteration must advance by a whole glyph after a zero-width match | Confirmed defect | Small | Very high |
| P0 | Undo eviction must invalidate an unreachable saved-clean state | Confirmed defect | Small | Very high |
| P1 | Preserve `prefix.supplied` separately from a numeric value of zero across `C-x` | Confirmed defect | Small | High |
| P1 | Model buffer/window switching as ownership plus per-window-view invariants | Major untested state machine | Medium | Very high |
| P1 | Add tiny-arena, forced-collection Fe root/error tests | Important weak spot | Medium | High |
| P2 | Turn regex fuzzing into an iterator/property harness, not only a crash harness | Test weakness | Small | High |
| P2 | Stateful UTF-8 edit/motion/undo model over valid and malformed byte strings | Test weakness | Medium | High |
| P2 | Stateful minibuffer/prefix/input model including cancellation and overflow | Test weakness | Medium | Medium-high |

## Confirmed defects

### P0: query-replace-regexp can revisit the same zero-width match forever

The wrapper intentionally snaps a match boundary inside UTF-8 back to the
glyph's lead byte (`src/regex.c:41-54`). That behavior is already specified by
the unit test that starts `"."` at byte 1 of a two-byte glyph and expects the
match to move back to byte 0 (`test/test_regex.c:404-423`).

Query replacement, however, advances a zero-width match by exactly one **byte**:
after replacement at `src/search.c:948-950`, and after skipping at
`src/search.c:951-954`. On a multibyte glyph, a zero-width match at byte 0
therefore advances to byte 1; the next wrapper call at
`src/search.c:863-868` snaps the result back to byte 0, and the loop assigns byte
1 again. With `!`, no further input is read and the loop is unbounded; with
`n`, the user is repeatedly offered the same match.

A minimal interactive matrix is a row beginning with `å`, regexp `a*`, empty
replacement, and answers `!` and `n`. The required invariant is:

> For every iterator step, either the buffer/region bound changes, the row
> increases, or the next search offset is strictly beyond the previous match
> in glyph order.

The same progress helper should be shared by query replacement and any future
match iterator. Advancing should use `utf8_glyph_span_at`, with an explicit
end-of-row transition. This is more robust than relying on a wrapper to accept
mid-glyph offsets.

Why existing tests miss it: zero-length matching is tested only on ASCII
(`test/test_regex.c:64-88`), UTF-8 offset snapping only with a consuming dot
(`test/test_regex.c:404-434`), the fuzzer makes only one forward call at offset
zero (`test/fuzz_regex.c:54-60`), and the differential driver likewise compares
only the first match (`test/regex_differential.c:63-71`).

### P0: undo-stack eviction can mark the wrong contents clean

`undo_mark_clean()` represents the saved state solely as an operation count
(`src/undo.c:338-339`). When the bounded stack overflows, old operations are
discarded and `undostack.size` is reduced, but `clean_size` is not adjusted or
invalidated (`src/undo.c:82-107`). Later, `editor_undo()` clears dirty whenever
the new size numerically equals that stale count (`src/undo.c:324-327`).

Concrete sequence with `undostack.max_size = 2`:

1. edit A; mark clean (`clean_size == 1`);
2. edit B; edit C, evicting A while size remains 2;
3. undo C, leaving contents A+B and size 1;
4. kg declares A+B clean, although the saved contents were A.

The production limit is 1000 (`src/def.h:366`), so this is latent rather than
rare in long sessions. The focused tests cover ordinary clean-count behavior
(`test/test_undo.c:271-291`) but never reduce `max_size` or cross an eviction
boundary.

Add a small-capacity model test with a reference snapshot. The property is:

> `dirty == 0` iff current bytes equal the last successfully saved bytes.

At minimum, eviction of any record needed to reach the clean checkpoint must
set `clean_size = -1`. A byte-snapshot oracle is preferable because it also
tests multi-record commands and buffer-local stack switching.

### P1: numeric prefix zero is lost at the `C-x` boundary

Prefix state correctly distinguishes `supplied` from `value`
(`src/def.h:253-256`, `src/kbd.c:565-578`), but `C-x` stores only the numeric
value (`src/kbd.c:758-762`). Its consumers reconstruct supplied-ness with
`> 0`: macro replay turns zero into one repetition (`src/kbd.c:433-435`), and
`C-x C-e` treats an explicit zero prefix as no prefix (`src/kbd.c:437-440`).

This contradicts the stated “C-u N C-x e repeats N times” behavior and, more
fundamentally, makes a valid value indistinguishable from absence. Add PTY
cases for `C-u 0 C-x e` and `C-u 0 C-x C-e`, plus a table-driven input test
covering absent/0/1/MAX across direct, `C-x`, `C-c`, and `M-x` dispatch.
Store a `struct command_prefix` for sub-prefixes rather than an integer
sentinel.

## Highest-value missing state models

### P1: buffer/window ownership and view switching

This is the largest test blind spot. The live editor owns the active buffer's
row pointer, filename pointer, undo stack, marks, modes, and cursor; buffer
slots receive shallow ownership copies in `buf_save_to_slot`
(`src/bufmgr.c:45-85`) and restore them in `buf_restore_from_slot`
(`src/bufmgr.c:88-139`). Windows separately own view state
(`src/winmgr.c:13-58`). Every focus/split/delete/display transition depends on
the save/restore ordering (`src/winmgr.c:184-377`).

There is no native `test_winmgr` and searches of the test tree find no direct
calls to the real split/cycle/delete functions; the keypress fuzzer seeds only
one buffer and one window (`test/fuzz_keypress.c:24-49`, `:79-88`). A few PTY
shell/output and visual-line cases cover individual outcomes, not ownership.

Build a native state-machine test linked to real `bufmgr.c`, `winmgr.c`,
`buffer.c`, and `undo.c`. Generate transitions:

- create/open A and B; edit; set mark/mode/readonly/compile command;
- split horizontally/vertically; show same or different buffers;
- move and scroll independently; cycle; replace another window's buffer;
- edit shared buffer from either window; undo in A and B;
- delete current/others; kill a visible buffer; reload/auto-revert;
- resize to narrow/short legal dimensions after every topology.

Check after each step:

- exactly `win_count` and `buf_count` active slots;
- every active window names an active buffer;
- `buf_current == winlist[win_current].bufidx`;
- the live row/filename/undo pointers equal the current buffer's owned values;
- two windows on one buffer share contents and undo history but retain distinct
  cursor/scroll views;
- switching away and back preserves all buffer-local fields;
- no inactive slot retains sole ownership of freed rows, filename, or undo
  records;
- every cursor and viewport is valid for its window dimensions and buffer.

Cost is medium because current test stubs deliberately sever this integration;
signal is very high because shallow-copy ownership failures become UAF,
double-free, cross-buffer undo, or silent data loss.

### P1: Fe forced-GC and nonlocal-error matrix

Fe's core rule is that allocation may collect (`fe/fe.c:515-527`) and only the
GC stack, symbols, evaluation/call result roots, and persistent roots are marked
(`fe/fe.c:421-447`). Errors reset evaluator control and call trace but leave
host GC checkpoint restoration to the recovering caller
(`fe/fe.c:248-281`). These are exactly the invariants that richer Lisp and more
native kg bindings will stress.

The API suite is strong on ordinary recovery, root lifetime, nested evaluation,
and stack balance (`fe/test_api.c:302-359`, `:396-503`, `:559-624`), but most
behavior runs in a fixed 64 KiB arena (`fe/test_api.c:37-43`). Context creation
tests the exact minimum only by open/close (`fe/test_api.c:68-99`), not by
allocating through evaluator/native/error paths.

Add a parameterized arena sweep from `FeMinimumArenaSize()` upward and force a
collection between every pair of object-producing operations. Exercise:

- every primitive with allocation in argument 1 and argument 2;
- long strings spanning many Fe string cells;
- `FeMakeList`, `FeCall`, `FeCreateRoot`, release, then forced reuse;
- native callback returns, native callback re-entry, and error from each
  allocation position;
- `longjmp`, host `FeRestoreGC`, then repeated successful evaluation;
- mark/GC callbacks whose external objects point back to Fe objects;
- nested evaluation sharing the outer budget, including errors at every depth;
- `FeCloseContext` after each recovered failure and exactly-once external
  finalization.

The reference properties are: balanced GC depth on every successful public
call documented as balanced; rooted object rendering remains stable across
arbitrary allocations; released/unrooted objects may be collected but rooted
ones never are; recovery restores a reusable context; external resources are
finalized exactly once. Also add a deep-`car` mark test under a controlled
limit: the collector is recursive there (`fe/fe.c:379-419`) and the limitation
is documented, but it deserves a measurable safety budget before Lisp grows
larger structures.

### P2: regex iteration properties

Keep the existing Emacs differential oracle, but compare **all successive
matches**, not just the first. Include forward and backward iteration from
every glyph boundary, zero-width matches, captures, anchored patterns, empty
subjects, and malformed UTF-8 for kg-only safety properties. The existing
fuzzer currently has no assertions beyond sanitizer survival and invokes one
forward/one backward operation (`test/fuzz_regex.c:54-60`).

For each successful result assert:

- participating spans are ordered, in bounds, and on the chosen UTF-8 boundary
  policy;
- capture 0 makes iterator progress even when empty;
- backward results are non-increasing and respect `before`;
- compiling/matching twice is deterministic;
- an iterator takes at most `glyph_count + 1` successful empty-match steps per
  row.

Feed both the wrapper and `fe/tiny-regex-c/re.c` directly so wrapper snapping
cannot hide an engine defect. Raise long-hunt differential budgets separately
from CI; the generator itself documents a prior four-per-million bug
(`utils/regex_differential.py:19-25`).

### P2: UTF-8 editing as a stateful byte/glyph model

Existing focused coverage is good for width, conversions, transpose,
overwrite, minibuffer backspace, and UTF-8 undo. What remains missing is the
composition of those operations. Extend the keypress fuzzer: its seeded buffer
maps input to ASCII/tab only (`test/fuzz_keypress.c:51-76`), so it cannot reach
most UTF-8 editor invariants.

Use a small reference model over ASCII, 2/3/4-byte scalars, combining marks,
wide glyphs, truncated leads, overlong encodings, and stray continuations.
After random insert/delete/overwrite/transpose/motion/mark/kill/yank/undo:

- rows remain NUL-terminated and sizes match;
- cursor/mark never bisect a well-formed glyph;
- forward then backward motion round-trips at glyph boundaries;
- byte-to-character-to-byte and visual-column conversions round-trip where
  injective (with explicit equivalence classes for zero-width/wide cells);
- one edit followed by its complete undo restores exact bytes, cursor, dirty
  truth, and row count.

### P2: minibuffer, prefixes, and input cancellation

`test/test_minibuf.c` explicitly tests only pure history and backspace helpers,
not the prompt loop (`test/test_minibuf.c:1-4`). PTYs cover representative
editing/history cases, but not a transition matrix.

Drive `editor_read_line_with_history` through the fake input fd and model
`{buffer bytes, cursor, overflow, history index, draft, echo cursor}`. Cross:

- insert/delete/move/kill/yank at start/middle/end;
- exact capacity, one glyph over capacity, multiple overflowed glyphs;
- edit a recalled item, walk older/newer, and restore the original draft;
- ESC/C-g/Enter/EOF after every state;
- quoted control input and malformed/truncated UTF-8;
- prompt and input containing combining/wide glyphs.

Separately model the prefix parser over all key categories. Assert that any
terminal command or cancellation consumes the pending prefix exactly once,
that prefix sub-state cannot leak to the next command, that the cap is stable,
and that `{supplied,value}` survives `C-x`, `C-c`, and `M-x`. This model would
catch the confirmed zero-prefix defect structurally rather than one binding at
a time.

## Recommended implementation order

1. Fix and regress the UTF-8 zero-width regexp progress bug.
2. Fix clean-checkpoint invalidation on undo eviction and add the snapshot
   property test.
3. Replace integer prefix sentinels with `struct command_prefix`; add the zero
   cases.
4. Add the real buffer/window state-machine harness before extending window or
   buffer Lisp APIs.
5. Add Fe forced-GC/tiny-arena/error-injection tests before enriching Fe object
   types, lexical environments, or native callbacks.
6. Upgrade regex and keypress fuzzers to assert progress/state invariants, then
   run high-budget differential hunts off the fast CI path.

The first three are small fixes with direct regressions. The next two are
architectural safety nets: without them, richer Lisp and editor extensibility
increase the number of shallow-ownership and GC-root transitions faster than
example-based tests can cover.
