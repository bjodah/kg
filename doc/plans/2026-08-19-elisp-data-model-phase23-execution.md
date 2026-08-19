# Phase 23 execution: landing the payload substrate, and what kg sees of it

Date: 2026-08-19
Status: proposed.  This is the phase plan Phase 23 of
`doc/plans/2026-08-18-elisp-data-model.md` requires now that the Phase 22
ADR has selected Design B (stable `FeObject *` headers over a
bump-allocated, compactable payload region inside the caller's arena).
The spike code on `spike/phase22-designs` (`386fdba`..`97cf0d3`) is the
reference for what was measured; per the ADR it does not land -- this
phase re-implements the design at release quality, test-first, under the
maintainer's standing directive that readability of implementation wins
over cleverness.

Scheduling: after the simplification plan's Phase A and B
(`2026-08-19-fe-simplification-and-cheap-compat.md`) -- deferral removal
so no stub machinery is ported onto the new allocator, and the arena
knob so the options-bearing open API has one authoritative size source.

Out of scope, per the master plan: reader syntax, any Lisp-visible type
(Phase 24), string migration (25), symbol indexing (26).  Phase 24-26
execution plans are authored at their own boundaries, informed by this
phase's recorded margins -- not now.

---

## 23.0 -- the interior-pointer census (entry gate)

nelisp's compaction work documents the exact failure class a movable
payload region invites: C code holding a raw pointer into a payload
across an allocation that compacts it
(`/opt/nelisp/docs/design/147-box-layout-container-shrink.org`, the
stale-pointer inventory around its Phase-2 notes).  fe today has no
movable payloads, so the census is cheap NOW and expensive after.

Deliverables, all before any allocator code:

1. A tracked census (`fe/doc/payload-pointer-census.md`): every site in
   fe C and in kg's `src/lisp_*.c` that will read through a payload
   pointer once one exists -- today that is the audit of the patterns
   that would migrate in Phase 25 (`BuildString`-style constructors,
   `copy_fe_string()`, the printer, `strcmp` in interning) plus every
   place the spike's harness touched payloads.  Each row: site, whether
   an allocation can occur while the pointer is live, and the rule that
   makes it safe (re-read after allocation, or the publish protocol
   below).
2. ONE publish protocol, stated in `fe_internal.h`'s comments and
   enforced by debug assertions: a payload pointer is obtained
   immediately before use, is invalid after ANY allocation, and a
   new/replacement block is published to its owning header only through
   the one internal function that also handles the rooting across its
   own allocation.  This is the ADR's "one publish protocol" condition
   made concrete, with the census as its checklist.
3. A debug-build poison mode (`FE_DEBUG_PAYLOAD_MOVE=1`, the
   `KG_DEBUG_COORDS` pattern): every allocation memmoves the payload
   region by one alignment unit and pattern-fills the vacated bytes, so
   any held-across-allocation pointer fails loudly in the sanitizer
   lanes rather than corrupting quietly on the rare natural compaction.
   This knob is the census's enforcement arm and a CI lane flag, not a
   shipped feature.

Gate: 23.1 does not start until the census exists and the poison mode is
wired into a `.ci` lane.

## 23.1 -- the substrate lands in release fe

Scope is the ADR's recorded shape; deviations need a written reason:

- `fe_internal.h` gains the private surface the spike sized (3
  functions, 1 struct, ~7 `FeContext` fields): block allocate, block
  publish/replace, compact; nothing in `fe.h`.
- The collector integrates payload marking as a new arm of the existing
  Deutsch-Schorr-Waite trampoline -- no recursive helper -- and the
  compactor preserves allocation order (deterministic addresses for a
  given history, which is what makes its tests exact).
- Exhaustion: `(payload-exhaustion)` and its degradation to
  `(arena-exhaustion)` under cell pressure, both catchable, both proved
  by direct test as the spike did.
- The internal child-bearing TEST object from the master plan proves
  mark/compact order without exposing any type: it exists only in the
  test build (the `FE_GC_STRESS`-style pattern).

Verification (master plan's list, made concrete):

- allocator unit tests: exact fit, one-byte-over, alignment, stale
  replaced blocks, sliding overlap, deterministic compaction addresses;
- live/dead mixtures with owners at the arena's front/middle/back;
- failure injection at every construction allocation;
- both GC-stress builds and the sanitizer lanes, plus the 23.0 poison
  lane;
- close-context with live payloads;
- the C-stack depth proof re-run (the spike's fourth commit: width via
  the trampoline, depth flat at 100k-deep aggregate chains);
- all Phase 21 workloads: unchanged results, and the run's arena
  margins recorded in this phase's results section.

Ratchets: the spike measured scc 867->899 and pmccabe 1285->1313 for
the whole design; the release implementation's actual numbers are
banked/raised with the bisect in the commit message, per house rules.
`FE_API_VERSION` moves once, on the commit that extends `FeArenaStats`;
`FE_LANGUAGE_VERSION` does not move in this phase.

## 23.2 -- the kg interface

What kg sees, and every file it touches:

| surface | change | kg consumer |
| --- | --- | --- |
| `FeOpenContextWithOptions(bytes, opts)` | new; `opts` carries the payload carve (default: the ADR's selected split) and room for later knobs; `FeOpenContext` remains the documented default wrapper | `src/lisp_core.c:406` passes the Phase-B arena knob's bytes and the default carve |
| `FeArenaStats` | gains payload capacity, live bytes, high-water bytes, compaction count, payload allocation failures; existing cell fields keep their exact meaning | `arena-stats` command output (`src/lisp_cmd.c`), `kgbatch -g`, `test/prelude_gc_probe` |
| perf counters | fe: payload alloc/compact counters in `fe_perf.h`; kg: `KG_PERF_SET` mirrors for the stats fields, the existing pattern at `src/lisp_core.c:1617-1619` | `test/test_perf.c` gains the shape assertions (compactions deterministic per workload; zero payload use in the prelude until Phase 25) |
| census | `.ci/prelude-startup-census.json` gains the payload fields AT ZERO -- the prelude allocates no payloads until Phase 25, and a nonzero value appearing early is exactly what the ratchet should catch | `utils/check_prelude_census.py` |
| facade | `src/lisp.h` unchanged; no editor module learns any of this | -- |

PTY/oracle surface: none -- nothing Lisp-visible changes in this phase,
which is itself asserted (the oracle corpus and PTY suite must pass
unmodified, and `kg -V` gains no word).

## 23.3 -- exit criteria and choreography

1. fe suites green including the new lanes; fe's own `.ci` runners green
   (the submodule's green light that a pin move requires).
2. kg `make check` green in both `WITH_LISP` configurations with the
   extended stats threaded through; census re-baselined with the payload
   fields at zero.
3. This document gains its results section: the measured margins (cell
   pool and payload pool against the one-third conditions, at the
   default arena AND at one deliberately small arena), the ratchet
   moves, and anything the census (23.0) found that the ADR did not
   predict.
4. The pin moves in a kg commit that cites 1-3.

Stopping rule, inherited from the master plan: if the release
implementation cannot reproduce the spike's O(1)-by-counter result or
its margins, stop at 23.1 and take the failure back to the ADR rather
than adjusting the gate.

---

## 23.1 results

Landed in fe as two commits on `more-elisp-work`:

  `b208d66`  Read the live set a gc workload leaves, not the garbage it
             stopped on
  `44efb18`  The payload substrate: stable headers over a compactable
             in-arena region

and pinned here by the commit this section is part of.  fe's full
`.ci/run-ci-steps.sh` (all ten steps, ci-04 arming `FE_DEBUG_PAYLOAD_MOVE`)
exits 0 at `44efb18`; kg's `make -j8 check` exits 0 with 59/59 native and
588 PTY cases (583 pass, 5 skip).

### The private surface, as landed

Against the plan's "3 functions, 1 struct, ~7 `FeContext` fields":

| plan | landed |
| --- | --- |
| block allocate; block publish/replace | `PublishPayload(ctx, owner, children, bytes)` -- ONE function |
| -- | `PayloadBytes(ctx, handle)`, the one accessor |
| compact | `CompactPayloads(ctx)` |
| 1 struct | `struct FePayloadBlock` (owner, children, bytes, mark_cursor; 32 bytes) |
| ~7 fields | exactly 7: `payload_base`, `payload_capacity`, `payload_start`, `payload_used`, `payload_peak_used`, `payload_compaction_count`, `payload_allocation_failures` |

Three functions, as sized, but not the three the plan named.  Allocation and
publication are one function because the protocol forbids the state between
them existing: a block no owner names yet is exactly what the compactor
reclaims, so there is no safe moment for a caller to stand in.  The third
name is the accessor instead, which is clause 1 of the protocol and has to be
in the surface anyway.

Three declarations sit beside that surface and are counted separately rather
than folded into it: `OpenContextWithPayload` (the internal context open that
takes a carve; 23.2 turns it into the public options-bearing API),
`RaisePayloadExhaustion` (a raise helper, living with fe's other raises in
`fe_unwind.c`), and `MovePayloadRegion` (23.0's poison step, still compiled
only under `FE_DEBUG_PAYLOAD_MOVE`).  23.0's `ResetPayloadRegion` is gone: a
real per-context region needs no reset, because a fresh context is the known
starting state.

`fe.h` is untouched.  `FE_API_VERSION` stays 12 and `FE_LANGUAGE_VERSION`
stays 15, the latter because nothing a Lisp program can write reaches any of
this -- no release type owns a payload until Phase 25, so
`(payload-exhaustion)` is release code that only a test build can reach.  That
is stated in `fe_internal.h`, in `doc/implementation.md`'s new "The payload
region" section, and beside the raise itself.

### Deviation: `FeOpenContext` carves nothing

The one departure from this plan's brief, taken deliberately and recorded in
`44efb18`'s message.  `PayloadArenaPercent` is the ADR's 25 and is what
23.2's options API will default to, but `FeOpenContext` passes **0**, so the
arena partition is byte-for-byte what it was.

Three reasons, in the order they decide it.  The ADR says so in as many
words -- "This is the SPIKE's split, not a shipped constant: Phase 23 owes an
options-bearing context-open API ... with `FeOpenContext` keeping today's
behaviour as the default".  This plan's own verification list requires the
Phase 21 workloads to give unchanged results, and a 25% default does not
merely change `gc-dense-live`'s numbers: it leaves that workload 8933 cells
for a live set of 9521, so it raises out of memory.  And kg would lose a
quarter of its cells to a region nothing can allocate from for two more
phases.

What the ADR's split would cost, measured here so 23.2 has the number rather
than an estimate (`OpenContextWithPayload` at 25%, frame capacity identical
in every row):

| arena | cells at 0% | cells at 25% | payload bytes at 25% | frames |
| --- | --- | --- | --- | --- |
| 640 KiB (the floor) | 34026 | 25746 | 132480 | 677 |
| 1 MiB (kg's default) | 56145 | 42335 | 220952 | 1086 |
| 2 MiB (`$KG_LISP_ARENA_BYTES=2M`) | 115127 | 86572 | 456880 | 2179 |

42335 cells beside 220952 payload bytes is the ADR's own figure for kg's
arena, reproduced by the release implementation to within the floor
divisions.  Frames are funded before the split and are untouched by it, which
is the ADR's "no unpriced split" rule.

### The Phase 21 workloads: how compared, and what moved

Method: `make perf-workloads` writes a machine-readable record per workload
(`perfobj/workloads.json`, schema `fe-perf-workloads/2`).  The battery was run
at the pre-23.1 tip and again at `44efb18`, and the two JSON files were
compared field by field -- every workload's `answer`, `cell_capacity`,
`context_open_cells`, all 45 counters and the four per-workload probes.  Not
by eye, and not by re-reading the printed tables.

**All 20 workloads pass at both ends.**  Every counter difference is one of
two arithmetic consequences, and there is no third:

1. **The `payload-exhaustion` condition row adds fourteen objects and one
   symbol to what a context open builds.**  `context-open` and
   `context-open-close` move by exactly that: `alloc_object` 892 -> 906,
   `alloc_symbol` 108 -> 109, `alloc_string`/`string_cell` 226 -> 233,
   `alloc_pair` 470 -> 476, `intern_miss` 108 -> 109.  Every workload that
   interns anything pays the one extra obarray candidate per miss:
   `intern_candidate` and `name_compare` rise by exactly the number of misses
   each workload makes (`intern-8192` +8193, `intern-1024` +1025,
   `intern-128` +129, `list-walk` +4, `arithmetic-loop` +2).  Every workload
   that collects marks fourteen more permanent objects per collection:
   `gc_mark_new` +630 over `gc-sparse-garbage`'s 45 collections, +34 over
   `gc-dense-live`'s 3, +14 over `arithmetic-loop`'s 1.
2. **`FeMinimumArenaSize()` grows by 280 bytes**, so a FIXED arena has two
   cells fewer: `cell_capacity` 2694 -> 2692, 11910 -> 11908, 56147 -> 56145,
   233094 -> 233092, and `gc_sweep_examined` falls by two per collection.
   224 of the 280 are the fourteen objects above; 56 are the seven new
   `FeContext` fields.

**Three answers moved, all three by exactly one, all three saying so:** the
intern tiers report how many symbols the obarray held before their miss, so
`intern-128` reads "miss scanned 237" against 236, `intern-1024` 1133 against
1132 and `intern-8192` 8301 against 8300.  That is the new condition symbol,
counted.  The other seventeen answers are identical strings.

**One assertion changed, and it is a correction rather than an
accommodation** (fe `b208d66`, landed as its own commit before the
substrate).  `CheckSparse` and `CheckDense` each end in a clause about "the
live set it leaves" and each was reading the live set PLUS whatever garbage
had accumulated since the last collection.  The free list runs out on a
schedule set by how much each collection frees, so that residue is a phase,
not a property: the fourteen new permanent objects make every collection in
`gc-sparse-garbage` fire fourteen allocations earlier, and over 45
collections the residue drifts from 110 to 830 while every counter the
workload actually asserts on is unchanged.  `SettledLive` forces a collection
and reads the live set after it, which is what both comments already claimed.
Verified before the substrate landed: it changes no recorded number.

### Arena margins

**In release, the payload margin is 0 bytes used of 0 bytes carved**, in every
Phase 21 workload and in both `gc_stress` builds, because `FeOpenContext`
carves nothing and no release type owns a payload.  That is the honest figure
and it is the one the ADR predicts for this phase; the cell margins the
battery prints are therefore also unchanged in kind, `gc-dense-live` still
the tightest at 19.6% of 11908 cells free and `gc-sparse-garbage` at 35.5%
of 2692.

Where a region exists -- `payload_tests.c`, which opens at
`PayloadArenaPercent` -- the measured margins are:

| case | payload high-water | of capacity |
| --- | --- | --- |
| carve, 256 KiB arena | -- | 44008 bytes beside 9158 cells (11908 with no region) |
| bump order (4 blocks) | 168 B | 0.4% |
| live/dead mixture (300 owners of 56 B) | 16800 B | 38.2% |
| exact fit | 44008 B | 100%, deliberately -- the edge is the case |
| one byte over | 0 B | refused, `payload_allocation_failures` 1 |

Frame capacity is reported beside both pools in every row above, per the
ADR's rule that a re-split must price frames: it is **identical** at 0% and
25% in all three arena sizes, because the carve is taken after the frame
region is funded.

### The C-stack depth proof

Re-run by the method the ADR records for the spike's fourth commit: a chain
of `n` aggregates, each holding the next in its payload, with an `FeTPtr`
object at the bottom whose `mark_fn` reads `__builtin_frame_address(0)` from
inside the mark phase, against an `n = 0` baseline.

```
payload_tests: mark probe n=    10 baseline=0x7ffd61ae1e00 probe=0x7ffd61ae1e00 delta=0 bytes
payload_tests: mark probe n=  1000 baseline=0x7ffd61ae1e00 probe=0x7ffd61ae1e00 delta=0 bytes
payload_tests: mark probe n=100000 baseline=0x7ffd61ae1e00 probe=0x7ffd61ae1e00 delta=0 bytes
```

**Delta 0 bytes at every depth**, in the ordinary build, in the GC-stress
build's sibling binary and under ASan+UBSan in fe's ci-04 (the run quoted
above is ci-04's).  The assertion is `delta < 2048`, the same tolerance the
pair chain's probe uses; a recursive payload walk would miss it by megabytes
at n = 100000.  The chain is walked all the way back down afterwards and the
`ptr` found at the end, so the walk really went that deep and pointer
reversal put every link back.

WIDTH is proved separately and structurally: the mark arm is a new arm of the
existing Deutsch-Schorr-Waite trampoline with the block's `mark_cursor`
standing in for the `GcMarkCdrBit` a pair uses, so an aggregate of n children
costs one C frame however large n is.

### Ratchets

| gate | before | after | bisect |
| --- | --- | --- | --- |
| fe scc total | 874 | **903** | fe.c 194 -> 221, fe_unwind.c 119 -> 120, headers 9 -> 10; fe_eval.c 404, main.c 43 and the rest unmoved |
| fe scc per file | 520 | 520 | unmoved; worst file still fe_eval.c at 404 |
| fe pmccabe total | 1294 / 412 symbols | **1333 / 429** | +42 in 19 new symbols, +4 in `FeMark` (8 -> 12), -7 for `OpenContext` (renamed) and `ResetPayloadRegion` (deleted) |
| fe pmccabe per function | 22 | 22 | unmoved; worst still `RunEvaluationLoop`/`ResumeEvalList` at 15 |
| fe pmccabe per symbol | -- | one regression banked | `fe.c:FeMark` 8 -> 12, with `--allow-regressions` |
| kg prelude census `peak_live_objects` | 10979 | **10993** | +14, the condition row |
| kg prelude census `reachable_live_objects` | 10002 | **10016** | +14, the same |
| kg prelude census `embedded_bytes` / `definition_count` | 71720 / 122 | unmoved | the prelude itself did not change |

The ADR priced the whole design at +32 scc and +28 pmccabe; measured, +29 and
+39.  Both raises were proved live in both directions (the gate fires one
below the new cap naming the actual, and passes at it), and both bisects are
in the fe Makefile's own comment and in `44efb18`'s message.

### kg-side adaptation

The pin costs kg two object slots and one evaluator frame, all of it from
`FeMinimumArenaSize()`'s 280-byte growth and none of it from the region
(which is 0 bytes here).  Corrected in the pin-move commit, each figure
re-measured rather than adjusted: `test/pty/lisp-arena-stats-command.yaml`
(56145 slots, 1086 frames), `lisp-arena-bytes-knob.yaml` (115127 at 2 MiB),
`lisp-exhaustion-mid-init-visible.yaml`,
`lisp-exhaustion-mid-command-recovers.yaml`,
`lisp-exhaustion-mid-hook-reports.yaml`, `src/lisp_core.c`'s two arena
comments (66544 bytes, 56145 / 1086, and the 640 KiB floor's 34026 against
the census's 30048) and `doc/lisp-api.md`'s frame-capacity aside.
`.ci/prelude-startup-census.json` is re-baselined for the two figures above.
Nothing else in kg changes: no editor module, no facade, no `kg -V` word, no
oracle snapshot, no PTY case that is not an arena figure.

### Two findings this phase adds to the ADR's record

1. **fe's static-analysis lane sees payload code for the first time**, which
   23.0's report predicted, and it found two real things rather than noise:
   gcc's `-fanalyzer` takes `OwnedBlock`'s null answer as reachable from
   `FeMark`'s ascent (it is not -- the walk is there only because it
   descended into that owner's block), so the invariant is now an `assert` the
   analyzer can read; and cppcheck asked for `const FeContext*` on every
   payload reader, which is correct C and is now stated once beside
   `PayloadAt`: those functions read the context and never write it, and the
   mutability of what a caller writes travels with the returned pointer.
   Both are fixes, neither is a suppression.
2. **The compaction ordering is proved by breaking it, not by inspection.**
   `FE_PAYLOAD_COMPACT_ORDER_BUG` (0 in every configuration, set by no
   target) runs the compactor after the sweep instead of before it; rebuilt
   with it at 1, `payload_tests` fails at `TestReplacedBlockIsReclaimed`
   (payload_tests.c:275) and exits 2, because the sweep has already cleared
   every mark bit and the liveness rule then reads false for survivors too.
   This reproduces the ADR's own diagnostic-knob method at a different call
   site.

### What 23.2 inherits

Nothing in this section is 23.2's work, and none of 23.2's surface was
anticipated in it.  `FeArenaStats` is unextended, `FeOpenContextWithOptions`
does not exist, `fe_perf.h` gained no payload counter, and
`.ci/prelude-startup-census.json` has no payload fields.  What 23.2 starts
from is: the internal `OpenContextWithPayload(arena, size, percent)` to make
public, seven `FeContext` fields to expose, `PayloadArenaPercent` as the
default to carry, and the table above as the arithmetic kg's own arena will
answer with when it starts passing a carve.
