# 03F — Two bounds, the API break, and kg

Parent: [Phase 3](../2026-08-03-elisp-subset-and-fe-evaluator.md#7-phase-3--replace-recursive-evaluation-with-an-explicit-frame-machine).
Fe change plus Phase 3's first kg *API/runtime adaptation*, in one atomic
pin commit.  03B already changed build rules and 03C corrected an
arena-layout comment; neither changed kg's Fe-facing structures or logic.

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

## Files this slice owns

**Fe:** `fe/fe.h`, `fe/fe.c`, `fe/test_api.c`, `fe/test_header.c`,
`fe/example_host.c`, `fe/README.md`, `fe/doc/c-api.md`,
`fe/doc/implementation.md` and `fe/doc/unwind-design.md`.  Audit
`fe/compat/features.json`, but do not edit it unless a successful language
answer genuinely changed.

**kg:** the `fe` gitlink; `src/lisp.h`, `src/lisp_core.c`, `src/perf.h`,
`src/perf.c`; `test/test_lisp.c`, `test/test_perf.c`; `utils/bench.py`;
root `README.md`, `doc/kg.1`, `doc/lisp-api.md` and `doc/fe-upstream.md`.
Run a final repository-wide spelling audit after these named edits.  Do not
bulk-replace unrelated writer, parser, tiny-regex or fuzz fields that also
happen to be named `max_depth`.

## The two bounds

The parent plan's restatement of the gate is the specification:

- **Lisp nesting** — nested calls, nested special forms, self-expanding
  macros, deep argument lists — consumes frames on the context-owned stack
  and a *constant* amount of C stack.
- **Native re-entry** still consumes C stack, bounded by its own counter.
  Its limit is a small number, not today's 1000.
- The two bounds are separate, separately configurable, and each raises a
  distinct, deterministic error.

Concretely, freeze these names and spellings in this slice:

| | Bound | Default | Error |
|---|---|---|---|
| Lisp nesting | simultaneously live ordinary evaluator frames | `frame_capacity`, derived from the arena per 03A's Decision; a nonzero option lowers it | `evaluation frame limit exceeded` |
| Native re-entry | nested evaluator runs entered synchronously from a `FeNativeFn` | measured constant; zero selects it | `native evaluation re-entry limit exceeded` |

`max_frames` counts slots usable by the ordinary computation, excluding any
03A cleanup/error reserve.  A push fails **before** writing when the current
index equals `min(max_frames, frame_capacity)` (with zero `max_frames`
meaning `frame_capacity`).  `frame_capacity` in the statistics is that same
host-usable capacity, not the raw allocation including private reserve.

Native depth is zero in a top-level host call.  Increment it immediately
before a native starts a nested `FeEvaluate*`/`FeCall*` run, permit values
through the configured limit, and fail on the next entry; decrement/restore
it on normal and abnormal return.  Calling a native from Lisp is not by
itself re-entry -- only that native synchronously entering evaluation is.
These definitions belong in `fe.h` comments and `doc/c-api.md`, not only in
the implementation.

As with today's step/depth options, the outermost active evaluation owns
the ambient limits.  A nested `FeCallWithOptions` reached from a native does
not replace them just because it was passed another options pointer; this
must be stated and covered by the native-reentry test, because kg's nested
helpers mix `FeCall` and `FeCallWithOptions`.

Error-path cleanups are the existing exception: the abandoned run's limits
are cleared and every cleanup gets the default frame/native ceilings (and
its separately captured fresh cleanup step budget).  Abandoned evaluator
frames are popped to the barrier, but the native-depth counter retains the
real C activations still live below that barrier and falls only as error
propagation unwinds them.  Document this in the option comments/C API and
keep 03E's tight-body/deeper-cleanup regression green; do not reapply an
already exhausted/tiny body limit, and do not pretend live native C frames
vanished merely because cleanup control is fresh.

Choose the native re-entry default from evidence, not taste: kg's
`internal--save-excursion` and `internal--with-current-buffer` nest as
deeply as user Lisp nests them, so grep kg's own Lisp and PTY corpus for
the deepest actual nesting and leave a comfortable multiple, in the same
spirit as `DefaultEvaluationDepth`'s measured derivation.  The current
corpus visibly composes `with-current-buffer` and `save-excursion`; static
grep is only a lower bound, so add a direct Fe test native and probe the
chosen limit too.  Record the deepest corpus nesting, chosen multiplier and
final constant in this Status and in `DefaultNativeReentry`'s comment.  Do
not ship the draft's vague "single digits to low tens" as a specification.

## The API break, and why it is a break

`FeEvalOptions.max_depth` keeps its name, type and position while its
meaning changes.  That is the worst kind of break: every host compiles, and
behaves differently.

**Rename it.**  Two fields, two names, no silent survivors:

```c
size_t max_frames;         // Lisp nesting, frame-stack slots
size_t max_native_reentry; // nested runs started from a native
```

Both zero values select the defaults above.  Replace the old field; do not
retain a union, macro alias or compatibility accessor for `max_depth`.

Every host that set `max_depth` now gets a compile error, which is the
entire point.  Same reasoning for `FeArenaStats.peak_evaluation_depth`,
whose comment currently reads "high-water mark of live `Evaluate()`
recursion (bound: `FeEvalOptions.max_depth`)": it becomes
`peak_frame_depth`, plus new `frame_capacity` and
`peak_native_reentry` fields.  `peak_frame_depth` counts the actual live
ordinary frames, not the transitional logical pair depth 03C–03E kept for
API continuity; `peak_native_reentry` uses the zero-at-top-level convention
above.  Querying statistics remains allocation-free and read-only.

**`FE_API_VERSION` moves 1 → 2.**  This is exactly the axis it exists for,
and Phase 2 established the discipline of moving only the axis that
actually broke: `FE_LANGUAGE_VERSION` stays at **2**, because no *language*
behaviour changes in Phase 3 — the phase is behaviour-neutral by design.

kg's tripwire already exists near the top of `src/lisp_core.c`:

```c
static_assert(FE_API_VERSION == 1);
static_assert(FE_LANGUAGE_VERSION == 2);
```

The first line moves to 2 in the kg commit; the second does not move.  If a
reviewer sees `FE_LANGUAGE_VERSION` change in this phase, something is
wrong.

Also move `FeVersion` from `"2.0"` to `"3.0"`: this is the next breaking
Fe release even though the language-version axis remains 2.  Update the
compile-time assertions/examples in `fe/test_header.c`,
`fe/example_host.c`, `fe/README.md` and `fe/doc/c-api.md`; do not leave only
kg's assertion proving the new header.

## Fe tests owned by this slice

- Replace `TestEvaluationDepth` with limit-specific cases.  A deliberately
  small `max_frames` succeeds at its last permitted push, fails on the next
  with the exact frame message, runs pending Lisp/native cleanups, and then
  reuses the context.  Repeat default physical exhaustion with
  `max_frames == 0` and assert it happens before object-allocation failure.
- Register a test native that recursively calls `FeCallWithOptions`.
  Exercise the configured `max_native_reentry` at the last permitted value
  and one beyond it, assert the exact native message and original semantic
  trace, and prove both inner and outer run barriers restore their counters.
  Follow with a successful ordinary call on the same context.
- Extend `TestArenaStats`: `frame_capacity` is nonzero and stable across
  read-only queries, each peak starts/advances according to its documented
  convention, and `peak_frame_depth <= frame_capacity`.  Preserve the
  exact-fit/minimum-arena padding test and the definition of object slots.
- Turn `TestEvaluationStackProbe` into the permanent flatness gate at 10,
  1000 and 100000 using the dynamically allocated arena and the numeric
  tolerance already measured under default and sanitizer builds.  Assert
  the result and peak-frame bound as well as callback addresses.
- Keep `TestEvaluationControl`'s exact step accounting, all 03A
  byte-for-byte trace goldens and the primitive-order table from 03E
  unchanged.  The API rename does not license expectation churn there.

Do not add one kg PTY for each resource error.  Fe's API harness owns exact
limit boundaries; kg needs its existing end-to-end recursion/recovery case
and existing native-backed editor features to stay green.

## kg's ripple

Small, but it is source, so Rule 10 applies with force: **fe lands and
passes in the submodule first; then the pin and every kg adaptation move
together in one green kg commit.**  A pin-only commit here does not
compile — `static_assert(FE_API_VERSION == 1)` fires — which is precisely
the state that assertion was added to make impossible.

| Site | Change |
|---|---|
| `src/lisp_core.c`'s version assertions | `FE_API_VERSION == 2`; keep `FE_LANGUAGE_VERSION == 2`. |
| `src/lisp.h`, `src/lisp_core.c` | Mirror `frame_capacity`, `peak_frame_depth` and `peak_native_reentry` in kg's Fe-free stats struct and accessor. |
| `src/perf.h`, `src/perf.c`, `src/lisp_core.c` | Replace `KG_PERF_LISP_PEAK_EVAL_DEPTH`/`lisp_peak_eval_depth` with `KG_PERF_LISP_FRAME_CAPACITY`/`lisp_frame_capacity`, `KG_PERF_LISP_PEAK_FRAME_DEPTH`/`lisp_peak_frame_depth`, and `KG_PERF_LISP_PEAK_NATIVE_REENTRY`/`lisp_peak_native_reentry`; wire all three snapshot assignments. |
| `src/lisp_core.c:eval_options`, plus callers in `src/lisp_process.c`, `src/lisp_hooks.c` and `src/lisp_core.c` | Audit the shared initializer and its `FeCallWithOptions` users.  It currently omits `max_depth`, so both new zero defaults require **no caller edit**; do not churn callers merely because they use the API. |
| `test/test_lisp.c:test_recursion_depth` | Bound/error/recovery adaptation — see below. |
| `test/test_perf.c` | Rewrite the arena-margin/evaluator-shape comments and assertions around the three new fields; retain the result, GC margin and half-free properties. |
| `utils/bench.py` | Rename the JSON keys and re-measure the prelude baseline/shape thresholds; never mechanically replace every old `> 2` with the same frame number.  Each nontrivial case still needs a counter that distinguishes it from prelude-only startup; if frame depth does not, choose a relevant existing/new counter and record why rather than inventing a threshold. |
| root `README.md`, `doc/kg.1`, `doc/lisp-api.md` | Replace the old ~450/`max_depth`/C-stack guidance with the two actual bounds and recorded kg capacity. `make docs-check` does not validate most of this prose, so it remains an explicit review item. |

### `test_recursion_depth` is the interesting one

It currently asserts that `(deep 5000)` raises **"evaluation depth limit
exceeded"** and that `(deep 200)` still works afterwards.  After Phase 3,
`(deep 5000)` is Lisp nesting, so it raises the exact
`evaluation frame limit exceeded` error instead — and it may not raise at
all if kg's derived capacity is above the frames that expression consumes.

Both halves have to be settled with the real numbers rather than by
editing the string:

- expose `frame_capacity` through `kg_lisp_arena_stats()`, measure the
  expression's frames per recursion level, then choose a literal depth that
  is demonstrably above that capacity (the audited candidate is
  `(deep 1000000)`, but keep it only after measuring).  State the measured
  capacity and why the literal exceeds it in the test comment; do not make
  the test depend on a private Fe struct or reverse-engineer frame size;
- the "still works afterwards" half is the property that actually matters
  — a bounded, host-recoverable error leaving a reusable context — and it must
  survive unchanged.

This is also the natural place to confirm 03C's requirement empirically:
**the frame bound fires before the arena is exhausted**, so deep recursion
in kg raises a clean evaluator error rather than an allocation failure.

## Documents rewritten in the same commit

`doc/fe-upstream.md`'s divergence table is authoritative and currently
describes the pre-frame-machine world in at least four rows:

| Row | What changes |
|---|---|
| `GcStackSize` 512 → 4096 | "The GC stack, not the C stack, bounds recursion … now about 450" stops being true once frames root their own objects; rewrite, including the `FeMinimumArenaSize()` figure, which 03C moved |
| explicit `evaluation_depth` counter | the whole entry describes a counter that now counts native re-entry only; rewrite it as the two bounds |
| `FeGetArenaStats()` | field renames |
| "kg compiles only `fe/fe.c`" | already rewritten by 03B; confirm it still reads correctly |

Also rewrite the introductory supported-version paragraphs:
the final tuple is `FeVersion "3.0"`, `FE_API_VERSION 2`, and
`FE_LANGUAGE_VERSION 2`.  Add or finish the divergence row for the frame
machine itself; a reader should not have to reconstruct the central kg-side
Fe divergence from three superseded rows.

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

Use a spelling audit as a checklist, not a bulk-edit command:

```text
rg -n 'max_depth|evaluation_depth|peak_evaluation_depth|DefaultEvaluationDepth|KG_PERF_LISP_PEAK_EVAL_DEPTH|lisp_peak_eval_depth|GC stack.*recurs|C.stack.*recurs' fe src test utils README.md doc
```

Expected survivors must be reviewed one by one: `FeWriteOptions.max_depth`,
writer documentation/examples, `fe/doc/FUZZING.md`'s writer-fuzz option,
tiny-regex recursion fields and historical plan/status text are unrelated.
There should be no survivor referring to evaluator recursion.

## The gate, claimed here

- **No Lisp-level nesting consumes C stack.**  03A's probe, now an
  assertion rather than a recording: flat across `(deep 10)`,
  `(deep 1000)` and `(deep 100000)`, within the numeric tolerance recorded
  by 03A/03E -- not the undefined phrase "one frame's slop".
  **`(deep 100000)` is an fe-side measurement**, taken with an arena sized
  dynamically by `test_api.c`, per 03A §4 — `./fe -s` cannot run the
  test-only `stack-probe`, and kg's 1 MiB arena cannot hold 100 000 frames
  and is not expected to.  State both numbers: the fe measurement
  that demonstrates flatness, and the kg frame bound that demonstrates a
  clean error.
- **Native re-entry has its own bound and exact error**, tested at limit
  and limit+1 with normal return, error, cleanup and context-reuse paths.
- **All supported behaviour preserved**: `make -C fe check` in full, plus
  all kg Lisp PTY cases discovered by the runner, `test/test_lisp.c`, and the Phase 0
  compatibility corpus.  Verify existing oracle snapshots; do not
  regenerate them merely because internals changed.
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

Run focused checks before the full runners: Fe's `test_api`, header/example
host builds, script goldens and `make -C fe compat` (checked-in snapshots,
no Emacs); kg's `test_lisp`, `test_perf`,
`lisp-compat-check`, `header-check` and `docs-check`.  Then run the complete
gates above.  Baseline suite counts are orientation only because 02E/03A
add tests; report what the runner discovers instead of treating 32/405 as
a fixed expected total.

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
sub-plan documents **only after the user/reviewer accepts the completed
workstream**; the README's Status and the commits are the record.

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
- **No new oracle answer and no broad benchmark rewrite.**  Rename and
  re-measure the evaluator counters; keep workloads and successful Lisp
  results stable so before/after evidence remains comparable.

## Status

**Complete, 2026-08-05.**  fe `675bcec` (the blocker), `f1c0dde` (a
use-after-free found on the way) and `60e4c9e` (this document's content) on
`analyzers-etc`; kg pin plus every kg adaptation in one commit, `50439c7`,
per Rule 10.

The full record — what landed, what the plan got wrong, the measured
headroom, and Phase 3's close against its price row — is in the set
README's "Status — Phase 3".  Three things belong here specifically.

**The two bounds shipped as specified**, with one deliberate deviation this
document invited: it asked 03F to "derive the relationship rather than leave
two independent numbers that happen to be close", and the answer was to
delete the second number.  `DefaultEvaluationDepth` and the transitional
logical counter are gone; the physical frame wall is the only bound on Lisp
nesting, so the ~10% margin cannot recur.  `DefaultNativeReentry` is **32**,
derived as this document required: deepest corpus nesting **3**
(`with-current-buffer` wrapping `save-excursion` under a hook or process
callback), 10x margin, ~1 KiB of C stack per level measured under ci-05's
flags.

**Native re-entry is counted correctly for the first time.**  03D's counter
incremented on every native activation.  This document's definition — "calling
a native from Lisp is not by itself re-entry; only that native synchronously
entering evaluation is" — is now implemented structurally rather than by
convention: `RunEvaluation` increments when its own frame stack was already
non-empty at entry, the one condition that can hold only if a native below on
the C stack started this run.  Top-level host calls see an empty stack;
cleanup drains go through `RunEvaluationBody` and are excluded.  No second
counter.

**The permanent flatness gate is stronger than this document asked for.**  It
specified 10/1000/100000 "within the numeric tolerance already measured".  The
measured delta is **0 bytes** at all three depths, with `peak_frame_depth`
300004 at N=100000.  `TestFullDeepFlatness` was folded into it rather than
kept: its N=340 and its entire comment block existed only because of the
`GcStackSize` ceiling that `675bcec` removed.

### Carried forward

- **`(dc 300)` peaks at 904 of kg's 1100 frames — 82% of capacity.**  Usable
  ordinary recursion is ~365 levels (up from 333).  With the logical counter
  gone, frames per level converts directly and solely into user-visible
  recursion depth: one more retained frame per level is a proportional cut
  *and* would put that fixture over the wall.  03E hit exactly this once.
- **A green `make check` plus ci-03/04/05 is not evidence that a GC-rooting
  change is safe.**  All four were green on the tree carrying `675bcec`'s
  use-after-free; only the fuzz lane's 64 KiB arena collects often enough to
  reach it.  Run ci-06 (fe) / ci-09 (kg) before believing a rooting change.
- **`fe/fuzz/seeds/` is new and is the durable half of the fuzz guard.**
  `fuzz/corpus/` is gitignored, so anything it discovers dies at the next
  clean checkout.  A fuzz artifact that turns out to be a genuine bug belongs
  in `seeds/`, named after the defect.

### Not done here, deliberately

The seven sub-plan documents are **not** deleted: this document says that
waits on the reviewer accepting the completed workstream.  The set README's
Status and the commits are the record when they go.
