# Elisp phases 21–26: the data-model wave

## Prompt

"Author a follow-up plan with your recommended next steps for bettering fe
to better serve as an 'elisp-like' engine in kg."  `/opt/nelisp` named as
inspiration, as it was for phases 13–19.

## Provenance

Written 2026-08-18, immediately after the embedded-prelude program closed
(`doc/plans/2026-08-14-embedded-prelude.md`, phases 0–3 plus the
post-prelude collect).  Three sources:

1. **A measured read of fe's object model**, from `fe/fe.h` and
   `fe/fe_internal.h` on the pin this plan is written at
   (`c3044f4`, `FE_API_VERSION` 12, `FE_LANGUAGE_VERSION` 14).  Every
   representation claim below was read out of those two files, not
   inferred.
2. **The forecast audit's own output** (`utils/forecast/AUDIT.md`), which
   phases 13–19's Declined section nominated as the instrument that would
   re-answer the vectors/hash/records question.  It has now answered.
   Section "The instrument answered, and the answer cannot be trusted yet"
   is about what that answer is worth.
3. **A second mining pass over `/opt/nelisp`.**  Phases 13–19 mined it for
   *which elisp semantics matter and in what order*.  This plan mines it
   for one thing more: `docs/02-scoping.org` is `SCOPE-LOCKED` and opens by
   naming scope management as the enemy that killed Remacs.  A plan called
   "make fe a better elisp engine" is unbounded by construction, and this
   one is written to be bounded — every phase past 22 is conditional on a
   measurement, and the plan says plainly what result kills it.

## What is actually there

Measured, not assumed.

**fe's whole type set** (`FeType`, `fe/fe.h:399`): `FeTPair`, `FeTFree`,
`FeTNil`, `FeTDouble`, `FeTInteger`, `FeTSymbol`, `FeTString`, `FeTFn`,
`FeTMacro`, `FeTPrimitive`, `FeTNativeFn`, `FeTPtr`, plus three `FeTFex*`
extension slots.  **There is no vector, no hash table, no record, no
char-table and no bool-vector.**  That is the gap this plan is about, and
it is a gap in the *data model*, not in the function surface — phases
13–20 closed the function surface almost completely.

**The function surface is nearly closed.**  `utils/forecast/AUDIT.md` at
this pin: COVERED 246 names over 2268 references; MISSING **4 names over 4
references**, and all four are the same family —

| Refs | Name |
| ---: | --- |
| 1 | `gethash` |
| 1 | `make-hash-table` |
| 1 | `maphash` |
| 1 | `puthash` |

with the audit's own watch table reporting **vectors 0 references, records
0 references**.

**The arena is a uniform fixed-cell heap.**  `Value` is a union that
`static_assert`s to exactly `sizeof(FeObject*)` (`fe_internal.h:180-194`),
`FeObject` is two of them, and `ctx->objects` is a flat array of
`object_count` such cells.  kg's arena is 1 MiB, which partitions to
`total_slots` **56147**.  There is no variable-length allocation in fe at
all.  The existing precedent for variable-length data is a **chain of
cells**: a string carries `StringBufferSize = sizeof(FeObject*) - 1` = 7
payload bytes per cell (`fe_internal.h:228`), so an N-byte string costs
`ceil(N/7)` slots.

**The collector reverses pointers.**  Sub-plan 09C's mark phase stores its
links in a pair's own `car`/`cdr` words with the collector's flags in the
low bits, which is why `fe_internal.h:298-300` requires bits 0, 1 and 2 of
every object pointer to be free.  Any new object shape has to be markable
under that scheme.

**Headroom, measured at this pin**: post-`kg_lisp_init()` live is 5959 of
56147 slots (10.6%), free 50188, after the post-prelude collect.

## The instrument answered, and the answer cannot be trusted yet

Phases 13–19's Declined section deferred hash tables, vectors and records
with a named condition: *"Phase 15's forecast audit re-answers this with
kg-relevant data; a nonzero verdict is recorded in the audit output either
way, and reopening requires its own phase with that evidence."*

That audit now exists and its verdict is: hash tables 4 references,
vectors 0, records 0.  Read literally, that closes all three — decline and
move on.

**Do not read it literally.**  The corpus is `utils/forecast/` — an
`init.el` and hand-written package sketches — plus kg's own `lisp/*.el`.
Every line of it was written by someone who knew kg has no vectors.  A
demand instrument fed only code written *against the current surface*
cannot measure demand for a surface that does not exist; it will report
zero for every absent feature, forever, and that zero is an artifact of
the corpus rather than a fact about Elisp.

This project has made exactly this mistake once already and paid for it.
The embedded-prelude program's Phase 1 gate asked for names "nothing in
the tree has ever called" while 417 of the 532 corpus items were
`test/lisp-compat`, a conformance suite whose *purpose* is to call every
name; the set was empty before it was measured, and the gate had to be
re-read against the plan's own goal statement before the phase could
proceed.  The lesson recorded there — **beware any gate a corpus gets to
decide** — applies here with the sign flipped: there the corpus made a set
look empty, here it makes demand look absent.

So the first phase of this plan implements nothing.  It fixes the
instrument, and everything after it is conditional on what the fixed
instrument says.

## Ground rules (all phases)

These restate standing project discipline; a phase agent reads this list
as binding.  They are unchanged from `2026-08-10-elisp-phases-13-19.md`
except where noted.

- **fe-first pin discipline.**  A change to fe lands as a commit in `fe/`
  (branch `more-elisp-work`) with fe's own suite green
  (`make -C fe check complexity-check pmccabe-check format-check`, and the
  same for `fe/tiny-regex-c`), then the kg-side commit moves the pin and
  adapts.
- **Version macros.**  A language-behaviour change bumps
  `FE_LANGUAGE_VERSION`; a C-contract change bumps `FE_API_VERSION`; both
  are recorded in `doc/fe-upstream.md` and `src/lisp_core.c`'s
  `static_assert`s move with the pin.  An addition-only change still bumps
  — the compile-time tripwire is the point.  The pin this plan starts from
  is 12/14.
- **Manifests are exhaustive.**  Every new fe primitive gets a
  `fe/compat/features.json` row; every new kg native or prelude definition
  gets a `test/lisp-compat/features.json` row *and* a case file under
  `test/lisp-compat/cases/`.  `make check` fails otherwise; do not
  "temporarily" silence it.
- **Ratchets.**  scc/pmccabe/coverage/gateway/prelude-census ceilings are
  re-set at measured actuals with rationale and before/after proof in the
  commit message, never in a comment beside the knob.  `src` scc is at
  **10642/10642 with zero slack** and the per-file cap is 519.
- **The GC-root rule.**  `MakeObject()` ends in an unconditional
  `FePushGC`, so every allocation roots itself on a fixed 4096-entry stack
  (effective ceiling 4032).  A C loop that allocates a caller-controlled
  number of times must collapse the stack each pass — `FeSaveGC` before,
  `FeRestoreGC` + `FePushGC(accumulator)` each iteration.  This rule is
  here because breaking it shipped a bug in the last program: a
  `native_reverse` that capped every list at ~4030 elements, invisible to
  a suite whose longest list is a few dozen.  **Any phase adding a
  sequence type must include a case above 4032.**
- **A fixture must need nothing fetched.**  The hosted CI box resolves
  internal names only.  This binds Phase 21 hardest: a corpus of real
  third-party Elisp has to be vendored, not downloaded at test time.
- **Docs move with behaviour**: `README.md`, `doc/kg.1`, `src/help.c` and
  `make docs-check` for anything user-visible; `doc/lisp-api.md` for the
  Lisp surface; `doc/TODO.md` rows retired when a phase lands them.
- **Green light** = `.ci/run-ci-steps.sh --parallel` all-PASS.  `make
  check` alone is an iteration signal, not a completion signal.
- **Never push.**  Commits carry the session's standard trailers.

---

## Phase 21 — Make the demand instrument honest

**Implements nothing.**  Its entire deliverable is a number that was not
produced by a corpus written against kg's current surface.

Work:

- Vendor a corpus of **real, unmodified third-party Elisp** under
  `utils/forecast/wild/` — code whose authors had never heard of kg.
  Licence-check every file and record provenance and licence per file;
  prefer GPL-compatible sources kg can legally carry, and prefer breadth
  (many small packages) over depth (one large one), since the question is
  *which features appear at all*, not how often one package uses them.
- Teach `utils/forecast_audit.py` to partition its report by corpus
  origin, so `wild/` demand and hand-written-sketch demand are never
  summed into one number again.  The existing MISSING/COVERED tables stay;
  they gain a column, or a second table.
- Re-run `make forecast-audit`, and record the new MISSING table and the
  new vectors/hash/records watch table in the phase's results.

**Gate:** none — this phase is unconditional and cheap, and it is the only
one in the plan that is.  It fails only if no vendorable corpus exists, in
which case it says so and the whole plan stops, because every phase below
depends on its number.

**What its output means for the rest of the plan.**  State the thresholds
*before* looking, so the number cannot be rationalised afterwards:

- Vectors at **≥ 50 references across ≥ 5 distinct packages** funds Phase
  23.  Below that, vectors are declined and 24–26 fall with them.
- Hash tables at **≥ 20 references across ≥ 3 packages** funds Phase 24
  independently of vectors.
- `cl-defstruct`/records at **≥ 10 references** funds Phase 25, which is
  additionally gated on 23.

These are pre-registered and deliberately not adjustable by the phase that
measures them.

## Phase 22 — The variable-length object question

fe has no variable-length allocation.  Every data type this plan might add
is variable-length.  **This question is prior to all of them and it is
fe's to answer**, exactly as the arena image's pointer-offset question was
in the embedded-prelude plan.  This phase answers it with a spike and
ships no type.

The three candidate representations, with what each costs:

1. **A chain of cells**, as strings already are (7 payload bytes per cell).
   Cheapest by far — the sweeper, the marker and the pointer-reversal
   scheme all already handle chains, and `FE_API_VERSION` need not move
   for the representation itself.  Costs O(n/k) indexing, which makes
   `aref` linear.  For a keymap, a `cl-defstruct` instance or a small
   sequence that is fine; for a 10k-element vector it is not.
2. **A multi-cell extent**: one header cell plus ⌈n/2⌉ contiguous payload
   cells taken from the arena.  O(1) indexing.  Costs the sweeper an
   understanding of extents — the free list is currently a singly-linked
   chain of individual cells, and a contiguous run cannot be carved from
   it without compaction or a size-class allocator.  This is the change
   that could destabilise the collector, and it is where the real risk in
   this whole plan sits.
3. **An out-of-arena handle**: an `FeTPtr`-shaped cell holding a
   `malloc`'d block, freed by a finalizer at sweep.  O(1) indexing and no
   collector change to the arena, at the price of giving fe its first
   finalizer and its first allocation the fixed arena does not bound —
   which is a real loss, because "the arena is the whole budget" is a
   property kg's exhaustion handling and its four PTY exhaustion cases
   currently rely on.

Work: spike all three far enough to measure, on a fixed benchmark
(construct, index, iterate, collect) at n = 8, 256 and 8192; report slots
consumed, indexing cost, collector diff size, and whether the
pointer-reversal invariant survives.

**Gate:** a Decision, recorded in `doc/fe-upstream.md` with its
measurements, naming one representation and saying why the other two lost.
If option 2 cannot be made to work without compaction, say so — that is a
finding, and it pushes the answer to 1 or 3 rather than pushing the
collector.

**A phase that ends "chain of cells, and `aref` is linear" is a success**,
not a failure.  It bounds every phase after it, which is the point.

## Phase 23 — Vectors

Conditional on 21's threshold and 22's Decision.

Work: `[...]` reader syntax and printing; `vector`, `make-vector`,
`vconcat`, `aref`, `aset`, `vectorp`, and `length`/`elt` extended to
vectors.  Reader syntax is the half that is easy to forget and the half
packages actually use — a `[...]` literal in a keymap or a `cl-defstruct`
default is far more common than an explicit `make-vector` call.

`FE_LANGUAGE_VERSION` moves.  Manifest rows in both compat manifests.  A
case above 4032 elements, per the ground rules.

**Gate:** the audit's vectors row from Phase 21, and a slot cost per
element from Phase 22 that the 56147-slot arena can carry — a
representation costing more than one slot per two elements makes a
1000-element vector 1% of the arena, which needs saying out loud before it
is shipped rather than after.

## Phase 24 — Hash tables

Conditional on 21's threshold; independent of 23 unless 22's Decision
makes a hash table a vector underneath.

Work: `make-hash-table` (`:test` restricted to `eq`/`eql`/`equal` — a
predicate table is a recorded divergence, as phases 13–19 already framed
it), `gethash`, `puthash`, `remhash`, `maphash`, `hash-table-p`,
`hash-table-count`, `clrhash`.  Printing and reading of `#s(hash-table
...)` is explicitly **out of scope** and a recorded divergence; nothing in
kg needs to serialise one.

Note for whoever takes this: fe already has an obarray-shaped problem
solved internally (`symbol_list`), and a hash table is the same structure
with a user-visible face.  Look there before inventing storage.

**Gate:** the audit's hash-table row from Phase 21.  Today's figure is 4
references from a single sketch file, which is *below* the pre-registered
threshold — so on current evidence this phase does not run.

## Phase 25 — Records, and the `cl-defstruct` question

Conditional on 21's threshold and on 23 having shipped.

Work: `record`, `make-record`, `recordp`, `type-of` extended, and enough
of `cl-defstruct` to define and use a struct.  The honest scoping question
is whether kg wants `cl-lib` at all; a `cl-defstruct` that works but whose
`cl-` neighbours do not is a trap for a package author, and the phase
should decide that explicitly rather than discovering it.

**Gate:** 21's records row, and a written answer to "does this commit kg
to `cl-lib`?" before any code.

## Phase 26 — Sequence generalisation

Conditional on 23.  The phase that makes 23–25 worth having.

Work: `elt`, `length`, `mapcar`, `mapc`, `mapconcat`, `append`, `copy-
sequence` and the rest of the sequence surface extended over strings and
vectors, not just lists.  Today several of these are list-only prelude
Lisp, which is invisible until a package passes a vector and gets a
`wrong-type-argument` from three frames down.

**Gate:** each generalised name carries a compat case proving it against
the Emacs oracle for all three sequence types.  A name that cannot be
generalised without slowing the list path is left alone and recorded.

---

## Declined and watch items

- **Bytecode.**  Declined 2026-08-07 with counters; the five triggers and
  the "fund instrumentation first" rule stand.  Nothing here reopens it,
  and nothing here should: `/opt/nelisp` has a bytecode interpreter *and*
  a JIT, and it is a self-hosting VM project, which kg is not.
- **The arena image / pre-parsed embedding.**  Declined 2026-08-18 with
  measurements; see the embedded-prelude plan's Phase 3 results.  Its
  finding is load-bearing here too: fe objects are pointer-linked with
  mark bits in the low bits of `car`/`cdr`, so anything that wants to
  serialise or relocate them inherits that problem.
- **Char-tables and bool-vectors.**  Not planned.  They are Emacs'
  answer to per-character data at Emacs' scale; kg's syntax and display
  layers are C and do not want them.  Reopening needs a demand number from
  Phase 21's corpus like everything else.
- **Text properties and overlays.**  Genuinely editor-relevant and
  deliberately *not* in this plan, because they are a buffer-model
  question rather than a data-model one and would double its scope.  A
  separate plan, after this one, if Phase 21's corpus shows packages
  reaching for them.
- **`eval`'s LEXICAL argument, macro environments.**  Still need
  first-class lexical environments; recorded, not planned.
- **Growing the arena, or making it growable.**  Measured useless in a
  different context (a 256 KiB arena runs the GC stress in 75.2 s against
  142.7 s, only 1.9×, because marking the live set costs what sweeping the
  rest does).  If Phase 22 picks representation 3 this needs revisiting,
  which is one more reason 22 comes before 23.

## Sequencing rationale

Instrument before decision, and representation before type.

Phase 21 is first because every threshold in this plan is a number it
produces, and because the number the current corpus produces is an
artifact of who wrote that corpus.  It is also the only phase that is
unconditionally worth doing: even if every threshold below fails, kg ends
up with an honest, re-runnable measure of how far its Lisp is from the
Elisp packages actually get written in — which is the question behind the
prompt, whatever the answer turns out to be.

Phase 22 is second because fe cannot allocate a variable-length object at
all, and all four types below are variable-length.  Deciding vectors
before deciding how a vector is *shaped* would repeat the embedded-prelude
plan's own Phase 3 error, where a variant was funded on "no fe change"
that turned out to need the hardest fe change in the document.  22's
cheapest outcome — chains of cells, linear `aref` — is a real answer that
bounds everything after it.

Then vectors (23) before records (25), because a record is a vector with a
type tag under every implementation worth having; hash tables (24) beside
them rather than after, because they need nothing from vectors if 22 says
so; and sequence generalisation (26) last, because it is the phase that
turns three new types into something a package author can actually use,
and it is worthless before they exist.

On current evidence, **the honest expected outcome of this plan is that
Phase 21 runs, Phase 22 runs, and one or none of 23–26 clears its
threshold.**  That is written here deliberately.  The last two programs
each produced more value from a measured decline than from the code they
shipped, and a plan whose phases are all expected to land is a plan whose
gates are not doing any work.
