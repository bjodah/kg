# Fe architecture review

Date: 2026-07-30
Scope: `fe/fe.c`, `fe/fe.h`, Fe documentation/tests/fuzzers, and kg's
`src/lisp.c` integration. This was a read-only product-code review.

## Executive assessment

Fe is an unusually understandable interpreter with a useful embedding model:
one caller-owned arena, no hidden allocator, a small public API, explicit GC
protection, and a host-controlled evaluation budget. kg also does careful work
around Fe's non-local error path: it checkpoints the GC stack, tracks malloc'd
native temporaries, roots registered commands, and restores state after
`longjmp` (`src/lisp.c:62-85`, `src/lisp.c:112-140`,
`src/lisp.c:2002-2039`).

The compactness is now being spent in the wrong places, however. Destructive
macro expansion has two reproducible semantic bugs. The reader silently
normalizes malformed dotted lists. The writer can corrupt memory on a
zero-sized destination and cannot terminate on cyclic data. The evaluator
cannot distinguish unbound from nil, does not enforce Lisp-function arity, and
has no language-level non-local exit or cleanup mechanism. Those are not
independent missing conveniences: they mark the seam where further Emacs Lisp
compatibility will become increasingly fragile if implemented only in kg's
prelude.

My recommendation is **not** a large rewrite. First fix the four concrete
correctness/availability issues below. Then introduce three small internal
abstractions—an expansion API, explicit binding cells with an unbound sentinel,
and evaluator completion/unwind records—while retaining fixed arena allocation
and the current two-word object. That sequence unlocks useful Lisp features
without committing immediately to bytecode, moving GC, or a large standard
library.

## Priority order

1. **Stop shallow-copying macro results into call-site objects.** It currently
   produces a truthy fake nil and defeats lexical lookup for an atom-symbol
   expansion. This is the highest priority correctness issue.
2. **Reject malformed dotted-list syntax.** The current reader silently changes
   source data.
3. **Make serialization bounded and cycle-safe, including `size == 0`.** The C
   API has a direct memory-safety edge and cyclic values can bypass kg's
   evaluation budget indefinitely.
4. **Add exact arity and an internal unbound value.** These are high-leverage
   semantic foundations for `defun`, `defvar`, useful diagnostics, optional
   arguments, and introspection.
5. **Add evaluator completion and unwind records before adding
   `condition-case`, `catch`/`throw`, or `unwind-protect`.** Building those atop
   the host's current `longjmp` would make editor/resource invariants
   unreviewable.
6. **Replace process-global custom-type slots with per-context descriptors.**
   Do this as an API-versioned migration, not by adding more `FeTFexN` tags.
7. Only after measurement, consider an explicit evaluator stack and/or compact
   bytecode. Symbol interning and cons allocation are more obvious near-term
   performance targets than a general VM rewrite.

## Validated findings

Severity meanings: **critical** permits memory corruption through a supported
API; **high** is wrong language behavior, source corruption, or an editor hang;
**medium** is a material compatibility/diagnostic defect or unsafe API
footgun. Confidence is confidence in the claim as stated, not in a proposed
fix.

### FE-1 — Destructive macro expansion creates noncanonical atoms and breaks lexical lookup

**Severity:** high
**Confidence:** high
**Evidence:** `fe/fe.c:1310-1317`; nil identity at `fe/fe.c:126-127` and
`fe/fe.c:356-358`; lexical lookup uses symbol pointer identity at
`fe/fe.c:828-839`.

The macro path evaluates the macro body, then performs:

```c
*obj = *DoList(...);
return Evaluate(ctx, obj, env, NULL);
```

This assumes every expansion can safely be represented by overwriting the
existing pair cell. That is false for atoms:

* Copying the global `nil` object into the pair produces an arena object tagged
  `FeTNil`, but `FeIsNil()` recognizes only the address of the global `nil`.
  The value prints as `nil` but is true in conditionals.
* Copying an interned symbol makes a second symbol object. It shares the global
  binding cell through its `cdr`, so global lookup appears to work, but lexical
  environments compare the symbol object's address. The clone misses a local
  binding and falls through to the global value.

Reproduced against the current `fe/fe`:

```lisp
(= m (macro () nil))
(if (m) (print "truthy") (print "falsey"))
;; prints: truthy

(= x 42)
(= mx (macro () (quote x)))
(print ((lambda (x) (mx)) 7))
;; prints: 42, not 7
```

The steered fuzzer does generate macros, but its macro always generates a
`(quote DATUM)` pair (`fe/fuzz/fuzz_eval.c:139-149`), so it cannot reach either
atom-expansion defect.

**Immediate fix:** stop overwriting the call cell. Evaluate the returned
expansion object directly. If expansion caching is retained, cache an
`FeObject *` in a dedicated, GC-visible call-site wrapper rather than copying
object bytes. The smallest safe initial change is no cache: expand on every
evaluation, preserving observable macro redefinition and avoiding a new object
kind.

**Tests:** macro expanding to `nil`; to `t`; to a symbol with a shadowing
lambda parameter; to a number and string; macro redefinition after a first
call; expansion whose returned form is shared elsewhere. Add atom expansions
to the evaluator fuzzer.

### FE-2 — The reader accepts malformed dotted lists and silently rewrites them

**Severity:** high
**Confidence:** high
**Evidence:** `fe/fe.c:912-933`.

On seeing `.`, the reader assigns one following form to `*tail`, but it neither
requires an existing head nor requires the next token to be `)`. The loop then
continues and can overwrite or discard the dotted tail.

Reproduced:

```text
'(a . b c)   => (a c)
'(a . b . c) => (a . c)
'(. a)       => a
```

Source is therefore accepted with a meaning different from both the written
datum and normal Lisp syntax.

**Fix:** track whether the list has at least one element; after dot, read
exactly one form using the internal reader, then require the next internal read
to return `rparen`. Reject leading dot, repeated dot, or trailing forms.

**Tests:** the three cases above, `(a .)`, `(a . b)`, nested dotted pairs,
comments/whitespace around dot, and byte offsets from `FeReadString`.
Keep equivalent seeds in the raw-reader corpus.

### FE-3 — `FeToString(..., size == 0)` underflows and writes through an invalid destination

**Severity:** critical
**Confidence:** high (source proof)
**Evidence:** `fe/fe.c:748-765`; public declaration `fe/fe.h:109-112`.

`FeToString` initializes `.size = size - 1`. At zero, that becomes `SIZE_MAX`;
the writer consequently treats the destination as enormous, writes the first
byte through `dst`, and finally writes a NUL. A null destination crashes; a
non-null zero-length destination violates its bound. Neither the header nor
the C API documentation specifies `size > 0` as a precondition.

**Fix:** define snprintf-like behavior. If `size == 0`, perform no writes and
return the required (or emitted) length; if retaining the current
"bytes actually stored" return contract, simply return zero. Validate
`dst != NULL` whenever `size > 0`. Preferably split this into:

* a no-allocation bounded display writer with explicit status; and
* an exact-length/query API.

**Tests:** `(dst, size)` of `(NULL, 0)`, `(valid, 0)`, `(NULL, 1)`, one-byte
buffer, exact buffer, truncation, and very large logical output under
ASan/UBSan.

### FE-4 — Cyclic conses make the writer nonterminating and bypass evaluation control

**Severity:** high for kg availability; medium for a trusted standalone host
**Confidence:** high
**Evidence:** cycles are constructible at `fe/fe.c:1211-1214`; pair output is
unbounded recursion/spine iteration at `fe/fe.c:671-685`; evaluation polling is
only charged by evaluator paths such as `fe/fe.c:304-317`.

Reproduction:

```lisp
(= x (cons 1 nil))
(setcdr x x)
(print x)
```

Under `timeout 1s`, current Fe timed out after writing about 43 MB. The same
core writer is used by `FeToString`; kg's `%S` formatting reaches it from a
native callback. Evaluation step limits and C-g polling cannot interrupt work
inside the writer, exactly as the current API documentation cautions for a
non-returning native. Here, however, no hostile C native is needed—ordinary
Lisp pair mutation is enough.

Recursive `car` structure can likewise exhaust the C stack in `FeWrite`; the
collector has the same known recursive-car issue (`fe/fe.c:379-419`,
`fe/doc/implementation.md`, Known Issues).

**Fix:** make traversal iterative and bounded. A practical compact design is a
small arena-backed traversal stack plus a visited set or tortoise/hare detection
for list spines. Emit a stable cycle marker (for example `#<cycle>`) or raise a
serialization error. Charge traversal against the ambient evaluation control
when one exists, and expose a byte/depth limit for host calls.

**Tests:** cdr cycle, car cycle, shared-but-acyclic structure, deep tree,
interrupt callback during rendering, byte limit, writer callback failure.
Add a separate bounded cyclic-object fuzz target; the existing evaluator
fuzzer deliberately excludes cycles (`fe/doc/FUZZING.md:27-41`).

### FE-5 — Lisp functions silently accept wrong arity and invalid parameter shapes

**Severity:** medium
**Confidence:** high
**Evidence:** `fe/fe.c:1092-1107`, dispatch at `fe/fe.c:1303-1308`; kg
explicitly relies on the behavior at `src/lisp.c:1838-1841`.

For a proper parameter list, missing arguments become nil because `FeCar(nil)`
and `FeCdr(nil)` return nil; extra arguments are ignored after parameters run
out. Thus `((lambda (x) x))` returns nil and `((lambda () 1) 2)` returns 1.
Parameter names are not checked to be symbols. This makes misspelled calls
quietly wrong and forces kg's `&optional` lowering to conflate optional with
missing required arguments.

**Fix:** parse/validate a function's argument specification once when creating
the closure, or at least validate it at call time. Store required count,
optional count, and rest symbol in a compact descriptor. Signal a structured
`wrong-number-of-arguments`; macros use the same binder. This can preserve
Fe's bare-symbol varargs and dotted rest syntax.

**Tests:** zero/exact/few/many arguments, dotted and bare rest, invalid
non-symbol parameters, duplicated parameters, malformed `&optional`/`&rest`,
native-to-Lisp `FeCall`, and macro arity.

### FE-6 — Unbound and bound-to-nil are indistinguishable

**Severity:** medium, architectural blocker
**Confidence:** high
**Evidence:** symbols initialize their binding cell to nil at
`fe/fe.c:571-584`; missing lexical bindings return the global binding cell at
`fe/fe.c:828-839`; symbol evaluation returns that value at
`fe/fe.c:1272-1275`. kg documents the resulting incorrect `defvar` behavior at
`src/lisp.c:1894-1899`.

Unknown variables silently evaluate to nil. Consequently `boundp`,
`makunbound`, correct `defvar`, useful typo diagnostics, and distinct
void-variable conditions are impossible. The prelude reinitializes a variable
whose legitimate value is nil.

**Fix:** add one private immortal `unbound` sentinel (it need not be a
user-visible type), initialize symbol binding cells to it, and raise
`void-variable` when evaluating it. Add internal/global lookup operations that
return "found" separately from the value; expose `boundp` and `makunbound`.
Do not use nil or a user-constructible symbol as the sentinel.

**Compatibility risk:** scripts may intentionally depend on unknown variables
being false. Gate the change behind a language-version mode for one release or
make it part of an explicitly versioned Fe 2 behavior change. kg should enable
the strict mode immediately because its surface claims Emacs shape.

## Architecture for richer Lisp without abandoning the arena

### 1. Evaluator completion records and an unwind stack

Today `FeHandleError` clears a few context fields and invokes a host callback
that must not return (`fe/fe.c:248-281`). kg's callback `longjmp`s to one
top-level frame (`src/lisp.c:129-140`). kg has done the necessary manual
bookkeeping for its current malloc'd temporaries, but this model cannot
correctly express nested Lisp handlers or cleanup:

* `condition-case` needs to intercept a condition inside evaluation rather than
  transfer directly to the host.
* `unwind-protect` must run cleanup for normal return, error, `throw`, quit, and
  future editor exits.
* `catch`/`throw`, `save-excursion`, `save-restriction`, temporary buffer
  selection, and dynamic bindings all need deterministic unwinding.

Introduce an internal completion:

```text
NORMAL(value) | SIGNAL(condition) | THROW(tag, value) | QUIT
```

and an arena-backed linked stack of unwind records. Each evaluator boundary
propagates a completion; matching handler/catch forms consume it; every record
crossed runs its cleanup. Only an unhandled completion reaches the embedding
error callback. This is mechanically larger than `longjmp`, but it keeps all
state in the fixed arena and makes behavior testable. A trampoline/explicit
control stack can be a later refactor; completion propagation can first be
implemented in the recursive evaluator.

Do not expose raw `jmp_buf` or allow arbitrary C callbacks as cleanup records.
Host natives that acquire heap/OS resources should register a host cleanup
record with an opaque pointer and a non-allocating callback. kg can then remove
the special-purpose `load_buffers`/`scratch` recovery lists over time.

**Features unlocked:** `condition-case`, `signal`, `error`, `catch`/`throw`,
`unwind-protect`, `ignore-errors`, robust `save-excursion`, C-g as `quit`, and
safe temporary editor state.

### 2. Binding cells, lexical frames, and optional dynamic bindings

Keep interned symbol objects and their O(1) global binding cell, but stop using
an untyped alist for every lexical access. A compact frame can still be a list
of `(symbol . value)` cells initially; add:

* the private unbound sentinel;
* validated lambda descriptors;
* explicit frame parent links;
* separate lookup/set operations returning found/not-found;
* optional "special" symbols whose values are pushed as unwind records.

This preserves lexical scope as the default and adds controlled dynamic
binding where editor variables benefit from it. It also creates a natural
place for buffer-local values later: a symbol's global binding can point to a
small binding descriptor whose active value is selected by host/context.
Avoid switching wholesale to Emacs' historical dynamic default.

**Features unlocked:** correct `let`/`let*`, optional/rest args,
`boundp`/`makunbound`, `defvar`, special variables, buffer-local variables,
function advice hooks that can bind context, and better backtraces.

### 3. Non-destructive macro expansion and syntax ownership

Separate `MacroExpandOne` from `Evaluate`. Expansion should return a rooted
object without mutating source. Add public or host-native `macroexpand-1` and
`macroexpand`, with a step/depth limit. Keep reader punctuation in Fe, but keep
most language macros in kg's prelude as today.

If caching becomes important, cache by `(macro identity/version, source
identity)` in a GC-visible side table. Invalidation on `defmacro` is mandatory.
Do not shallow-copy arbitrary objects and do not mutate quoted user data.

Longer term, source locations should be metadata attached outside the
two-word object (for example an arena hash table keyed by object address), so
macro errors can report expansion and call locations without inflating every
cons.

**Features unlocked:** reliable `defmacro`, nested quasiquote, macro debugging,
macro redefinition, hygienic helper generation if desired, and load-time
expansion/compilation.

### 4. Per-context extension descriptors

The public API exposes mutable process-global `type_names`, global `nil`, three
fixed `FeTFexN` slots, and unconstrained `FeMakePtr(type, ptr)`
(`fe/fe.h:25-66`, `fe/fe.h:91-99`). The C API documentation already labels
these legacy and slated for replacement (`fe/doc/c-api.md:333-346`).

For Fe API v2, register a per-context type descriptor:

```text
name, mark(ctx,payload), finalize(ctx,payload), write(ctx,payload,writer),
equal(ctx,a,b)
```

Return an opaque type handle rather than extending the public enum. Validate
handles in construction and conversion. Keep core object tags compact by
using one `external` tag whose payload references the descriptor. Finalizers
must not allocate Fe objects and should run exactly once at close/collection.

For native functions, add a descriptor carrying name, arity, flags
(`may_allocate`, `may_reenter`, `interruptible`), and documentation. Preserve
the current simple `FeDefineNative` as a convenience wrapper.

**Features unlocked:** kg buffer/window/marker objects, user data types,
structured conditions, callable host objects, introspection/help, and multiple
independent Fe contexts in one process.

### 5. Reader/writer and object-model evolution

Near-term reader work should remain streaming and arena-only:

* remove the 63-byte atom limit by building a temporary arena string or
  intern probe incrementally (`fe/fe.c:862-887`);
* make escape output round-trip—currently quoted writing escapes `"` but not
  backslash or control bytes (`fe/fe.c:692-707`);
* return structured reader status (`object`, clean EOF, error) rather than
  overloading NUL and null pointers;
* retain byte offsets and add line/column only in the higher-level source
  adapter.

Vectors and hash tables need not force moving storage. A vector can be a small
header object plus chained fixed-size payload cells, much like strings. A hash
table can use arena cells and open addressing, sized at creation. Both should
participate through core tracing, not legacy external pointers.

The largest representation tax today is strings: on a 64-bit host each
16-byte object carries only seven string bytes, and symbols add a string chain,
binding pair, symbol object, and intern-list node (`fe/fe.c:118-123`,
`fe/fe.c:548-585`). Before adding new object types, measure whether a
length-bearing string/header plus payload cells reduces both prelude footprint
and interning cost enough to justify an API v2 layout change.

## Performance plan

Performance changes should be measurement-driven. Add three stable benchmarks
before changing representation:

1. cold context creation + kg prelude load (time, objects, peak GC stack);
2. symbol-heavy source read/evaluate (intern probes and bytes compared);
3. iterative list processing and recursive function calls (evaluation steps,
   allocations, collections, maximum C/GC depth).

Add optional per-context counters compiled out by default: objects allocated,
live/free objects after GC, collections, marked objects, maximum GC stack,
symbol probes, evaluator steps, macro expansions, and writer nodes/bytes.

Likely wins, in order:

* hash or bucket symbol interning while preserving symbol identity;
* prevalidated lambda descriptors and faster lexical frames;
* fewer temporary conses in `FeCall` (it currently builds quoted source forms,
  `fe/fe.c:1370-1392`) via a direct internal callable dispatch;
* non-destructive macro expansion with optional valid cache;
* iterative mark traversal to remove C-stack risk;
* only then, a compact bytecode/control-stack VM for hot loaded functions.

A bytecode VM would unlock proper tail calls and cheaper repeated command
execution, but it increases debugging, source mapping, GC-root, and
non-local-exit complexity. It should follow—not precede—the completion and
binding work.

## Phased delivery, cost, and risk

### Phase 0 — Correctness hardening (small, days)

Fix FE-1 through FE-4 and add focused unit/fuzz seeds. Add zero-size API tests
under sanitizers. No intended language expansion.

**Risk:** macro behavior changes because expansions are no longer cached.
Document this as a correction; later caching must be explicit and validatable.

### Phase 1 — Semantic foundations (medium, roughly 1–2 weeks)

Add unbound sentinel, strict arity/lambda descriptors, non-destructive
`macroexpand-1`, and round-trippable writer escapes. Provide a temporary
compatibility option for unbound-variable behavior.

**Risk:** existing Fe scripts may rely on nil-for-unbound or lax arity. Run all
Fe scripts, kg native tests, and PTY Lisp cases in both modes; migrate kg
prelude to strict mode first.

### Phase 2 — Completion/unwind engine (large, roughly 2–4 weeks)

Introduce internal completion propagation and unwind records while retaining
the recursive evaluator. Implement `catch`/`throw`, `unwind-protect`, `signal`,
and a minimal `condition-case`. Turn C-g into a distinct quit condition. Port
kg's temporary editor-state helpers only after the core tests are strong.

**Risk:** every evaluator return path changes. Use table-driven conformance
tests crossing function, macro, native re-entry, nested load, GC, and each
completion kind. Run ASan/UBSan/MSan, Valgrind, analyzers, and long fuzz
campaigns.

### Phase 3 — Extension API v2 (large, 2–3 weeks plus migration)

Add per-context external-type and native descriptors; deprecate global type
names, `nil`, and `FeTFexN`. Keep v1 wrappers where safe, bump
`FE_API_VERSION` for removals/layout-visible changes, and publish a migration
table. Adapt kg behind `src/lisp.c` only.

**Risk:** embedders and Fex modules. Compile a v1 compatibility test and a v2
multi-context test in CI.

### Phase 4 — richer editor Lisp and measured optimization (incremental)

Build markers, buffer/window objects, hooks, buffer-local/special variables,
interactive argument specs, keymaps, `save-excursion`, and package
`provide`/`require` atop the foundations. Then evaluate symbol hashing,
representation changes, and bytecode against the benchmarks.

**Risk:** surface-area growth. Require each feature to have a bounded core
primitive set, a prelude/library layer where feasible, and WITH_LISP=0 parity
for kg's non-Lisp editor.

## Test and analysis additions

The existing pipeline is strong—coverage threshold, GCC analyzer, Valgrind,
ASan/UBSan, MSan, two fuzzers, IWYU, clang analyzer, cppcheck, clang-tidy, and
complexity ratchets are already documented in `fe/AGENTS.md`. The main gap is
semantic oracles and excluded cyclic/non-local behavior, not another general
lint tool.

Add:

* a reader round-trip/property target: `read(write(x))` for representable
  acyclic objects, plus explicit malformed dotted syntax;
* a bounded cyclic object-graph fuzzer that exercises mark, write, equality,
  close, and finalizers without relying on source generation;
* a macro differential/property suite asserting atom expansion, lexical
  capture, no source mutation, repeatability, and redefinition;
* a completion/unwind matrix with allocation and host re-entry at every edge;
* API contract tests for every `(pointer, length)` zero/null combination and
  deliberately invalid root/type handles;
* differential tests for the deliberately Emacs-shaped subset (reader forms,
  arity, `let`, macro expansion, conditions), comparing normalized values and
  condition names against `emacs -Q --batch`;
* benchmark budgets as reported trends first, ratchets only after several
  stable CI runs.

Complexity tooling should track the evaluator by responsibility, not merely
permit a larger `EvaluatePrimitive` switch. Split reading, expansion,
binding/call dispatch, completion propagation, and serialization into
independently tested modules/functions before adding new special forms.

## What I would not do

* Do not add richer error handling by stacking more host `setjmp` frames.
* Do not add more fixed `FeTFexN` enum values or process-global names.
* Do not implement vectors/hash tables as untraced raw pointers merely to
  preserve the current object enum.
* Do not pursue full Emacs Lisp compatibility. Preserve Fe's lexical,
  arena-bounded identity and select the high-productivity subset.
* Do not start with bytecode or a moving collector. Both are plausible later,
  but neither fixes the current semantic foundations.

## Suggested first merge sequence

1. FE-3 zero-size `FeToString` fix and sanitizer/API tests.
2. FE-2 dotted-list rejection and reader seeds.
3. FE-1 non-destructive macro evaluation plus atom/lexical regression tests
   and fuzzer grammar expansion.
4. FE-4 bounded/cycle-safe writer and cyclic graph tests.
5. Design note for unbound sentinel + strict lambda descriptors, including the
   compatibility switch and kg prelude migration.
6. Design note and prototype tests for completion/unwind records before any
   user-visible condition feature.

That order removes direct hazards first, keeps reviews small, and creates a
credible path to richer kg extensibility without sacrificing Fe's best
property: its entire language runtime remains visible, bounded, and owned by
the embedding context.
