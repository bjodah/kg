# Plan 05 — Fe correctness foundations and extension resource safety

## Goal

Repair Fe's reproduced semantic/API defects, make errors and external resources
safe enough for richer kg callbacks, and introduce compatibility-sensitive
language changes in a controlled order.

Landing order: (1) `FeToString()` zero-size writes; (2) malformed dotted lists;
(3) destructive macro expansion; (4) cyclic/unbounded writer traversal; (5) lax
function/macro arity; (6) absence of an unbound value; (7) Fex file
double-close/leaks; (8) Fex argument/path/write truncation; (9) a unified
cleanup/unwind design; (10) per-context extension descriptors.

kg compiles only `fe/fe.c` (kg `Makefile:191`), so phases 7–8 and 10 have no kg
consumer today; they belong to Fe's own branch and standalone CI.

## Verification status (re-checked against the pinned tree)

Pinned Fe is `0ca1229`, checked out **detached** at `origin/analyzers-etc`
(`git submodule status fe`). All line numbers below are from that commit.

| Phase | Claim | Verdict |
| --- | --- | --- |
| 1 | `FeToString(dst, 0)` underflows and writes | **confirmed**, scratch host: `size=0` wrote `hello\0` into a 0-size buffer and returned 5 (`fe.c:761-766`) |
| 2 | `'(a . b c)` reads as `(a c)` | **confirmed** by `./fe`: `(a c)`, `(a . b . c)` → `(a . c)`, `(. a)` → `a` (`fe.c:912-934`) |
| 3 | Macro expansion fakes `nil`, breaks symbol identity | **confirmed** by `./fe`: `(if ((macro () nil)) …)` takes the true branch; `((lambda (x) (mx)) 7)` prints `42` (`fe.c:1310-1317`) |
| 4 | Writer is unbounded on cycles, bypasses the budget | **confirmed**: `(setcdr x x)` then `(print x)` runs until killed (`fe.c:671-686`, no `EvaluationStep` on that path) |
| 4 | Recursive `car` printing risks the C stack | **plausible, not reproduced**: nesting is bounded by arena slots first (`out of memory` at kg's 1 MiB). `FeMark` has the identical recursion and it is a documented known issue (`fe/doc/implementation.md:188-190`) |
| 5 | Lax arity, unchecked parameter shapes | **confirmed**: `((lambda (x) x))` → `nil`, `((lambda () 1) 2)` → `1`, and `((lambda (1) 5) 2)` → `5` (`fe.c:1092-1107`, `fe.c:1303-1308`) |
| 6 | Unbound == nil | **confirmed** (`fe.c:571-585`, `fe.c:828-839`, `fe.c:1273-1275`); Fe's own `TODO.md` already calls this out |
| 7 | Fex file lifecycle | **confirmed, and worse than stated**: `FexGC` returns `&nil` for `FexTFile` (`fex.c:12-34`), so there is *no* finalizer — unclosed files leak fds; `FexCloseFile` (`fex_io.c:34-38`) `fclose`s a raw pointer, so double close is UB and `(close-file stdout)` is allowed (`fex_io.c:29-31`) |
| 8 | Fex truncation / `waitpid` EINTR | **confirmed** (`fex_process.c:26-46,61`, `fex_io.c:40-91`) |

Two claims in the source plan were **wrong** and are corrected below: the
reader-offset rationale in phase 2, and the "writer callback can express
failure" requirement in phase 4 (`FeWriteFn` returns `void`, `fe.h:22`).

## Repository, submodule and change policy

Fe is pinned by *branch*, not SHA (`doc/fe-upstream.md`), and the working tree
is detached, so every Fe change starts with
`git -C fe checkout analyzers-etc && git -C fe pull --ff-only`. Then:

1. implement, test and commit **inside `fe/` on `analyzers-etc`**, and push;
2. update `fe/doc/language.md`, `c-api.md`, and/or `implementation.md`;
3. add Fe-native script/API tests (see "Test strategy" below);
4. pass `make -C fe check` and `(cd fe && .ci/run-ci-steps.sh)`;
5. in kg, `git add fe` as its **own** commit saying what moved and why;
6. add every new behavior difference to the divergence table in
   `doc/fe-upstream.md` — each phase here creates one, and that table is the
   only record that `fe.c` is not pristine upstream;
7. update kg docs when semantics leak out: `README.md:264` (arity/`&optional`),
   `README.md:287` (`defvar`), the `README.md` "macro expands once per call
   site" bullet, and the matching `doc/kg.1` entries.

A `fe/` edit that is not committed on the branch is lost at the next
`git submodule update`.

## Budget the complexity ratchets before writing code

Fe's CI stage 1 (`fe/.ci/ci-01-complexity.sh` → `make complexity-check
pmccabe-check`) is the first thing every phase here will break. Measured now:
`SCC_COMPLEXITY_MAX = 172` against a current total of **171** across
`$(SOURCES)` — one added branch keyword anywhere in Fe fails the gate;
`SCC_FILE_COMPLEXITY_MAX = 98` with `fe.c` at **92**; and
`PMCCABE_FUNCTION_COMPLEXITY_MAX = 22` with `Read` at **18**,
`EvaluatePrimitive` and `FeWrite` at **15**.

scc counts branch keywords per file, so extracting helpers does *not* buy scc
headroom — only pmccabe headroom. Therefore each phase must either stay
keyword-neutral or raise the ratchet in the same commit with a written
justification; `fe/AGENTS.md` permits that only for "reviewed structural
growth". Plan on `SCC_COMPLEXITY_MAX`/`SCC_FILE_COMPLEXITY_MAX` moving once,
deliberately, in phase 2, and on extracting `ReadList()` out of `Read()` and a
traversal helper out of `FeWrite()` to keep pmccabe under 22.

Fe's coverage gate is 80% of lines (`COVERAGE_MIN_LINES`). New error paths
must be exercised by a script or `test_api.c` case or coverage drops.

## Test strategy (Fe's actual harness)

`fe/test.sh` is the whole regression suite: for every `scripts/*.fe` it runs
`./fe scripts/assert.fe "$s"` and compares **both** stdout against
`tests/<name>.out` and stderr against `tests/<name>.err`, byte for byte, twice
(plain, then `RELEASE=1`, which the Makefile currently ignores).
`make -C fe check` also builds and runs `test_api` and `example_host`.

Consequences:

- **Error-message tests are golden stderr files**, including the
  `label:offset:` prefix `FeHandleError` composes (`fe.c:261-276`). Choose the
  wording once; later edits are golden-file churn.
- A script that dies stops there, so error cases go one-per-script or through
  `main.c`'s recoverable REPL. Prefer `test_api.c` when one run needs many
  failures. `scripts/assert.fe` is preloaded, so `(assert …)` is available.
- **Fuzz corpora are not tracked**: `fuzz/corpus/` and `fuzz/artifacts/` are in
  `fe/.gitignore`; the tracked seeds are `fuzz/fe.dict` and `scripts/*.fe`
  (`Makefile:114-129`). "Add a seed" means a `fe.dict` token or a *valid*
  script — a malformed reader input cannot become a script without a golden
  `.err`. Say which, per phase.
- The steered evaluator fuzzer excludes cycles and self-reference by design
  (`fe/doc/FUZZING.md:27-41`); read it before widening any grammar.

## Read first

- `fe/AGENTS.md` (especially "Memory and error invariants"), and
  `fe/doc/{implementation,language,c-api,FUZZING}.md`
- `fe/fe.c`: reader `Read()` (889), writer `FeWrite()` (652), `FeToString()`
  (761), `ArgsToEnv()` (1092), `Evaluate()` (1268) macro case (1310),
  `GetBound()` (828), `FeHandleError()` (248), GC/roots (379-448);
  `fe/fe.h`, `fe/fex.h`, `fe/fex.c`, `fe/fex_io.c`, `fe/fex_process.c`
- kg `src/lisp.c`: prelude (1700-1908), recovery/`FeToString` (2010-2040),
  command roots and `FeCall` (1435-1580); kg `doc/fe-upstream.md`

## Phase 1 — Define safe serializer buffer semantics

**Files:** `fe/fe.c` (748-766), `fe/fe.h` (113-116), `fe/doc/c-api.md`
(313-332), `fe/test_api.c`, `fe/fex_io.c` (82-85).

### Changes

```c
size_t FeToString(FeContext* ctx, FeObject* obj, char* dst, size_t size);
```

- if `size == 0`, perform no writes and permit `dst == NULL`;
- if `size > 0`, require `dst != NULL`, write at most `size - 1`, NUL
  terminate;
- pick **one** return contract and document it.

Two viable contracts, and the choice is not free:

- **(a) bytes stored** (today's contract, minus the underflow). One-line fix:
  guard `size == 0` and return 0. No traversal change, no phase 4 interaction.
  Truncation is then only detectable as `return == size - 1`.
- **(b) snprintf-like required length**, truncation iff `return >= size`. The
  better API, but computing a required length for `size == 0` is an *unbounded*
  traversal on a cyclic object — it recreates the phase 4 hang at a new entry
  point. (b) must land **after** phase 4, or define the measurement as bounded.

Recommendation: ship (a) now as the security fix and add a bounded
`FeMeasureString()` alongside the writer in phase 4, rather than changing
`FeToString`'s return meaning under `FE_API_VERSION 1` (`src/lisp.c:33`
static-asserts that version).

### Caller migration
The return value has exactly **one** in-tree consumer: `FexWriteFile()`
(`fex_io.c:82-85`) feeds it to `fwrite`, so under contract (b) a truncated
render becomes a heap over-read. Every other caller `(void)`-casts it:
`fe/main.c:28,50`, `fe/test_api.c:51`, `fuzz_reader.c:25`, `fuzz_eval.c:259`,
kg `src/lisp.c:860,2035`.

kg is **not** currently exposed to the underflow (`src/lisp.c:2035` is guarded
by `result_size != 0`; `src/lisp.c:860` passes `sizeof(thing)`). Priority here
is public-API hygiene, not a live kg bug — say so in the commit message.

**Tests** (`test_api.c`): `(NULL, 0)`; valid pointer with `size == 0` and
redzones; size 1; exact fit; one short; long truncation; atoms, strings, nested
list; under ASan and MSan.

## Phase 2 — Reject malformed dotted-list syntax

**Files:** `fe/fe.c:912-934`, `fe/doc/language.md`, one new script +
`tests/*.out`/`.err` pair (or `test_api.c` cases), `fe/fuzz/fe.dict`.

### Changes
Extract the `'('` arm into `ReadList()` (pmccabe headroom: `Read` is 18/22) and
give it explicit states:

```text
START -> ELEMENTS -> DOT_SEEN -> TAIL_READ -> REQUIRE_RPAREN
```

Rules:

- dot requires at least one preceding element (rejects `(. a)`);
- exactly one datum follows the dot;
- the next token must be `)` (rejects `(a . b c)` and `(a . b . c)`);
- `(a . b)` stays a valid pair; `.` outside a list stays the symbol `.`
  (current behavior — `ReadAtom` at `fe.c:862-887` interns it), and
  `.5` still reads as a double via `strtod`;
- `(a .)` is *already* an error today, but with the misleading message
  `stray ')'`; give it a real one.

**Correction to the source plan:** it claimed internal `Read()` must be used
"so source offsets/labels remain accurate". That is wrong — offsets come from
the reader callback (`ReadString`, `fe.c:1014-1028`, sets `ctx->error_offset`)
and are unaffected. The actual reason is narrower and still decisive: only the
internal `Read()` can return `&rparen` (`fe.c:909-910`); `FeRead()`
(`fe.c:991-997`) converts it into `stray ')'`. The `REQUIRE_RPAREN` state needs
`Read()`; the dotted tail should keep using `FeRead()` so a `)` there is an
error.

Keep the existing GC discipline (`FeRestoreGC`/`FePushGC` around each element,
`fe.c:930-931`) for the partially built list and the tail.

**Tests:** `(a . b)`; `(. a)`; `(a .)`; `(a . b c)`; `(a . b . c)`; nested proper and
improper lists; whitespace and `;` comments around the dot; `FeReadString`
byte offset on each rejection; reader still usable after a caught error
(`test_api.c` drives this; `main.c`'s REPL is the interactive equivalent).

Add `.`-adjacent tokens to `fuzz/fe.dict`. Do **not** try to add malformed
inputs to `scripts/` — the corpus directory is untracked and a malformed
script would need a golden `.err`.

**Ratchet:** this is the phase most likely to need the reviewed
`SCC_COMPLEXITY_MAX` bump. Do it here, once, justified in the commit message.

## Phase 3 — Remove destructive macro call-site mutation

**Files:** `fe/fe.c:1310-1317`, `fe/doc/language.md:62-103`,
`fe/doc/implementation.md`, `fe/scripts/macros.fe` + goldens,
`fe/fuzz/fuzz_eval.c:140-149`, then kg `README.md`, `doc/kg.1`,
`test/test_lisp.c` after the pin update.

### Changes
Current code:

```c
*obj = *DoList(ctx, CDR(vb), ArgsToEnv(ctx, CAR(vb), arg, CAR(va)));
FeRestoreGC(ctx, gc);
ctx->call_list = CDR(&cl);
return Evaluate(ctx, obj, env, NULL);
```

Replace with: bind raw arguments; evaluate the body to an expansion object;
**push the expansion onto the GC stack across the `FeRestoreGC(ctx, gc)`**
(today's code does not need this because `obj` stays reachable from the caller's
form — the fixed version does); restore `ctx->call_list`; evaluate the
expansion in the caller environment; never copy `*expansion` over `*obj`.

Why it matters, precisely: copying `nil` (`fe.c:126-127`) yields an arena cell
tagged `FeTNil` that `FeIsNil()` (`fe.c:356-358`, address comparison) rejects →
a truthy `nil`; and copying an interned symbol clones it, while `GetBound()`
(`fe.c:828-839`) compares symbol *addresses*, so the clone misses every lexical
binding and silently reads the global cell instead.

The simplest correct behavior expands on **every** invocation. Do not add a
cache in this patch. If later profiling justifies one: cache an `FeObject *`
through a GC-visible wrapper keyed by macro and call-site identity, invalidate
on redefinition, and never mutate quoted or shared source. Cost to note in the
commit message: `(++ i)` inside a `while` now re-expands each iteration, and
each expansion is charged against `EvaluationStep` through `DoList`, so kg's
step budget sees more steps for macro-heavy loops.

Documentation that becomes false: `fe/doc/language.md:62-103` describes and
demonstrates one-expansion-per-call-site as the design; and kg's `README.md`
bullet "A macro expands once per call site … gives a stale answer" must go
after the pin moves.

**Tests.** `fe/scripts/macros.fe` uses only list expansions, so `tests/macros.fe.out`
should be **unchanged** — treat that as the canary. Add:

- macro expanding to `nil`, and `(not (m))` → `t`;
- expansion to `t`, a number, a string;
- expansion to a symbol shadowed by a `lambda` parameter resolving lexically;
- list expansion;
- a shared expansion object left unmutated;
- macro redefinition affecting later calls;
- repeated invocation in a `while` loop;
- expansion error and recovery;
- forced GC between expansion and evaluation (tiny arena, `test_api.c`).

Extend `BuildMacroCall()` (`fuzz/fuzz_eval.c:140-149`) — today it always
generates `(list 'quote x)`, so it cannot reach either atom defect — to emit
`nil`, `t`, symbol and number expansions.

## Phase 4 — Bound and make writer traversal cycle-safe

**Files:** `fe/fe.c:643-766`, `fe/fe.h`, `fe/doc/c-api.md`, `fe/doc/implementation.md`,
`fe/test_api.c`, a new bounded cyclic-graph fuzz target.

**This is a kg availability bug, not just a Fe wart.** kg reaches the writer
from `src/cmd.c:95` (`M-:`), `:111` (`eval-buffer`) and `:542` (`C-j`), each
passing a 512-byte result buffer to `kg_lisp_eval_string`, which renders the
result at `src/lisp.c:2035`. `(progn (setq x (cons 1 nil)) (setcdr x x) x)`
therefore hangs kg in a loop `WriteBuffer` silently discards, with no C-g
escape: `EvaluationStep()` (`fe.c:304-318`) is never reached from `FeWrite`.
kg's `%s`/`%S` in `format`/`message` reach it too (`src/lisp.c:322`).

### Design
```c
typedef struct FeWriteOptions {
    size_t max_bytes;
    size_t max_nodes;
    size_t max_depth;
    bool charge_eval_budget;
} FeWriteOptions;
```

Add `FeWriteWithOptions()`; keep `FeWrite()` as a bounded default wrapper so
`FE_API_VERSION` need not move.

Requirements: no unbounded recursive walk of `car` (bound depth explicitly);
terminate on cdr-spine cycles — tortoise/hare over the spine needs no visited
set and no allocation, and is the smallest thing that works; bound car nesting
by `max_depth` rather than a visited set, so shared acyclic structure is never
misreported as a cycle; stop before exceeding byte/node/depth limits; report
the stop as a status or a documented Fe error; poll the ambient interrupt and
step budget when `charge_eval_budget` and `ctx->evaluation_active`.

Two constraints the source plan missed:

- **`FeWrite` allocates.** The `FeTFn`/`FeTMacro` arms (`fe.c:710-720`) call
  `FeCons` and `FeMakeSymbol`, so printing can collect and raise
  `out of memory` mid-write — which longjmps out of any C caller holding a
  buffer (see phase 8). Either pre-intern `lambda`/`macro` at context open, or
  document that the writer may allocate and may raise.
- **`FeWriteFn` returns `void`** (`fe.h:22`), so "writer callback failure must
  propagate" is not expressible today; `WriteFile` (`fe.c:740-742`) already
  drops `fputc` failures, so `FeWriteFile` silently swallows I/O errors. Accept
  that and drop the requirement, or add a status-returning callback in an API
  v2 — do not pretend v1 can do it.

Define the spelling for a truncated/cyclic render (`#<cycle>` is fine if reader
round-trip is not promised) and keep it stable: it becomes golden-file text.

`FeMark` (`fe.c:379-419`) has the same recursive-`car` shape and is a
documented known issue; fixing it is *not* in this phase, but note the
duplication so the two do not drift.

**Tests:** self cdr cycle; two-node cycle; cycle through `car`; shared acyclic tail
printed in full; deep car nesting at `max_depth ± 1`; byte/node boundaries
± 1; interrupt callback fires mid-render; context reusable after a writer
error; and, after the pin update, a kg PTY case that evaluates a self-cycle at
`M-:` and expects a diagnostic rather than a hang.

New fuzz target: build object graphs through the public API with strict node
bounds. `fuzz_eval.c` deliberately excludes cycles, so extend `fe/doc/FUZZING.md`
alongside it.

## Phase 5 — Validate lambda/macro parameter descriptors and arity

**Files:** `fe/fe.c:1092-1107` (`ArgsToEnv`), `fe.c:1170-1177` (closure construction),
`fe.c:1303-1317` (dispatch), `fe/doc/language.md`, scripts + goldens; then kg
`src/lisp.c:1839-1861`, `test/test_lisp.c`, `README.md:264`, `doc/kg.1`.

### Changes
Validate parameter forms when the closure is built (`PFn`/`PMacro`), not only
at call time: required symbols, Fe's dotted and bare-symbol rest syntax, no
non-symbol parameters (`((lambda (1) 5) 2)` currently returns `5`), and a
decision on duplicates. Record required count, optional count and rest symbol
in a compact descriptor, or validate through one shared binder if changing the
object layout is deferred.

At call: too few required or too many without rest →
`wrong-number-of-arguments`; missing optional → canonical `nil`; rest receives
the remaining list; the macro binder follows the same rules on raw forms;
native functions keep using `FeGetNextArgument`/`FeRequireNoArguments`.

**Strongly recommended addition:** teach the C binder `&optional` and `&rest`
markers directly. Without them, kg cannot enable strict arity at all (below).

### kg compatibility — this phase breaks kg unless sequenced
`internal--arglist` (`src/lisp.c:1839-1861`) *deletes* `&optional` and its own
comment says why: "missing arguments already bind to nil and extra ones are
already dropped". So `(a &optional b)` lowers to the two-required list `(a b)`
and depends on the lax binder. Concretely broken by strict arity:

- `test/test_lisp.c:1341-1343`: `(defun opt (a &optional b) …)`, `(opt 1)` →
  `(1 nil)`;
- `test/pty/lisp-defun-interactive.yaml`: `(defun greet (&optional who) …)`;
- `src/lisp.c:1578`: `FeCall(context, state.pending_command, nullptr, 0)` —
  every interactive command is invoked with **zero** arguments, so any
  `(defun c (&optional x) (interactive) …)` becomes an arity error;
- `README.md:264` and `doc/kg.1:139-141,194,289-291,329-331` document
  "a missing argument is `nil`".

Order of operations: land `&optional`/`&rest` in Fe's binder → bump the pin →
shrink `internal--arglist` toward identity → *then* enable strict arity.
`&rest` needs nothing: `(a &rest r)` already lowers to `(a . r)` and
`ArgsToEnv`'s dotted arm binds `nil` correctly (`test_lisp.c:1345-1346`).

Gate the strict semantics behind a Fe context option or an explicitly versioned
semantic bump, and run Fe's whole script suite in both modes for one transition
release.

## Phase 6 — Add an internal unbound sentinel

**Files:** `fe/fe.c:571-585` (interning), `828-843` (`GetBound`/`FeSet`), `1273-1275`
(symbol evaluation), `1143-1144` (`env`), `fe/fe.h` for the public predicates,
language/API/implementation docs, scripts; then kg prelude, `README.md:287`,
`doc/kg.1`, `test/test_lisp.c:1361-1368`.

### Changes
Create one private immortal `unbound` object, not constructible from Lisp and
not a symbol.

- initialize new symbol value cells to `unbound` (today `FeMakeSymbol` conses
  `(name)` so the value reads as `nil`);
- make lexical/global lookup return `(found, value)` internally;
- evaluating an unbound symbol raises `void-variable NAME`, matching the
  existing `void-function NAME` shape (`fe.c:1257-1266`);
- assignment via `=`/`FeSet` creates or updates the binding, unchanged;
- add `FeIsBound()` plus Lisp `boundp` and `makunbound`;
- canonical `nil` stays a legitimate bound value.

Audit, because the sentinel must not escape: `(env)` returns `ctx->symbol_list`
verbatim (`fe.c:1143-1144`); the writer must never print it; GC must mark it
(or make it immortal like `nil`); primitive and math installation runs through
`FeSet`/`FeDefineNative`; `t` stays an ordinary assignable global. Fe's own
`TODO.md` already records the underlying complaint ("naming a non-existent
variable creates it in the global env, but should not") — cite it; this is
upstream agreement, not a kg-only preference.

### kg compatibility
kg's `defvar` (`src/lisp.c:1894-1899`) is
`(if name (quote nil) (setq name (car rest)))` — it *evaluates* `name`, which
under this phase raises `void-variable`. It must become
`(if (boundp 'name) nil (setq name …))` in the same kg commit that bumps the
pin. Then delete the `README.md:287` caveat and the matching `doc/kg.1` note,
and update `test/test_lisp.c:1361-1368` so that `(setq dv nil)` followed by
`(defvar dv 9)` keeps `nil`.

Add a migration note: unknown variables previously evaluated to `nil`, so a
typo that was silently false now raises.

## Phase 7 — Fix Fex file ownership

**Files:** `fe/fex.h`, `fe/fex.c:12-40`, `fe/fex_io.c`, scripts
(`scripts/io.fe` + goldens), `test_api.c`, docs.

**Current state (verified).** `FexTFile` is a raw `FILE*` in an `FeMakePtr`
cell, and `FexGC` returns `&nil` for it, so there is **no finalizer**: unclosed
files leak descriptors for the process lifetime. `FexCloseFile` `fclose`s the
raw pointer with no closed flag, so double close is undefined behavior and
use-after-close is unchecked. `stdin`,
`stdout` and `stderr` are bound as ordinary `FexTFile` values
(`fex_io.c:29-31`), so `(close-file stdout)` closes the host's stdout.

### Changes
```c
struct FexFile { FILE* fp; bool owned; bool closed; };
```

Every file native validates type and live state, obtains `fp` through one
helper, marks closed in a failure-safe order around `fclose`, defines whether
repeated close is idempotent or an error, and never closes the standard streams
unless explicitly allowed; the finalizer closes only owned, live files exactly
once. Two allocation hazards to design for: the Fe cell stores one `void*`, so
`struct FexFile` must be heap-allocated, and `FeMakePtr` can itself collect and
raise before ownership transfers, leaking it — the smallest concrete motivation
for phase 9; and `FexGC` runs on *every* swept object (`fe.c:437-440`), so the
finalizer must be cheap, non-reentrant, and must not allocate Fe objects.

`FexInit` also mutates the process-global `type_names` array (`fex.c:37-38`) —
the legacy limitation documented at `fe/doc/c-api.md:333-347`; do not deepen
it, phase 10 removes it.

**Tests:** double close; read/write after close; GC of many unclosed files
under a low `RLIMIT_NOFILE`; standard streams surviving collection; `fclose`
failure; exactly-once finalization at `FeCloseContext`.

## Phase 8 — Remove Fex truncation and process lifecycle gaps

**Files:** `fe/fex_io.c`, `fe/fex_process.c`, a shared exact-string helper,
scripts/tests.

### Verified defects
- `FexOpenFile`/`FexRemoveFile` (`fex_io.c:40-49,68-73`): paths rendered via
  `FeToString` into `char[PATH_MAX + 1]`, silently truncated; mode into
  `char[8]`.
- `FexReadFile` (`fex_io.c:51-66`): uses only `delimiter[0]` and renders
  non-strings through the writer, so `nil` becomes the delimiter `'n'`.
- `FexWriteFile` (`fex_io.c:75-91`): mallocs 4 MiB per call, truncates at
  4 MiB − 1, reads `errno` even on success, and passes `FeToString`'s return as
  the byte count — the one place that contract is load-bearing (phase 1).
- `FexExecute` (`fex_process.c:25-70`): `MaxArgumentCount = 31` and the loop
  `break`s silently, so argument 32 on is dropped with no error; each argument
  is truncated into `char string[1024]`; `waitpid` (line 61) is not retried on
  `EINTR`.

### Changes
Add one helper built on the APIs that already exist —
`FeStringByteLength()` and `FeCopyStringBytes()` (`fe.c:799-814`), which are
exact, byte-counted and NUL-free, unlike `FeToString`. The helper: requires a
string (or an explicitly accepted symbol); takes the exact byte length;
checked-adds the NUL; allocates exact storage; copies; registers cleanup until
ownership transfers. Use it for paths, mode, delimiter, argv, and writes.

For `FexExecute`: allocate argv dynamically with a documented maximum, or
reject above a fixed maximum *explicitly*; never drop arguments silently; free
every partial allocation on error; retry `waitpid` on `EINTR`; report exit
versus signal deliberately rather than folding both into one double. For
writes: write exact bytes rather than printer text unless the API says "write
the printed object"; loop on partial writes; surface short writes and errors;
distinguish EOF from `ferror()` and from stale `errno`.

Cross-phase hazard: `FexWriteFile` holds a `malloc`'d buffer across a call that
can raise (`FeToString` → `FeWrite` allocates for closures, phase 4), and
`FeHandleError` does not return — the buffer leaks. That is the error-injection
case phase 9 must cover.

## Phase 9 — Design one cleanup/unwind system

**Do not code language conditions before this design is reviewed.** Overlapping
needs: C heap/fd cleanup across host `longjmp`; Lisp `unwind-protect`;
`condition-case`, `catch`/`throw`, and quit; kg temporary buffer/root/resource
restoration. `fe/AGENTS.md` already states the binding
constraint: `FeHandleError` does not return, and `auto.[ch]`'s
`cleanup`-attribute helpers explicitly do **not** survive a `longjmp`.

Write `fe/doc/unwind-design.md` covering: completion kinds (normal, signal,
throw, quit); arena-backed evaluator unwind records; non-allocating C cleanup
records; LIFO ordering between Lisp and C cleanups; ownership transfer and
cancellation; nested evaluation and native re-entry; what reaches the host
error callback; interaction with GC checkpoints and evaluation budgets.

An interim C-only cleanup stack may land first:

```c
FeCleanupCheckpoint FeSaveCleanups(FeContext*);
FeCleanupToken FeDeferCleanup(FeContext*, FeCleanupFn, void*);
void FeCancelCleanup(FeContext*, FeCleanupToken);
```

but it must be explicitly compatible with the eventual completion engine, and
must land with an error-injection matrix before Fex or kg resources migrate.

kg does **not** need this today: kg's cleanup runs after `setjmp` returns
(`src/lisp.c:2018-2025` restores the GC checkpoint and calls
`release_frame_buffers()`). Phase 9 serves standalone Fe and Fex; rank it below
phases 1–6.

## Phase 10 — Per-context extension descriptors

Depends on phase 9's cleanup/finalization semantics. Note that Fe's own
documentation already calls this work "Phase 8" (`fe/doc/c-api.md:338,344`) —
use Fe's number inside the submodule and cross-reference this plan's, or the
two schemes collide in commit messages.

### API
```c
struct FeUserTypeDescriptor {
    const char* name;
    FeUserMarkFn* mark;
    FeUserFinalizeFn* finalize;
    FeUserWriteFn* write;
    FeUserEqualFn* equal;
};
```

Registration returns a context-local opaque handle. One core external-object
tag references descriptor + payload. Validate context and handle on every
construction and access. Native descriptors may additionally carry name, arity,
documentation, and re-entry/allocation flags. This replaces the process-global
`FeTFex0..2` tags and the mutable `type_names` array `FexInit` writes into.

**kg impact: none today.** kg never calls `FeMakePtr`, `FeSetMarkFn`,
`FeSetGCFn` or `FeMark` — verified across `src/lisp.c`, the only file allowed
to include `fe.h`. This is enabling work for future kg C-owned objects
(buffers, processes, markers), not a fix. Keep it last.

Tests: two contexts with different registrations; multiple types; marking
back-references into Fe objects; printing and equality; invalid and foreign
handles; exactly-once finalization on GC and on `FeCloseContext`; API v1
compatibility wrappers still working for in-tree Fex.

## Forced-GC/error test matrix

Before richer kg Lisp: sweep arena sizes upward from `FeMinimumArenaSize()`
(currently 36608 bytes after the `GcStackSize` 512 → 4096 divergence, per
`doc/fe-upstream.md`); force collection between object-producing operations;
root, release and reuse objects; re-enter through native callbacks; inject an
error at each allocation position; confirm the host restores its GC checkpoint
and the context stays reusable; confirm external resources finalize once.

Add debug counters for allocations, collections, live/free slots, max GC stack
depth, symbol comparisons, and evaluator steps. kg's minimum viable
`KG_LISP_ARENA_SIZE` is already ~68 KiB, so a per-context descriptor table or a
wider object header moves that floor — re-measure and update
`doc/fe-upstream.md` when it does.

## Final verification

For each Fe submodule commit, on `analyzers-etc`:

```sh
make -C fe check
make -C fe fuzz-smoke
(cd fe && .ci/run-ci-steps.sh)
```

After the kg pin update:

```sh
make check
make WITH_LISP=0 clean all check
.ci/run-ci-steps.sh --parallel
```

Root CI does not run `make -C fe check` today, so the Fe run above is manual
until a numbered root stage exists (see plan 07).

Never combine macro, reader, writer, arity and unbound semantic changes in one
commit. Each needs its own compatibility note in `doc/fe-upstream.md`, its own
golden-file churn, and its own focused regression.
