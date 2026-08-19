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
