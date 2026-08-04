# 03F — Two bounds, the API break, and kg

Parent: [Phase 3](../2026-08-03-elisp-subset-and-fe-evaluator.md#7-phase-3--replace-recursive-evaluation-with-an-explicit-frame-machine).
Fe change plus the first kg *source* change in Phase 3, in one atomic pin
commit.

**Prerequisite:** [03E](03e-special-form-frames-and-unwind.md).  The frame
machine must be the only implementation before its bounds are renamed;
otherwise a bound is being defined for a thing that is half-built.

## Outcome

Phase 3's gate is claimed and enforced.  `evaluation_depth` — one counter
that today means "live `Evaluate()` re-entries" — becomes two counters that
mean two different things, each with its own limit, its own default and its
own deterministic error.  The public API says so, the version says so, and
every document that describes the old counter is rewritten in the same
commit rather than left lying.

## The two bounds

The parent plan's restatement of the gate is the specification:

- **Lisp nesting** — nested calls, nested special forms, self-expanding
  macros, deep argument lists — consumes frames on the context-owned stack
  and a *constant* amount of C stack.
- **Native re-entry** still consumes C stack, bounded by its own counter.
  Its limit is a small number, not today's 1000.
- The two bounds are separate, separately configurable, and each raises a
  distinct, deterministic error.

Concretely:

| | Bound | Default | Error |
|---|---|---|---|
| Lisp nesting | frames on the frame stack | derived from the arena, per 03A's Decision | frame-stack exhaustion |
| Native re-entry | nested frame-machine runs started from a native | small — single digits to low tens | native re-entry limit |

Choose the native re-entry default from evidence, not taste: kg's
`internal--save-excursion` and `internal--with-current-buffer` nest as
deeply as user Lisp nests them, so grep kg's own Lisp and PTY corpus for
the deepest actual nesting and leave a comfortable multiple, in the same
spirit as `DefaultEvaluationDepth`'s measured derivation.  Write the
measurement into the constant's comment, as that constant already does.

## The API break, and why it is a break

`FeEvalOptions.max_depth` keeps its name, type and position while its
meaning changes.  That is the worst kind of break: every host compiles, and
behaves differently.

**Rename it.**  Two fields, two names, no silent survivors:

```c
size_t max_frames;         // Lisp nesting, frame-stack slots
size_t max_native_reentry; // nested runs started from a native
```

Every host that set `max_depth` now gets a compile error, which is the
entire point.  Same reasoning for `FeArenaStats.peak_evaluation_depth`,
whose comment currently reads "high-water mark of live `Evaluate()`
recursion (bound: `FeEvalOptions.max_depth`)": it becomes
`peak_frame_depth`, plus a new `peak_native_reentry`.

**`FE_API_VERSION` moves 1 → 2.**  This is exactly the axis it exists for,
and Phase 2 established the discipline of moving only the axis that
actually broke: `FE_LANGUAGE_VERSION` stays at **2**, because no *language*
behaviour changes in Phase 3 — the phase is behaviour-neutral by design.

kg's tripwire already exists.  `src/lisp_core.c:43` reads:

```c
static_assert(FE_API_VERSION == 1);
static_assert(FE_LANGUAGE_VERSION == 2);
```

The first line moves to 2 in the kg commit; the second does not move.  If a
reviewer sees `FE_LANGUAGE_VERSION` change in this phase, something is
wrong.

## kg's ripple

Small, but it is source, so Rule 10 applies with force: **fe lands and
passes in the submodule first; then the pin and every kg adaptation move
together in one green kg commit.**  A pin-only commit here does not
compile — `static_assert(FE_API_VERSION == 1)` fires — which is precisely
the state that assertion was added to make impossible.

| Site | Change |
|---|---|
| `src/lisp_core.c:43` | `FE_API_VERSION == 2` |
| `src/lisp_core.c:497` | `out->peak_evaluation_depth = stats.peak_evaluation_depth` follows the rename |
| `src/lisp.h:40` | `struct kg_lisp_arena_stats.peak_evaluation_depth` — kg's Fe-free copy; rename to match, and add the native-re-entry field or state why not |
| `src/perf.h:132` | `KG_PERF_LISP_PEAK_EVAL_DEPTH` and its comment |
| `src/lisp_core.c:515` | the `KG_PERF_SET` call |
| `src/lisp_process.c:107`, `src/lisp_hooks.c:113`, `src/lisp_core.c:443` | `FeCallWithOptions` callers that populate `FeEvalOptions`; check each for `max_depth` |
| `test/test_lisp.c:2390` | `test_recursion_depth` — see below |
| `utils/bench.py` | the Lisp cases that report the peak-depth counter |
| `doc/lisp-api.md` | the versioned Lisp API reference; `make docs-check` does **not** check it, so it is on the author |

### `test_recursion_depth` is the interesting one

It currently asserts that `(deep 5000)` raises **"evaluation depth limit
exceeded"** and that `(deep 200)` still works afterwards.  After Phase 3,
`(deep 5000)` is Lisp nesting, so it raises the *frame* error instead — and
it may not raise at all, if the frame bound derived from kg's 1 MiB arena
is above 5000 frames.

Both halves have to be settled with the real numbers rather than by
editing the string:

- if the frame bound in kg's configuration is above the depth this test
  uses, raise the test's depth to something that does exhaust it, and say
  in the test's comment what the bound is and where it comes from;
- the "still works afterwards" half is the property that actually matters
  — a bounded, catchable error leaving a reusable context — and it must
  survive unchanged.

This is also the natural place to confirm 03C's requirement empirically:
**the frame bound fires before the arena is exhausted**, so deep recursion
in kg raises a clean evaluator error rather than an allocation failure.

## Documents rewritten in the same commit

`doc/fe-upstream.md`'s divergence table is authoritative and currently
describes the pre-frame-machine world in at least four rows:

| Row | What changes |
|---|---|
| `GcStackSize` 512 → 4096 (line 63) | "The GC stack, not the C stack, bounds recursion … now about 450" stops being true once frames root their own objects; rewrite, including the `FeMinimumArenaSize()` figure, which 03C moved |
| explicit `evaluation_depth` counter (line 73) | the whole entry describes a counter that now counts native re-entry only; rewrite it as the two bounds |
| `FeGetArenaStats()` (line 74) | field renames |
| "kg compiles only `fe/fe.c`" (line 24) | already rewritten by 03B; confirm it still reads correctly |

Plus, in fe: `doc/unwind-design.md` (the parent plan names it explicitly —
it must describe the frame machine's cleanup discipline, not the recursive
one), `doc/implementation.md` (fe's own rule: update it when evaluation,
GC or error-handling invariants change — all three did),
`doc/c-api.md` (the `FeEvalOptions`/`FeArenaStats` change and the version
bump), `fe.h`'s field comments, and the `DefaultEvaluationDepth` comment
block in `fe.c`, which is a long measured justification for a constant that
no longer exists in that form.

`fe/compat/features.json`: Phase 3 is behaviour-neutral, so no
`comparison: emacs` case should change.  Check rather than assume — if one
does, that is a finding and it goes in the manifest as an intentional
change with an oracle case, per the parent plan's regression requirement.

## The gate, claimed here

- **No Lisp-level nesting consumes C stack.**  03A's probe, now an
  assertion rather than a recording: flat across `(deep 10)`,
  `(deep 1000)` and `(deep 100000)`, to within one frame's slop.
  **`(deep 100000)` is an fe-side measurement**, taken with an arena sized
  for it (`./fe -s`), per 03A §4 — kg's 1 MiB arena cannot hold 100 000
  frames and is not expected to.  State both numbers: the fe measurement
  that demonstrates flatness, and the kg frame bound that demonstrates a
  clean error.
- **Native re-entry has its own bound and its own error**, tested at the
  boundary.
- **All supported behaviour preserved**: `make -C fe check` in full, plus
  kg's 69 Lisp PTY cases, `test/test_lisp.c`, and the Phase 0
  compatibility corpus.
- **No sanitizer build can produce a C-stack overflow from Lisp nesting.**
  `.ci/ci-05` (MSan) is the binding lane — it crashed at `(deep 418)`
  before sub-plan 06E's counter existed, and it is the build where fatter
  C frames make the C stack the constraint.
- **The GC-stack and cleanup checkpoints live in frames** and are
  exercised by a GC forced at every resumable frame state (03C + 03E).
- **The old evaluator is gone** (03E) and `doc/unwind-design.md` describes
  the new one.

## Gates

fe: `make -C fe check`, both complexity gates, `format-check`, and the
**full nine-stage `.ci/run-ci-steps.sh`** — this is the end of an fe
workstream, not a slice inside one, and fe's own numbered runners are the
submodule's green light before a pin moves.

kg: `make check` (32 native / 405 PTY, from an idle tree), `make WITH_LISP=0
clean all check` (337 pass + 68 skip), and `.ci/run-ci-steps.sh --parallel`
in full.  Regenerate `compile_commands.json` before believing `ci-06`; that
trap has caught this program twice.

Complexity, both trees, recorded before and after per Rule 6.  This is
where Phase 3's real cost is finally known: the split (03B), the substrate
(03C), the frames (03D, 03E) and the decomposition credit that 03E's
`EvaluatePrimitive` breakup should return.  The set README's price table
row says **+42 measured split tax + 60–100 substance ≈ +100 to +140 scc**;
that estimate was written before 03A established that the pmccabe total is
the honest measure for the core, so the row has to be settled in both
units, with the pmccabe number as the one that means something.

## Closing the phase

Write the Phase 3 Status into the set README, in the shape the first two
sets used: what each slice actually cost against its estimate, what the
plan got wrong, and what is carried forward.  Then remove the seven
sub-plan documents; the README's Status and the commits are the record.

Phase 4 (Lisp-2 namespaces) is next, and its price row assumes the frame
machine exists.

## What this does not do

- **No new language behaviour.**  Phase 3 is behaviour-neutral and this
  slice is the one that gets to prove it.
- **No condition system.**  The throw and quit completion kinds stay
  internal and unreachable; Phase 6 makes them visible.
- **No `FeNativeFn` signature change.**  The parent plan requires it
  preserved, and 03D's boundary capture is what makes preserving it
  possible.
- **No arena-size change in kg.**  If `FeMinimumArenaSize()` growth makes
  1 MiB uncomfortable, that is a finding to report with numbers, not a
  constant to quietly raise.

## Status

Not started.
