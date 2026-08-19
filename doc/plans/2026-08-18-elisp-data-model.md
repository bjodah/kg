# Elisp phases 21+: an engine-substrate wave

Status: **REVISED 2026-08-19 after architecture review.**  This replaces the
original phases 21–26 proposal added in `adabf89`.  Git retains that proposal
as the record of what was reviewed.

## Executive decision

Do **not** execute the original plan as written.

It found the right frontier: fe's fixed-cell representation is now the
constraint behind vectors, length-bearing strings, hash tables and records,
and representation work must precede adding those public types.  It also got
several important details right:

- reader syntax is part of a type's implementation, not a later nicety;
- the fixed arena and the pointer-reversing collector are properties to
  preserve deliberately, not obstacles to route around accidentally;
- vectors should precede records;
- bytecode and text properties are separate questions; and
- a phase may legitimately end in a measured decline.

The proposed execution order and gates do not follow from those observations,
however:

1. **A raw third-party reference count is not a product decision.**  One
   vector literal prevents a whole file from being read, while fifty calls to
   an optional helper may block nothing.  Counts also depend more on which
   packages were selected than on what kg users want to do.  Pre-registering
   50/20/10 as thresholds prevents post-hoc rationalisation, but does not make
   the thresholds meaningful.
2. **The representation menu is incomplete.**  Variable payload can remain
   inside the caller's arena without being either a linked list, a contiguous
   run in the object free list, or a `malloc` allocation.  A stable
   `FeObject` header plus a separately managed, compactable payload region is
   the most important candidate for fe and was absent.
3. **The proposed chain is not a performant vector.**  It costs at least one
   16-byte cell per element and makes `aref`/`aset` linear.  It is a useful
   control in a spike, not an acceptable expected outcome for a plan whose
   goal includes performance.
4. **The plan separates a type from the operations that make it usable.**
   Shipping vectors in Phase 23 and only generalising sequence functions in
   Phase 26 creates a deliberately broken interval.  Reader, writer, access,
   mutation, equality and the core sequence surface belong in one vertical
   slice.
5. **It skips the data-model costs fe already pays.**  Strings are
   NUL-terminated seven-byte cell chains; symbol interning scans every
   interned symbol and compares those chains; lexical environments are
   alists; every integer result is a fresh 16-byte object.  Adding a vector
   without measuring those paths is not a performance plan.
6. **`symbol_list` is not an obarray-shaped problem already solved.**
   `FindInternedSymbol()` linearly walks `ctx->symbol_list`.  It is a correct
   permanent root and enumeration list, but it is the absence of an index,
   not a reusable hash-table implementation.
7. **“The function surface is nearly closed” is true only of the current
   forecast corpus.**  The checked-in audit reports four missing names for
   exactly that corpus.  fe's compatibility manifest still records unsupported
   reader/type families and dozens of deliberate semantic divergences.  The
   audit is useful; the broader claim is not.
8. **The original Phase 22 runs even if Phase 21 funds no type.**  In that
   outcome all three prototypes are throwaway work with no consumer.

The better sequence is therefore:

1. measure engine costs and name the compatibility milestones;
2. decide the storage/value architecture against those costs;
3. land the private storage substrate with no half-implemented Lisp type;
4. add vectors together with their sequence contract;
5. use the same substrate to repair strings;
6. index symbols and extract a real hash substrate; and
7. add user hash tables, records, or lexical-environment work according to
   named blockers rather than token counts.

The expected architecture is **stable `FeObject *` handles plus a bounded
payload region inside the existing arena**.  A tagged-value rewrite remains a
real candidate, not a rhetorical one, but it has to beat the evolutionary
design on kg's measured workloads by enough to pay for an API-wide migration.
nelisp's 8-byte tagged-word work is evidence that per-value boxing can matter;
its measured crisis was a 1.80 GB vendor load with 72-byte cons boxes.  fe
already has 16-byte conses and a 1 MiB budget.  The lesson to import is
“measure the representation”, not “copy the representation”.

### What this review takes from nelisp

The reference is evidence, not a target architecture:

- `/opt/nelisp/docs/02-scoping.org` supplies the useful scope rule: define a
  concrete completion condition and keep attractive later layers out.
- `/opt/nelisp/docs/design/146-low-memory-value-rep-and-gc.org` records why
  nelisp eventually funded tagged values and moving collection: per-value
  boxing was measured as the dominant cost at its scale.
- `/opt/nelisp/docs/design/147-box-layout-container-shrink.org` records the
  corrective result that shrinking vector/record slots did not move its
  vendor-load peak, while shrinking conses did.  “Vectors are a foundational
  type” and “vectors are the memory bottleneck” are different claims.

That last result is a direct reason for Phase 21: even a sensible
representation optimisation can be irrelevant to the workload that motivated
it.

## Goal and boundaries

The goal is not “run arbitrary ELPA”.  It is:

> Make fe a predictable, extensible and efficient Elisp substrate for kg,
> able to add ordinary aggregate types without abandoning its bounded-memory
> contract, while improving the hot paths kg actually exercises.

The following remain the default contract unless Phase 22 proves that one
must change:

- one caller-owned arena is fe's complete memory budget;
- core fe performs no `malloc` and gains no runtime dependency;
- a failure to allocate is a structured, recoverable arena exhaustion;
- host-visible `FeObject *` values remain stable for their lifetime;
- the host roots persistent values explicitly and pointer extension objects
  remain traceable through the mark callback;
- collection uses no C stack proportional to the object graph; and
- kg remains the primary downstream.  Generic embeddability matters, but kg
  does not pay for an abstract VM project it does not need.

Explicit non-goals for this wave:

- full Emacs compatibility, ELPA percentage targets, or `cl-lib` as a whole;
- bytecode, JIT, native compilation or an arena image;
- text properties, overlays, char-tables and bool-vectors;
- moving host-visible objects;
- a growable or host-heap-backed default arena; and
- replacing kg's C buffer/window/display model with Lisp objects.

## Baseline at the plan's pin

The facts below are the starting constraints, not estimates.

- Superproject `adabf89` pins fe `c3044f4`,
  `FE_API_VERSION == 12` and `FE_LANGUAGE_VERSION == 14`.
- kg's 1 MiB arena partitions into **56147 object cells**.  On this tree,
  `./test/kgbatch -a /dev/null` reports **50188 free** after
  `kg_lisp_init()`, hence **5959 reachable live**, with a startup high-water
  mark of **6819** and the one deliberate post-prelude collection.
- On a 64-bit build, one `FeObject` is 16 bytes: two pointer-sized `Value`
  words.  Every integer and float is boxed in one such object.
- A string is a chain of `FeTString` cells carrying seven bytes each.  It has
  no stored length, cannot contain NUL, and indexing/copying walks the chain.
- An interned symbol owns several ordinary cells and is itself kept forever
  through `symbol_list`.  Every interning hit or miss linearly scans that
  list, comparing name chains.
- A lexical environment is an alist.  Lookup and parameter binding allocate
  and traverse ordinary pairs.
- The collector sweeps all 56147 cells and uses pointer reversal for pairs
  and one-child objects.  `FeTPtr`/`FeTFex*` children are reported by the
  host mark callback.
- `MakeObject()` pushes every new object onto a fixed GC-root stack.  Any
  caller-controlled allocation loop must restore its checkpoint as it goes;
  tests above 4032 elements remain mandatory.
- The current forecast is an exact report about kg's shipped Lisp and three
  hand-written target sketches.  It is not a representative sample of Elisp
  packages in the wild.

These figures must be re-recorded if work starts at a different pin.

## Evidence policy

Two questions require different evidence and must not be collapsed into one
score.

### Capability evidence

A compatibility target is a **named use case or package**, with:

- why kg wants it;
- its exact version, source and licence;
- the first unsupported reader form, type, special form and function family;
- whether the blocker is on an unconditional load path; and
- a small end-to-end acceptance case that will become green.

A single unconditional `[...]` is sufficient evidence for vectors because it
prevents parsing.  Repeating it fifty times does not make the package fifty
times more valuable.  Conversely, a thousand references in code kg does not
intend to run fund nothing.

The forecast audit remains a cheap discovery tool and drift gate for kg's own
target corpus.  A broader corpus may be mined manually to produce a shortlist,
but this plan does **not** require vendoring arbitrary packages into
`utils/forecast/wild/` or turn their aggregate reference counts into CI
policy.  Vendor a package only after it becomes a named target and its licence
and fixture cost are accepted.

### Performance evidence

Counters decide algorithmic shape; repeated wall-clock measurements decide
whether a shape matters to users.  A phase does not argue from elapsed time
alone, and it does not optimise a counter merely because the counter exists.

Every performance claim records:

- commit and fe pin;
- compiler and flags;
- arena layout;
- exact workload and result;
- allocation/live/collection counters;
- median and dispersion for wall time where relevant; and
- the same measurement before and after.

Wall time is a report, not a CI gate.  Deterministic shape assertions are the
gate.

## Ground rules for every implementation phase

- **fe first.**  A fe change lands and passes fe's own
  `check`, `complexity-check`, `pmccabe-check` and `format-check` before kg
  moves the pin and adapts.
- **Version tripwires move deliberately.**  Language behaviour moves
  `FE_LANGUAGE_VERSION`; a C contract or representation-facing API moves
  `FE_API_VERSION`.  `doc/fe-upstream.md` records both.
- **Manifests stay exhaustive.**  Freeze oracle cases before implementation,
  then add every new primitive/native/prelude definition to its owning
  manifest.  A recorded divergence that starts agreeing is fixed, not ignored.
- **The fixed arena is tested small as well as large.**  New allocation
  machinery gets exhaustion and recovery tests in arenas that collect often,
  not only kg's roomy 1 MiB configuration.
- **GC stress is mandatory.**  Aggregate tests cover cycles, self-reference,
  host-pointer edges, mutation immediately before collection, collection
  during construction, and more than 4032 elements.
- **No unpriced split.**  Cell capacity, frame capacity and payload bytes are
  reported separately.  A design must not make one pool look healthy by
  silently starving another.
- **No partial public type.**  A private allocator may land alone.  A public
  type lands with reader/writer behaviour, predicates, equality, mutation,
  C accessors, error contracts and the core operations packages will use.
- **Docs and ratchets move with the code.**  Representation changes update
  fe's `doc/implementation.md` and `doc/c-api.md`.  User-visible language
  changes update both language/API documentation and kg's Lisp documentation.
- **Green light is the complete CI pipeline**, not only `make check`.

---

## Phase 21 — Measure the engine, not a guessed feature set

This phase implements no language feature and changes no default behaviour.

### 21.1 Deterministic fe counters

Add a compile-time `FE_PERF_COUNTERS` facility that compiles to nothing in a
normal build, following kg's `KG_PERF_COUNTERS` rule.  At minimum count:

- object allocations by final type, plus reclaimed objects;
- string cells and source bytes created/copied;
- symbol interning calls, candidate symbols examined and name bytes compared;
- lexical binding cells examined and parameter-binding pairs allocated;
- function-cell indirections followed;
- evaluator frame pushes and dispatches by broad family;
- macro expansions;
- GC cells examined, newly marked and reclaimed; and
- peak live cells, GC-root depth and frame depth (the existing arena stats,
  reported beside the new totals rather than duplicated).

Instrumentation must not call the clock, allocate, or alter rooting.  A
non-counting build has identical `sizeof` results and generated code at each
instrumented site apart from ordinary compilation noise.

### 21.2 Workload battery

Build an in-process fe runner and extend kg's existing Lisp benchmark cases so
both layers exercise the same named shapes:

1. bare context open/close;
2. kg prelude and post-prelude collection;
3. the representative init and every shipped kg Lisp package;
4. the existing list walk, arithmetic loop, macro-heavy loop and deep call;
5. intern hits and misses after 128, 1024 and 8192 distinct symbols;
6. lexical lookup by environment width and depth;
7. strings at 0, 7, 8, 256 and 8192 bytes;
8. sparse-garbage and dense-live collections; and
9. an interactive command that calls a small Lisp function on every
   invocation.

Each workload checks its answer.  Counter JSON is checked for schema and
deterministic relationships; wall time is emitted under `test/.results/` and
is not gated.

### 21.3 Capability shortlist

Produce a short table of two or three packages or init-file capabilities kg
might actually want next.  This is a decision aid, not vendored test data and
not an implementation commitment.  For each, report first blockers as
described under “Capability evidence”.  Include kg's own likely future uses:
Lisp keymaps/configuration tables, package-local caches, and structured state.

### Gate and result

The phase closes with a checked-in baseline report naming:

- the top three sources of cell allocation;
- the top three sources of lookup/dispatch work;
- which workloads collect and why;
- current live/peak arena margins; and
- the capability shortlist.

No optimisation is allowed in the counter commit.  Phase 22 uses these
numbers; if the data model is not material to any measured or named target,
the wave stops here.

---

## Phase 22 — Storage/value architecture decision

Conditional on Phase 21 finding a material aggregate/string/lookup constraint
or a named capability blocker.  This is a spike and ADR phase.  Its prototype
code does not land in release sources.  It compares two serious designs and
one control.

### Design A — cell chains, as the control

A vector header points to an ordinary pair chain, one cell per element.

- Good: almost no collector work, stable pointers, no new allocator.
- Bad: at least 16 bytes per element, O(n) random access, poor cache locality,
  and the GC-root-stack loop discipline leaks into every constructor.

Measure it so the cheapest implementation has a number.  Do not select it for
public vectors.  It remains suitable for lists, which are already represented
that way.

### Design B — stable objects plus an in-arena payload region

This is the recommended design to beat.

- Keep the current stable 16-byte `FeObject` cells for conses and object
  headers.
- Partition the caller's arena into context, evaluator frames, stable object
  cells and a variable-size payload region.  The split is explicit in a new
  open-options API and visible in arena statistics.
- Allocate payload blocks with a bump pointer.  A block records its byte size
  and stable owning object.  A block is live only when that owner is marked
  and its current payload field still names that block; this distinction makes
  a replaced block dead even though its owner survives.  On exhaustion, mark
  the object graph, discard dead blocks, slide live blocks down with
  `memmove`, and update only the owning object's payload pointer.  Compaction
  runs after marking restores the graph and before the cell sweep clears mark
  bits or reuses an owner.
- A vector payload is a contiguous `FeObject *` array: 8 bytes per element on
  the target build and O(1) indexing.  Its elements point to stable objects,
  so moving the payload rewrites no Lisp edge.
- A string payload carries an explicit byte length followed by bytes.  It can
  contain NUL and move without changing string identity.
- Marking a vector iterates its elements and invokes the existing non-recursive
  marker on each child.  The vector is already marked, so a child cycle back
  to it terminates.  Payload compaction occurs only after pointer reversal has
  restored the graph.
- Replacing/resizing a payload publishes the new block through the stable
  header; the old block becomes garbage.  A collection may occur at every
  allocation boundary.

The price is a partition: unused payload bytes cannot satisfy a cell
allocation and vice versa.  The spike must size and stress that split instead
of hiding it.  A later page allocator could allow regions to trade pages, but
it is not part of the first design.

### Design C — a tagged `FeValue` word

Make Lisp values a public pointer-sized word:

- immediate nil, t and fixnums;
- aligned pointers to heap objects for conses, floats, strings, symbols and
  aggregates;
- two `FeValue` words in a cons; and
- `FeValue` elements in vectors and environment slots.

This removes integer allocation, can shrink several internal paths, and is the
direction nelisp's measurements eventually justified.  It also:

- breaks virtually every fe and kg C signature that currently passes
  `FeObject *`;
- requires a new mark-bit/side-metadata design;
- changes extension callbacks and root handling;
- creates an ABI cut with no incremental compatibility shim worth keeping;
  and
- still needs a variable-payload allocator for strings and vectors.

It is a valid “fe 2” candidate only if boxed scalar churn is a leading measured
cost.  Do not select it merely because tagged values are conventional.

### Explicitly rejected control — host `malloc` payloads

A `FeTPtr`-like header pointing to `malloc` storage makes O(1) vectors easy,
but it breaks the arena's role as the complete budget, introduces allocation
failure and finalisation paths outside arena exhaustion, and makes
`FeCloseContext`/`longjmp` ownership harder.  It is not a candidate in this
wave.  Reopening it is an explicit decision to change fe's core product
contract, not a shortcut inside a vector phase.

### Spike

Prototype A and B as private aggregate types far enough to run:

- create, sequential scan, random read and random write at n = 8, 256, 8192;
- an aggregate containing itself and two aggregates forming a cycle;
- an aggregate containing a host pointer object whose mark callback reports a
  child; and
- forced collection after every allocation in a deliberately small arena.

Run the unchanged kg prelude and representative init with the object region
reduced to B's candidate split.  Use Phase 21's allocation trace to project
the payload bytes their strings would occupy after Phase 25; do not call an
otherwise-empty payload pool “headroom”.

Design C is gated before code.  If scalar boxing is not one of Phase 21's
three leading allocation/time costs, the measured trace eliminates C and the
ADR says so.  If it is, first replay the allocation trace with nil/t/fixnum
boxing removed.  Only when that projection could clear C's selection
threshold is a throwaway tagged-value branch funded, far enough to run the
arithmetic loop, list walk, macro-heavy loop and public-API compile probes.
A full fe/kg API migration is an outcome to plan after selection, not
throwaway work required merely to obtain a number.

Record cell bytes, payload bytes, allocations, collection work, median time,
binary size, affected public declarations and the code/complexity delta.
“Collector diff size” alone is not a quality metric.

### Decision rule

All selectable designs must:

- keep current compatibility cases green;
- provide O(1) vector access, proved by a counter independent of vector length;
- use 8n + O(1) payload bytes for n vector elements on a 64-bit build;
- complete collection without graph-proportional C stack;
- preserve structured exhaustion and recover after it; and
- leave the stable-cell pool below one third of capacity on the prelude plus
  representative-init workload, and the projected payload pool below one
  third after charging every string in that trace plus the selected vector
  capability fixture, at the selected kg arena split.

Select B unless it fails one of those conditions.  Select C only if it either
unlocks a condition B cannot meet, or on Phase 21's two most expensive real
workloads it reduces allocations by at least 50%, improves median evaluator
time by at least 25%, and regresses no existing workload by more than 10%.
Those are return-on-migration thresholds, not package-popularity guesses.

The ADR records the selected layout, exact arena split, failure semantics,
rooting model and why the alternatives lost.  If no design clears the gate,
stop; no public aggregate type follows.

---

## Phase 23 — Land the private payload substrate

Conditional on Phase 22 selecting a design.  The description below assumes
the expected Design B result; a Design C result requires a replacement phase
plan reviewed before implementation.

Land the arena payload allocator/compactor without adding reader syntax or a
Lisp-visible type.

### C contract

- Add an options-bearing context-open API that makes frame/cell/payload
  budgeting explicit.  Keep `FeOpenContext` as the documented default.
- Extend `FeArenaStats` with payload capacity, current/live bytes,
  high-water bytes, compaction count and payload allocation failures.  Existing
  cell fields retain their exact meaning.
- Keep object addresses stable.  Document that an internal payload pointer is
  invalid after any allocation; do not expose one through the public API.
- Establish one publish protocol for a new/replacement block, including the
  roots required across allocation and the point at which the old block
  becomes dead.

### Verification

- allocator unit tests for exact fit, one-byte-over, alignment, stale replaced
  blocks, sliding overlap and deterministic compaction;
- live/dead mixtures with owners in every part of the object array;
- failure injection at every construction allocation;
- GC-stress and sanitiser lanes;
- close-context with live payloads and pointer extension objects;
- an internal child-bearing test object proving mark/compact order; and
- unchanged results and recorded arena margins for all Phase 21 workloads.

`FE_API_VERSION` moves.  `FE_LANGUAGE_VERSION` does not: no Lisp program can
construct the private test object and no reader rule changes.

---

## Phase 24 — Vectors and the sequence contract, as one slice

Vectors are the first public proof because they exercise variable payload,
child tracing, mutation, reader/writer syntax and O(1) access together.

### fe-owned surface

- `[...]` reader syntax and re-readable vector printing;
- `vector`, `make-vector`, `vectorp`, `aref`, `aset` and `vconcat`;
- `length` and `elt` over lists, strings and vectors at their owning layer;
- public C construction, length, checked ref and checked set accessors; and
- `type-of`/type-name, `eq`/`eql` identity and structural `equal` behaviour
  frozen against the oracle before code.

The primitive storage operations belong in fe.  Higher-order combinators may
stay in kg's prelude, but in the same pin-moving kg commit they must accept the
new sequence where Emacs does:

- `mapcar`, `mapc` and `mapconcat`;
- `append` and `copy-sequence`; and
- the shipped `seq-` shim where its contract is generic sequence rather than
  list.

Do not publish vectors while these still fail three frames down in list-only
helpers.

### Required cases

- empty, one-element, nested and self-referential vectors;
- reader error recovery for a missing bracket;
- mutation observed through two references to the same vector;
- bounds and wrong-type condition data;
- a vector containing more than 4032 elements;
- collection during construction and immediately after mutation;
- random-access counters identical at n = 8 and n = 8192;
- list/string/vector oracle cases for every generalised sequence name; and
- one named capability from Phase 21 that previously failed at vector syntax.

Report cell and payload cost for n = 0, 1, 1000 and 8192, including temporary
construction high-water, not only retained size.  `FE_LANGUAGE_VERSION` and
the vector C API version move together.

---

## Phase 25 — Length-bearing, binary-safe strings

Use the same stable-header/payload mechanism to replace the seven-byte cell
chain.  This phase is justified even if no external package mentions a new
function: strings already dominate source reading, symbol names and kg's C
boundary.

### Representation and API

- A string object has stable identity, explicit byte length and an arena
  payload that may contain NUL.
- Add `FeMakeStringBytes(ctx, bytes, length)` and a length-aware copy API.
  Keep `FeMakeString` as the NUL-terminated convenience wrapper.
- Do not expose a borrowed payload pointer unless its “valid until the next
  possible allocation” lifetime can be made impossible to misuse in kg.
- Symbol names use the same representation without changing symbol identity.
- Reader, writer, equality, order, substring and formatting stop using
  `strlen` as a data-model operation.

The representation being binary-safe does not by itself promise Emacs'
unibyte/multibyte duality.  The language contract for this phase is UTF-8
text plus explicit bytes, with character-indexed operations continuing to use
kg's existing codepoint policy.  Freeze oracle cases for NUL, non-ASCII,
invalid byte input, `aref` and `aset` before deciding exactly which forms
agree and which remain documented divergences.

### Performance and correctness gates

- embedded NUL round-trips through read/write and the C API;
- 0, 7, 8, 256 and 8192-byte strings survive compaction;
- symbol lookup remains correct when its name payload moves;
- string mutation preserves the stable header even when a codepoint-width
  change requires a replacement block;
- old string/symbol goldens stay byte-identical except where a planned
  divergence closes; and
- Phase 21's string, reader, prelude and interning counters are reported
  before/after.

This phase may reduce cells while increasing payload pressure.  Both numbers
must be shown.

---

## Phase 26 — Index symbols and extract the hash substrate

This is an internal performance phase before a user hash-table API.

Keep `symbol_list` as the permanent GC root and enumeration order.  Add an
arena-owned open-addressed index from name bytes/hash to the stable symbol
object.  `FeMakeSymbol` and `intern-soft` consult the index; creating a symbol
publishes it to both structures as one recoverable operation.  Give the index
a private stable owner rooted by the context, so it follows Phase 23's one
payload-ownership rule rather than teaching the compactor a context-only
exception.

Requirements:

- a miss never interns, preserving `intern-soft`'s double-probe contract;
- resizing and tombstone policy allocate only through the payload substrate;
- hash collisions compare length and bytes, including embedded NUL where the
  symbol-name policy permits it;
- partial failure leaves neither a symbol visible only in the list nor only
  in the index;
- `FeCloseContext` and collection need no special external finaliser;
- a debug check can rebuild/compare the index against `symbol_list`; and
- candidate-probe counters grow approximately O(1), not O(symbol count), at
  the Phase 21 sizes.  The gate is a bounded probe relationship, not a flaky
  time limit.

At the same time, define the internal hash/equality contracts user tables will
need for `eq`, `eql` and `equal` keys.  Do not expose a Lisp hash table yet.
Cycles, mutable keys, float corner cases and a recursion/step bound must have
answers before Phase 27 builds on them.

---

## Phase 27 — User hash tables

Conditional on the Phase 26 substrate being green and either:

- a named kg/package capability needs a table; or
- kg itself has an internal Lisp-facing cache/configuration use that is
  clearer as a hash table than as an alist.

Implement:

- `make-hash-table` with `:test` restricted to `eq`, `eql` and `equal`;
- `gethash`, `puthash`, `remhash`, `clrhash` and `maphash`;
- `hash-table-p`, `hash-table-count` and `hash-table-test`;
- checked C accessors if kg needs them; and
- opaque printing.  `#s(hash-table ...)` reading/printing, weakness,
  user-supplied test functions and Emacs' full sizing/rehash knob set are
  explicit divergences, not accepted-and-ignored arguments.

Freeze semantics for mutation during `maphash`, re-entrant callbacks, a key
mutated after insertion, NaN/signed-zero keys and cyclic `equal` structures
before implementation.  Exercise more than 4032 entries, collision-heavy
tables, repeated grow/clear cycles, self-reference and table↔table cycles
under GC stress.

The acceptance result is an end-to-end named capability, not “the audit now
has N fewer missing names”.

---

## Phase 28 — Choose the next semantic substrate

Do not automatically continue from hash tables to `cl-defstruct`.  Re-run the
Phase 21 measurements and capability table, then choose one separately
planned branch:

1. **First-class lexical environments.**  This is the strongest likely next
   semantic investment: it can close `eval`'s LEXICAL argument,
   `macroexpand` environments, the one-argument-`defvar` closure residual and
   potentially alist lookup costs.  It needs an explicit environment object
   and a closure contract; it is not a vector footnote.
2. **Records.**  Add `record`, `make-record`, `recordp` and accessors as a
   typed vector-like object if a named target needs them.  Reader `#s(...)`
   is a separate serialization decision.
3. **`cl-defstruct`.**  Treat this as a library/macro compatibility project
   after records, not as part of the record representation.  It does not
   commit kg to all of `cl-lib`, but the supported `cl-` neighbourhood must
   be stated before advertising it.
4. **Macro-expansion caching or another evaluator optimisation.**  fe expands
   macros on every invocation.  Take this branch if Phase 21 still shows it
   dominating realistic code after the storage work; preserve macro
   redefinition semantics explicitly.
5. **Tagged `FeValue` migration.**  Reopen Design C if arithmetic/scalar
   boxing remains a leading cost and now clears the same migration thresholds
   it failed in Phase 22.

Each branch gets its own plan and compatibility milestone.  None is selected
by aggregate wild-corpus references.

## Declined and watch items

- **The original `utils/forecast/wild/` gate:** declined.  A selected,
  licensed package may be vendored as a fixture; an arbitrary popularity
  census is not roadmap authority.
- **Linear-access vectors:** benchmark control only, not a shippable vector
  representation.
- **Out-of-arena aggregate payloads:** declined while the fixed arena remains
  fe's product contract.
- **A moving object heap:** not needed by the recommended design.  Payloads
  may move behind stable headers; host-visible objects do not.
- **Bytecode/JIT:** the 2026-08-07 measured decline stands.  Storage work does
  not reopen it.
- **Arena image/pre-parsed embedding:** the embedded-prelude plan's measured
  decline stands.
- **Text properties/overlays:** editor buffer-model work, not this data-model
  wave.
- **Char-tables/bool-vectors:** no current kg consumer; require their own
  named capability.
- **Weak hash tables:** require ephemeron/weak-edge collector semantics and
  are not smuggled into the ordinary hash-table phase.
- **Bignums:** the integer-overflow divergence stays recorded.  A payload
  allocator makes bignums possible but does not fund them.

## Recommended execution summary

Phase 21 is unconditional if this wave starts.  Phase 22 starts only if 21
finds a material engine cost or named capability blocker; otherwise the wave
ends with its baseline.  Phase 23 lands only after an architecture clears the
ADR gate.  Vectors/sequences (24), strings (25), and symbol indexing (26) are
the coherent foundation and are the recommended implementation wave.  Hash
tables (27) require a named consumer.  Records, lexical environments and
evaluator optimisation compete openly in Phase 28 rather than being assumed
by a data-type checklist.

That sequence improves fe even if no third-party package ever loads: it leaves
kg with measured evaluator costs, a bounded variable-payload mechanism,
complete vectors, better strings and scalable interning.  It also leaves a
clean stopping point after every phase, which is the scope discipline worth
borrowing from nelisp.
