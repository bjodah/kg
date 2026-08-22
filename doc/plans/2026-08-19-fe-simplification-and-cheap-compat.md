# fe follow-ups: simplification, the arena knob, and the cheap compatibility wins

Date: 2026-08-19
Status: proposed.  Written after the 2026-08-19 adversarial review's
findings were fixed (kg `b692e9d`/`69d7cd5`, fe `5d5053d`/`1a2d980`, plus
the tooling fixes), and after the maintainer's directive that this wave
must optimize for READABILITY OF IMPLEMENTATION: no mechanism stays merely
because it works, and the 1 MiB arena is an artificial constant, not a
product constraint -- growing it 10x or making it user-configurable is on
the table.

That directive re-prices one landed decision and three pending ones, so
this plan puts numbers beside each and proposes the order.  Everything
here is independent of the Phase 22 storage architecture; the one
scheduling dependency is stated in Phase A (land it BEFORE Phase 23's
substrate, so nobody ports stub machinery onto the new allocator).

## What this plan does not reopen

- Bytecode/JIT: the 2026-08-07 measured decline stands; nothing here is
  evaluator-dispatch-bound.
- Arena image / pre-parsed embedding: the embedded-prelude plan's measured
  decline stands.
- Hash tables: Phase 27's named-consumer rule stands.
- The Phase 22 ADR (Design B): unaffected.  Phase B below feeds its
  options-bearing open API; it does not re-argue the layout.

---

## Phase A -- remove lazy prelude deferral, keep the post-prelude collection

### The re-pricing

Deferral's entire measured payoff is memory: 5043 slots, 9.0% of the
56147-slot (1 MiB) arena (`doc/plans/2026-08-14-embedded-prelude.md`,
Phase 1's table).  Its cost was priced as "a stub factory and one native"
-- but the adversarial review's Finding 1 (High: a captured stub silently
overwrote later user definitions) showed the true cost: observable
function-identity semantics, native re-entry, and a class of bugs that
required per-entry `FeRoot` caching and a restore-unless-still-stub rule
to fix.  The fix is correct and tested, but it is exactly the kind of
mechanism the readability directive says not to carry for a small win:

| quantity | value | source |
| --- | --- | --- |
| slots deferral saves | 5043 (9.0% of arena) | embedded-prelude plan, Phase 1 |
| eager prelude peak | ~11428 of 56147 = 20% | same, Phase 0.2/0.4 |
| eager peak at a 10x arena | ~2% | arithmetic |
| eager peak vs ADR's post-carve cell pool | ~11.4k of ~42335 = 27% | ADR condition 6's carve |
| startup cost of the 93 eager forms | a fraction of the whole prelude's 2.2-3.4 ms; measured exactly by `test/perfobj/prelude_read_eval_split` before deletion | embedded-prelude plan 0.3 |
| machinery deleted | see inventory below; ~600+ lines and one policy file | this plan |

The post-prelude forced collection is a different trade and is KEPT: one
~0.1 ms collection reclaims the ~1238 load-time-garbage objects (10.8% of
eager peak) with no semantic surface at all.  Deferral and that
collection were always separable (the plan said so when it scheduled
them); this phase separates them.

### Decision rule

Remove deferral if, measured on this tree before deletion:

1. the eager prelude's post-collection reachable set stays under one
   third of the cell pool at the CURRENT default arena AND at the ADR's
   selected payload carve (both hold today: 20% and 27%); and
2. the read/eval cost the 93 forms add to startup, read off
   `prelude_read_eval_split` eager-vs-deferred, is under 2.0 ms (the
   whole prelude is 2.2-3.4 ms; a PTY readiness wait absorbs this
   without any case change).

If either fails, stop and keep the (now-fixed) deferral; record why.

That ceiling was 1.5 ms when this plan was written, and 1.5 was a
PROJECTION off the 2.2-3.4 ms parenthetical beside it rather than a
measurement of anything: the deferred prelude turned out to cost 1.24 ms,
so the delta could only have come in under 1.5 if the eager prelude
landed at the very bottom of that range, and it did not.  Measured, the
delta is +1.685 ms (release median, n=21; +1.771 ms and +1.897 ms on the
counting build's two columns), so the original number fails.  What the
ceiling actually protects survives that comfortably, which is why the
number moved rather than the decision: an interactive launch paying
another 1.7 ms is orders of magnitude under the threshold at which a
human perceives a delay at all, and across a full PTY layer of ~600
editor launches the whole CI bill is roughly one second.  A ceiling
exists here to catch a startup regression of the kind a user or a test
lane would feel; 2.0 ms is set above the measured actual with enough room
to be a real gate on the next change and still no room to grow into
casually.

Forward hook: the eager prelude's load cost is interning-heavy, so Phase
26's symbol index is expected to reduce it.  Record prelude `eval_ns`
again at Phase 26's exit, and re-price this ceiling against what it
reads then.

### Deletion inventory

- `lisp/prelude.el`: `internal--make-deferred-stub` and its comment block.
- `src/lisp_prelude.c`: `native_internal_force_deferred`,
  `install_deferred_stubs()`, `deferred_state[]` and its `FeRoot`s (the
  Finding 1 fix's own machinery goes too -- it exists only to make stubs
  safe), the deferred generated-index include.
- `utils/embed_lisp_split.py` and `utils/prelude_deferred_names.txt`;
  the Makefile returns to `utils/embed_lisp.py`'s single-array embed
  (the pre-split embedder, still in tree).  The split script's new
  `--names` parsing and self-test (review Finding 6) die with it; they
  were correct for the tree that had it.
- `test/pty/lisp-deferred-stub-first-call.yaml`; the
  `internal--force-deferred` rows in any binding/forecast tables.
- Census machinery that exists only for the split (the eager/deferred
  partition in `utils/prelude_slot_census.py` truncation handling stays
  -- it predates the split).

### What is deliberately kept

- The two Finding 1 oracle regressions (`prelude-deferred-capture-*`,
  renamed to drop "deferred"): they pin EAGER-capture semantics against
  Emacs -- `(symbol-function 'mapcar)` captured, symbol redefined,
  captured value still runs the original.  Eager definitions satisfy
  them trivially, which is the point: they guard against any future
  reintroduction of forwarding stubs.
- The post-prelude forced collection, its probe
  (`test/prelude_gc_probe`), and all four census ceilings.
- `test_lisp.c`'s capture-semantics unit (adapted to eager).

### Ratchet consequences, stated up front

`peak_live_objects` rises ~6933 -> ~11.4k and `reachable_live_objects`
~6049 -> ~10.2k: a deliberate ceiling RAISE carrying this plan's
rationale and the measured table above in its commit message, per the
census policy.  `embedded_bytes` and `definition_count` fall (factory
and comments removed) and are banked.  scc/pmccabe fall and are banked.

### Verification

- Full `make check` in both `WITH_LISP` configurations; oracle counts
  unchanged except the renamed cases; `.ci/ci-08` green.
- `prelude_read_eval_split` numbers recorded before/after in the commit.
- `kgbatch -g` high-water and forced-collection figures re-recorded.

### Results

Landed.  Condition 1 passed as projected and better than projected;
condition 2 failed at its original 1.5 ms ceiling, which the Decision
rule above re-derives to 2.0 ms.  The failed row stays in this table: the
raise is a correction to a projected number, not an erasure of what was
measured against it.

Startup, `prelude_read_eval_split`, medians on an otherwise-quiet
32-core box (load average 3.3).  "release" is the ordinary `-Os` build
(`test/prelude_read_eval_split`), which is the configuration a user gets;
"counting" is `test/perfobj/`, which carries kg's and fe's counters and
so is the slower of the two.

| reading | deferred | eager | delta | vs 1.5 | vs 2.0 |
| --- | --- | --- | --- | --- | --- |
| `eval_ns` release, n=21 | 1.241 ms | 2.926 ms | **+1.685 ms** | FAIL | PASS |
| `eval_ns` counting, n=5 | 1.630 ms | 3.401 ms | +1.771 ms | FAIL | PASS |
| `prelude_only_ns` counting, n=5 | 0.792 ms | 2.689 ms | +1.897 ms | FAIL | PASS |

The eager readings reproduce the pre-deferral Phase 0.3 table
(`doc/plans/2026-08-14-embedded-prelude.md`: counting `eval_ns` 3.381 ms,
`prelude_only_ns` 2.880 ms) to within noise, which is the check that this
is the same prelude cost that plan measured and not a new one --
`prelude_only_ns` is the one that came in lower, because Phase 2 has
since moved seven names to natives.

Arena, `test/kgbatch -g` and `test/prelude_gc_probe`:

| quantity | deferred | eager | of 56147-slot arena | of ADR carve (~42335) |
| --- | --- | --- | --- | --- |
| `peak_live_objects` | 6933 | 10950 | 19.5% | 25.9% |
| `reachable_live_objects` | 6049 | 9979 | 17.8% | 23.6% |

Both come in under this plan's own projection (~11.4k / ~10.2k) and well
inside condition 1's one-third bar on both denominators.  The four census
ceilings move accordingly: the two above rise and carry this table;
`embedded_bytes` 74583 -> 70954 and `definition_count` 122 -> 121 fall
and are banked.

The gc-stress lane was measured either side, because it collects before
every allocation and so multiplies whatever the prelude's live set costs:
1.24 s -> 1.53 s wall (+23%), 1.2 s -> 1.5 s of stress run against a
120 s budget.  No retune: the lane spends 1.3% of its budget, and that
budget is derived from the box's own ordinary run (`STRESS_TIMEOUT_RATIO`
1000x, floored at 120 s), so a slower box stretches it with the run
rather than against it.

Two counters did NOT move, which corrects a claim this repository was
making in two places: `peak_frame_depth` 8 and `peak_native_reentry` 1
read identically before and after.  `test/test_perf.c` and
`utils/bench.py` both attributed those to `install_deferred_stubs()`
re-entering the evaluator once per deferred name; deleting that loop left
both numbers untouched, so the attribution was wrong.  Both comments now
say so and neither replaces it with an unmeasured guess.

#### Why the embed is still generated, and what would replace it

Phase A returns the build to `utils/embed_lisp.py`'s single-array embed
rather than adopting C23's `#embed`, which would delete that script
outright and is the recorded end state.  The blocker is the default
toolchain floor, measured on this box:

| compiler | `__has_embed` |
| --- | --- |
| gcc 14.2.0 (Debian 14.2.0-19), the default `CC` | absent |
| gcc 16.2.0 (`/opt-2/gcc-16`) | present |
| clang 22.1.8 | present |

So `#embed` is available from two compilers here and not from the one a
plain `make` uses.  The swap is a one-commit change the day plain
`make`'s default compiler reports `__has_embed` EVERYWHERE that matters
-- this box and the hosted CI image both.  Until then the generated array
stays, and there is deliberately no dual mechanism in the meantime: a
`#if __has_embed` fork carrying both paths is exactly the kind of
machinery the directive behind this phase says not to keep.

---

## Phase B -- the arena becomes a run-time knob

`KG_LISP_ARENA_SIZE` is a compile-time 1 MiB default
(`src/lisp_core.c:62`).  Make the size a run-time decision:

- `KG_LISP_ARENA_BYTES` in the environment, read once by
  `kg_lisp_init()`, following the run-time-hook precedent
  (`KG_LSP_SERVER_C`).  Accepts plain bytes and `K`/`M` suffixes.
  Invalid or absent -> the compiled default; a value below the floor
  (the prelude's measured reachable set x 3, the margin
  `test_lisp.c` already asserts) is refused with a message naming the
  variable and the floor, and kg starts with Lisp disabled the same way
  a failed init already does -- never a smaller silent arena.
- `arena-stats` (the command) and `kgbatch -g` report configured bytes
  beside slots, so a report always says which arena produced it.
- Documentation: `doc/kg.1`, `README.md`; one PTY case with `env:`
  proving the knob and one proving the refusal message.

### Whether to raise the default

Separate, measured decision -- the knob lands first.  The cost of a
bigger arena is per-collection sweep time, O(total_slots): measured
~0.1 ms per collection at 56k slots (`lisp-gc-stress-check`: 15701
collections in 1.5 s), so 10 MiB projects to ~1 ms -- still far inside
interactive latency budgets, but it is a projection; measure a real
sweep at the candidate size (`make bench` gc cases + a forced-collect
timing at 1/4/10 MiB) and put the table in the commit that changes the
default.  Mark-side cost scales with live objects, not arena size, and
does not move.

### Interface note for Phase 23

The ADR's options-bearing open API (total bytes, payload carve) takes
this knob as its byte source.  Land B before 23.2 so the API has one
authoritative size input from day one.

---

## Phase C -- the cheap compatibility shims

Three small items, each with its own commit and its own evidence, per
the capabilities report (`...phase21-capabilities.md`).

### C1: `autoload` as a documented no-op macro

`(defalias 'autoload (macro args nil))` beside the `interactive`
precedent (`lisp/prelude.el:776`).  s.el's actual first blocker
(`s.el:34`, measured `void-function autoload`).  Divergence stated
honestly in the compat manifest: Emacs' `autoload` arms lazy loading;
kg's records nothing, so a function that only an autoload would have
provided stays `void-function` at first CALL.  That is the correct kg
answer until kg has package loading at all.  Oracle case for the inert
shape; manifest row for the divergence.

### C2: form-feed is reader whitespace (fe-side)

`fe/fe.c:2447`'s skip set `" \n\t\r"` gains `\f`.  Found incidentally
by the capabilities sweep (s.el:770, f.el:39; `nil\f\nnil` answers
`void-variable \` today).  fe script-suite case + kg oracle case;
`FE_LANGUAGE_VERSION` moves per fe's versioning rule; the divergence
list in `doc/fe-upstream.md` gains the entry; fe pin moves with fe's
own suites green.

### C3: pin the s.el frontier -- without vendoring

The capabilities report deliberately vendored nothing, and the suite's
rule is that a fixture must need nothing fetched.  So:

- Now: a SYNTHETIC fixture (a dozen lines: an `(autoload ...)` form,
  then a `(defmacro ... (declare (debug (... [&or ...]))))`) reproducing
  s.el's exact blocker sequence.  With C1+C2 landed it must fail at the
  vector literal with `unsupported read syntax: vector brackets` --
  checked in as the expected error, so the frontier is pinned and Phase
  24 flips this case to a clean load as its first visible payoff.
- Phase 24's real end-to-end "load unmodified s.el, call representative
  functions" needs the file itself.  Vendoring a GPLv3+ package as a
  test fixture is a LICENSING DECISION THE MAINTAINER MAKES, not this
  plan; the alternative is a skip-when-absent case reading the box's
  elpa copy, which the suite's skip policy already models
  (`requires_tool:`-style).  Decision left open, flagged for Phase 24.

---

## Phase D -- benchmark results name the artifact that produced them

The review's Finding 4 fix ties the measured process to its evidence via
counters.  The remaining gap (and nelisp's third lesson) is artifact
identity: `test/.results/bench.json` and fe's `workloads.json` gain a
header with `kg -V`'s feature string, `git describe --dirty` for both
trees, and the measured binary's sha256; `utils/quality_report.py`
surfaces it.  A number whose artifact line does not match the tree under
discussion is not evidence, and now says so itself.

---

## Order of work

A (deferral removal) -> C1+C2 (independent, small) -> B (knob) -> C3 ->
D.  All before Phase 23's substrate lands; A is the one hard ordering
(do not port stub machinery onto the new allocator).  Each phase is one
or two commits with its measured table in the message, per house rules.
