# Fe upstream

kg embeds the core of [Fe](https://github.com/bjodah/fe) through the `fe/` git
submodule. The submodule tracks the **`more-elisp` branch** of
`github.com:bjodah/fe`; that branch name is the pin. The exact commit is
recorded automatically by git as part of `fe/` being a submodule — the
superproject's tree stores the SHA the working tree is checked out at, and
`git submodule status` prints it — so no commit hash is repeated here. A hash
written into prose only goes stale, as it did before this document was
rewritten.

The supported embedding interface is `FE_API_VERSION 13`; `src/lisp_core.c`
asserts it at compile time. Fe's *language* — its evaluated behaviour,
independent of the C embedding contract — is versioned separately as
`FE_LANGUAGE_VERSION 15`, which `src/lisp_core.c` also asserts at compile
time, beside the API assertion. The two move independently: language
version 2 was the `setq`/`set`/numeric-`=` hard cut below, which broke no
C function, type, or callback contract, so `FE_API_VERSION` stayed at 1
through it; `FE_API_VERSION` later moved 1 → 2 for sub-plan 03F's
`FeEvalOptions`/`FeArenaStats` rename below, a C-contract break with no
language-behaviour change of its own, so `FE_LANGUAGE_VERSION` did not
move with it. Both moved 2 → 3 together in sub-plan 04D's Lisp-2
namespace cut below, which changed a C contract *and* a language
behaviour in the same slice: `FeDefineNative`'s target cell and the
call-position/`#'` rules moved together. Both moved 3 → 4 together in
sub-plan 05D's numeric cut below, which changed a C contract (the new
`FeTInteger` object type and its `FeMakeInteger`/`FeToInteger` accessors)
*and* a language behaviour (integer literals, `eq`/`eql`, the widened
comparison family) in the same slice. Both moved 4 → 5 together in
sub-plan 06D's condition-object cut below, for the same reason once more:
`condition-case`/`signal`/`error` changed what programs mean, and the
completion accessors changed the C contract, in one slice. Both moved
5 → 6 together in sub-plan 07B's strict-arity cut below, for that same
reason a third time: the C break is a *removal* — `FeSetStrictArity()`
and `FeGetStrictArity()` are gone, so the compile error is the whole
notification — and the language break is that `((lambda (x) x))`,
`((lambda () 1) 2)`, `(car 1 2)` and `(quote 1 2)` used to answer and now
raise, `(and)` was nil and is `t`, and a malformed parameter list raises
`invalid-function`. `FE_LANGUAGE_VERSION` then moved 6 → **7** alone, in
Phase 8's constants/keywords and strict-reader pair below: `t`, `nil` and
keywords stopped being assignable, keywords self-evaluate, and a program
that read before may no longer read — all language, no C contract, so
`FE_API_VERSION` stayed at **6**. Phase 9 moved **neither**, and the row
that records it says why against this same contract: making arena
exhaustion and GC-root-stack overflow raise the two condition objects
`FeOpenContext` now pre-builds adds no C declaration and no language
surface — both names were already rows of the hierarchy — and repairing a
narrower instance of that same nil-condition substitution moved no version
in Phase 7 either. `FE_LANGUAGE_VERSION` then moved 7 → **8** alone again,
in Phase 10's `macroexpand`/`macroexpand-1` pair below, and that one is
the series' only bump that is *not* a break: no program that ran under 7
answers differently under 8, because the three names it adds
(`macroexpand-1`, `macroexpand`, and `macroexpand-all`'s
reject-by-name stub) answered `void-function` before. fe's own commit
records both sides of that decision — against it, `doc/c-api.md`'s
"compatible additions do not require a bump", which 04C's seven new
primitives were landed under; for it, that the macro's single consumer is
kg's compile-time `static_assert`, and a macro that does not move cannot
tell kg whether the fe it links against has the names Proof 3 reflects
with. kg records the same verdict here rather than re-deciding it:
the bump is what made the tripwire fire at this pin, which is the
behaviour the two-macro scheme exists for, and the alternative was
finding out at run time as `void-function`. `FE_API_VERSION` stayed at
**6** for it — `fe.h`'s diff over the range is comment text only.
Phase 11 then moved **both** again, in two fe slices under one pin:
`FE_LANGUAGE_VERSION` 8 → **9** in sub-plan 11B's special-variable and
shallow-dynamic-binding cut and 11C's quote-writer change below (evaluated
answers move — `(let ((v 2)) (f))` over a `defvar`'d `v` reads 2 where it
read the global before — and so does printed representation, `(quote x)`
now writing as `'x`), and `FE_API_VERSION` 6 → **7** in 11C for the new
public entry point `FeTryEvaluateStringWithOptions`, the protected *string*
evaluation kg's loader seam is built on. Both assertions in
`src/lisp_core.c` fired at this pin, which is the behaviour the two-macro
scheme exists for. `FeVersion` is `"10.0"`, bumped once for the pair — it
is Fe's own next release, not a language-version mirror, so it has run one
release ahead of `FE_LANGUAGE_VERSION` since 06D and, Phase 11 having moved
the API macro again, three ahead of `FE_API_VERSION`.
Phase 12 moved `FE_LANGUAGE_VERSION` 9 → **10** alone, in fe sub-plans 12B
and 12C below, and `FE_API_VERSION` stayed at **7** — no declaration in
`fe.h` changed, only comment text and the two data lines the hierarchy and
the primitive table gained. The language bump is a break in the same sense
version 9 was: a condition handler established *inside* an
`unwind-protect` cleanup is now honored where every raise inside a running
cleanup used to behave as unhandled, and `eval` exists where the name
answered `void-function`. `FeVersion` is `"11.0"`.
Phase 12's **fix cycle** then moved `FE_API_VERSION` 7 → **8** alone, at
the phase's second pin (the fix-cycle exception to the one-pin rule): the
input-unit trio `FeEnterInputUnit`/`FeReadInputForm`/`FeLeaveInputUnit`
with the public `FeInputUnit` token, the entries kg's prelude `load` loop
is built on. No language behaviour changed with it, so
`FE_LANGUAGE_VERSION` stays **10**, and `FeVersion` stays `"11.0"` — the
fix cycle is the same phase, so fe's `doc/c-api.md` records that release
11.0 moves both macros. With the API at 8, `FeVersion` has run three
ahead of `FE_API_VERSION` since Phase 11 and still does.
Phase 13 moved `FE_LANGUAGE_VERSION` 10 → **11** alone, in fe's
funcall-classification repair below, and `FE_API_VERSION` stayed at **8** —
no declaration in `fe.h` changed, only comment text and three data lines in
`fe_eval.c`'s `primitive_is_function[]`. The change is that `signal`,
`error` and `keywordp` are reachable through `funcall`/`apply` at all: all
three evaluate every operand and are ordinary functions in Emacs, but a
missing row made `IsRawFormCallable()` treat them as special forms, so
`(funcall 'signal 'error '("x"))`, `(apply 'error '("boom"))` and
`(mapcar 'keywordp '(:a 1))` answered `invalid-function` in `fe/fe` and,
identically, in kg. Like the version 8 bump this is *not* a break — no
program that ran under 10 answers differently under 11, because every
affected program raised — and it is a bump for version 8's reason, which
this table records rather than re-decides: kg's compile-time
`static_assert` is the macro's only consumer, and `signal` reachable
through the two entry points a prelude's higher-order functions are built
on is exactly the kind of thing a version that does not move cannot report.
The assertion in `src/lisp_core.c` fired at this pin, which is the
behaviour the two-macro scheme exists for. `FeVersion` is `"12.0"`.
Phase 14 moved `FE_LANGUAGE_VERSION` 11 → **12** alone, in fe's symbol
surface below, and `FE_API_VERSION` stayed at **8** — no declaration in
`fe.h` changed, only comment text. Unlike the 8 and 11 bumps this one is a
*break*, and in three directions rather than one. New names: `intern`,
`intern-soft`, `symbol-name`, `make-symbol`, `gensym`, `put`, `get` and
`symbol-plist`, all ordinary functions, all `void-function` before. The
reader: a backslash in a token was a named read error and is now Emacs'
symbol escape, so `(a\ b)` is a one-element list where it was a
diagnostic, `\1` is the symbol `1`, an escaped `\.` inside a list is an
ordinary element rather than the dotted-tail marker, and `##` is the
empty-name symbol. The writer: a symbol whose name would otherwise read
back as something else prints with escapes, so the symbol `.` prints `\.`
and `(intern "a b")` prints `a\ b` — which is what makes the reader change
safe, the two being inverses. One behaviour outside those three moved with
them, and it is the kind a version that did not move could not report:
`keywordp` now asks whether the interner self-bound the name rather than
only whether it starts with a colon, so `(keywordp (make-symbol ":a"))` is
nil as it is on Emacs 31.0.90. The object layout moved too, invisibly to
any host: a symbol's `cdr` chain widened from `((name . function) . value)`
to `(((name . plist) . function) . value)`, one cons per symbol, so
`FeMinimumArenaSize()` rose 58080 → 60344 bytes and kg's 1 MiB arena
re-partitioned from 56225 objects / 1095 frames to **56239 / 1093** — the
object count rising as the frame count falls, because the partition moves
bytes rather than slots. The property list lives in the symbol rather than
in a context-side registry because an uninterned symbol is the first symbol
fe has that the collector may reclaim, and a registry keyed by symbol would
pin every symbol that ever carried a property. The assertion in
`src/lisp_core.c` fired at this pin. `FeVersion` is `"13.0"`.
Phase 18 moved `FE_API_VERSION` 8 → **9** alone, and
`FE_LANGUAGE_VERSION` stayed at **12** — no name, no reader syntax and no
evaluation rule changed, so nothing a Lisp program can observe moved, and
`FeVersion` stays `"13.0"`. Two declarations are added to `fe.h`:
`FeGetValue`, the read half of `FeSet`, and `FeMakeUnbound`, the C
spelling of what `makunbound` does to a global binding. Both address the
GLOBAL binding and never an environment entry, which is the rule
`FeSet`/`FeIsBound` already live under. `FeGetValue` answers `nullptr`
rather than `nil` for an unbound name, because the caller these exist for
takes a value out of the cell and puts it back later and has to tell "no
value" from "the value nil"; `&unbound` is private and no API returns it.
`FeMakeUnbound` reuses `FeSet`'s constant check rather than inventing a
second policy, so it raises `setting-constant` for `t`, `nil` and
keywords and leaves the binding alone.

Phase 19 moved **both**, in one fe commit: `FE_LANGUAGE_VERSION` 12 → **13**
and `FE_API_VERSION` 9 → **10**, with `FeVersion` "13.0" → "14.0". The
language bump is a break in three directions, all of them about what a
condition READS as. `error-message-string` is a new name that answered
`void-function`. Every condition symbol in the standard hierarchy now
carries the `error-message` property Emacs gives it, seeded when the context
opens, so `(get 'wrong-type-argument 'error-message)` is `"Wrong type
argument"` where it was nil — and a raise of that condition reports
`Wrong type argument: listp, 6` where it reported the bare symbol. And the
writer escapes a backslash inside a printed string, so `(format "%S" "a\\b")`
is `"a\\b"` where it was `"a\b"` — the last printed form in fe that did not
read back, recorded in `doc/TODO.md` since Phase 14 with "whichever phase
next moves the fe pin for the writer" as its stated home. This is that
phase, because `error-message-string` prints its data items with `prin1`.
The API bump is the one new declaration, `FeErrorMessageString`: the same
rendering from C, for the one caller the Lisp primitive cannot serve — a
host inside its `FeSetErrorFn` callback, where calling Lisp is exactly what
must not happen. It allocates nothing and suspends the step budget across
the render, because printing is charged work and both the budget and the
interrupt poll raise. Both assertions in `src/lisp_core.c` fired at this
pin.

Two consequences of the seeding are worth recording beside the version
decision, because both are the kind that a reader would otherwise
rediscover. `FeMinimumArenaSize()` rose 60344 → **63592 bytes**, and kg's
1 MiB arena re-partitioned from 56239 objects / 1093 frames to
**56259 / 1090** — the object count rising again as the frame count falls,
for Phase 14's reason. And fe's fuzz harness arena moved 64 → 68 KiB, which
is not an enlargement but the opposite: what steers that lane is the FREE
portion, which the seeding had cut from 292 slots to 109, and two tracked
seeds stopped reaching their shapes because their churn form now exhausted
the arena rather than because the grammar moved. Restoring the free portion
restored all fifteen with no seed re-derived.

Phase 20 moved `FE_LANGUAGE_VERSION` 13 → **14** alone, with `FeVersion`
"14.0" → "15.0"; `FE_API_VERSION` stays at **10** because `fe.h`'s diff
over the range is comment text only. Two additions under one bump, neither
of them a break — no program that ran under 13 answers differently under
14. `string<` and `string>` are new names that answered `void-function`:
Emacs' lexicographic string order, taking a string or a symbol on either
side. And `end-of-buffer` and `beginning-of-buffer` join the condition
hierarchy as children of `error`, so `(signal 'end-of-buffer nil)` is legal
where it raised `Invalid error symbol`, `(get 'end-of-buffer
'error-message)` answers `"End of buffer"` where it answered nil, and an
`error` handler catches either. The bump follows Phase 10's precedent, the
one this table already records for an addition-only change: kg's
compile-time `static_assert` is the macro's only consumer, and a macro that
does not move cannot tell kg whether the fe it links against has the names
kg's natives now raise.

The same pin carries a translation-unit split that is *not* a version move
of any kind, and is recorded here because kg's build has to know about it:
fe_eval.c reached 519 of its 520 per-file complexity cap at the Phase 19
pin, and the completion machinery — evaluation control, the condition
hierarchy and its handler search, the cleanup registry, every raise — moved
to a new **fe_unwind.c**. No code changed; eight functions became
non-static across the seam. kg compiles fe's core sources directly, so
`Makefile`'s `FE_OBJ`, `FUZZ_FE_OBJ`, the GC-stress link and the
missing-source guard each gain the fourth name.

Arena arithmetic at this pin, measured and not carried forward:
`FeMinimumArenaSize()` rose 63592 → **64200 bytes** (+608: two primitive
objects with their symbols, two condition symbols with their message
strings and plist pairs), and kg's 1 MiB arena re-partitioned from 56259
objects / 1090 frames to **56263 / 1089** — one frame slot's worth of bytes
moving to the object side, the same trade Phases 14 and 19 made. `peak_live`
after the prelude is **11335**, against 11281 at the merge that opened this
phase.

The caller is kg's Phase 18 buffer-local storage, and the reason it needs
fe at all is worth recording, because the plan's constraint was that fe
grows *at most* a hook: kg keeps Emacs' representation, one value cell per
symbol holding whichever buffer's binding is current with the displaced
one stashed beside it, so a variable reference inside fe's evaluator stays
exactly one cell read and fe learns nothing about buffers. What kg needs
from fe is only the ability to read and unbind that cell from C. Before
this the only way to read it was to evaluate `(symbol-value 'x)` — a
nested run, a step budget and a catchable raise, for a two-word load, and
on a path (a hook's execution-context restore) that must not raise at all.

The kg side of that phase carries one change of its own that is not fe's:
`save-excursion` and `with-current-buffer` bind their captured state to a
`(gensym)` instead of to the ordinary symbol `internal--excursion`, which
is Phase 14's hygiene demonstration and closed `doc/TODO.md`'s sharpest
instance of the capturable-temporary row.  The rest of that row was swept
later and needed no fe change at all: a function body has no expansion to
mint a `gensym` in, and paying for one at load time measured out against
the arena, so those temporaries became lambda parameters — fe's existing
unconditional-lexical binding kind — instead.

Phase 18's **follow-up** moved `FE_API_VERSION` 10 → **11** alone, with
`FeVersion` "15.0" → "16.0"; `FE_LANGUAGE_VERSION` stays at **14**. The
language macro not moving is a decision and not an oversight: no program
that ran under 15.0 answers differently under 16.0 unless its host installs
the new callbacks, fe's own `fe` binary installs neither, and fe's script
suite and its 427-case compat corpus are byte-identical across the change.
There is no new name for kg to reflect with either, which is what Phase
10's precedent (bump for an addition-only *language* change) is about. What
moved is a C contract, so the C macro moved.

The contract is the **dynamic-binding location seam**: `FeSetBindingFns`
installs two callbacks, one asked for an opaque tag as a shallow dynamic
binding is pushed — before the value cell is read — and one asked at the
matching restore for the symbol whose value cell receives the saved value,
or `nullptr` to drop it. fe stores the tag in its `FeCleanupBinding` entry
and never interprets it: not an `FeObject*`, not marked, never freed. This
is the smallest thing that closes Phase 18's two recorded divergences, and
it closes them without teaching fe what a buffer is — the constraint that
phase worked under and the reason it left them open. kg's `let` over a
buffer-local name now carries the same thing Emacs' specpdl carries (its
`SPECPDL_LET_LOCAL` and the `where` field), and `src/lisp_locals.c` answers
both callbacks out of the table it already keeps: zero for "the cell held
the default", the binding's own identity otherwise, resolved back to a cell
at the restore, or to nothing when that binding has died with its buffer or
been killed. `phase18-let-buffer-switched-out` and
`phase18-make-local-while-let-bound` are `supported` rows now, and
`lettag-let-binding-buffer-tag` pins the five interactions around them.

Two measured consequences, neither carried forward. `FeMinimumArenaSize()`
rose 64200 → **66264 bytes** — the context lives in the arena and the
cleanup stack is 256 entries, so one pointer each is 2 KiB — and kg's 1 MiB
arena re-partitioned from 56263 objects / 1089 frames to **56147 / 1087**,
the frame side paying for it this time rather than the object side. And
fe's own fuzz-harness arena moved 68 → 70 KiB for Phase 19's reason, not as
an enlargement: the FREE portion is what steers that lane, the growth cut it
from ~339 slots to ~210, and `strict-arity-rest` stopped reaching
`(x &rest y)` until the free portion was restored. `peak_live` after kg's
prelude is **11339** here; the 11335 recorded at the Phase 20 pin above
predates this branch's prelude hygiene sweep.

The same pin carries fe's `FE_GC_STRESS` build knob, which moves neither
macro on purpose: it is a compile-time define inside `fe.c`, defaulting to
0 and compiling to nothing there, with no declaration in `fe.h` and no
effect on any program's answer — the same standard the Phase 9 row above
applies to a change with no C declaration and no language surface. kg
builds one object with it (`test/fe_gcstress.o`, linked into
`test/kgbatch-gcstress`, the `make lisp-gc-stress-check` lane); the shipped
editor's `src/fe.o` is built without it, exactly as `test/perfobj/` keeps
the counting build out of `src/`.

The pin then moved once more for kg's embedded-prelude program
(`doc/plans/2026-08-14-embedded-prelude.md`'s "Post-prelude collect" work,
which the plan itself says sits outside its numbered phases):
`FE_API_VERSION` 11 → **12** alone, `FE_LANGUAGE_VERSION` staying at **14**.
The addition is `FeCollectGarbage`, a thin public wrapper — declared in
`fe.h`, defined in one line in `fe.c` — around the `static` `CollectGarbage`
that forces an immediate mark-and-sweep, the same one
`ArenaCanAllocate()`/`MakeObject()`'s exhaustion path and the `FE_GC_STRESS`
build already run on their own schedule. Nothing removed, nothing an
existing call's meaning changes, so every host that links against this pin
keeps compiling unmodified; the bump exists for the reason versions 7, 8, 11
and 20 above already used for an addition-only change — kg's compile-time
`static_assert` is the macro's only consumer, and a macro that does not move
cannot tell kg whether the fe it links against has the new entry point at
all. Nothing a Lisp program evaluates can observe whether or when a
collection ran, beyond the arena not running out, so the language macro does
not move with it. The assertion in `src/lisp_core.c` fired at this pin.
`FeVersion` is `"17.0"`.

kg's own use of the new entry point is the one call `kg_lisp_init()` makes,
once, right after `evaluate_prelude()` and the `FeRestoreGC()` that
follows it — root safety depends on that order, since the collection has to
run *after* the GC stack is back at its post-setup checkpoint or it would
find the prelude's own transient garbage still rooted by the raw stack and
reclaim none of it. The measured yield is the prelude's own collectable
footprint (~860 slots at the Phase 2 pin this document's Phase 3 section
records, unchanged at this one): `peak_live_objects` and
`reachable_live_objects` read exactly what they did before this pin moved,
by construction — the former is a high-water mark already reached by the
time the call runs, the latter is by definition the post-collection figure —
so this pin moves neither the arena partition (`FeMinimumArenaSize()` adds
no field to `FeContext`, so `total_slots` stays 56147) nor either census
number; only a *new* reading, live slots at the moment `kg_lisp_init()`
returns, shows the change did anything. See
`doc/plans/2026-08-14-embedded-prelude.md`'s "Post-prelude collect —
results" for the full measurement, the root-safety audit, and what it does
not buy.

The pin then moved for the cheap-compatibility phase of
`doc/plans/2026-08-19-fe-simplification-and-cheap-compat.md` (fe `cbcea0e`):
`FE_LANGUAGE_VERSION` 14 → **15** and `FeVersion` "17.0" → **"18.0"**, with
`FE_API_VERSION` staying at **12** — `fe.h`'s diff over the range is the
language macro and comment text. The form feed is reader whitespace now; the
divergence row below is where that change is recorded, and it is a break in
one narrow direction only, a symbol whose name holds a literal form feed no
longer reading back as itself unless the byte is escaped. The printer has
escaped that byte since language version 12, so the round trip still closes.
The assertion in `src/lisp_core.c` fired at this pin. No arena figure moves
with it: a `#define` and four `strchr` arguments add no object, no symbol and
no field to `FeContext`, so `FeMinimumArenaSize()` is unchanged and kg's
1 MiB arena still partitions to **56147 object slots** (re-measured through
`test/kgbatch -a` at this pin, not carried).

The pin then moved for section 23.0 of
`doc/plans/2026-08-19-elisp-data-model-phase23-execution.md`, the entry gate
for the payload substrate the Phase 22 ADR selected (fe `a219b14`). Neither
macro moves: `FE_API_VERSION` stays at **12**, `FE_LANGUAGE_VERSION` at
**15**, and `FeVersion` at **"18.0"** — `fe.h` is untouched by the range. What
lands is a tracked census (`fe/doc/payload-pointer-census.md`), the one
publish protocol stated in `fe/fe_internal.h` beside `STRING_BUFFER`, and
fe's second build knob in the `FE_GC_STRESS` family,
**`FE_DEBUG_PAYLOAD_MOVE`**, which the paragraph above's standard covers
exactly: a compile-time define defaulting to 0, compiling to nothing there,
with no declaration in `fe.h` and no effect on any program's answer. fe's
`.ci/ci-04-clang-asan-ubsan.sh` is the lane that arms it; kg builds nothing
with it, unlike `FE_GC_STRESS`, because no live object owns a payload yet and
the knob's only caller is fe's own `test_api.c`.

The default build is unchanged and was proved so rather than inspected:
`fe.c`, `fe_eval.c`, `fe_run.c` and `fe_unwind.c` compiled at both ends of
the range with identical flags and `-DNDEBUG` produce byte-identical objects,
and without `-DNDEBUG` the objects have identical section sizes and differ
only in the `__LINE__` constants `assert` embeds. So no arena figure moves
either: no object, no symbol and no `FeContext` field is added, so
`FeMinimumArenaSize()` is unchanged and kg's 1 MiB arena still partitions to
**56147 object slots**. Thirteen of the census's rows are kg's own
`src/lisp_*.c`, and the finding they record is that kg holds no interior
pointer into fe storage at all — `fe.h` returns none, and `copy_fe_string()`
and every `FeToString` caller copy bytes into host memory. The one row that
constrains kg going forward is `src/lisp_io.c`'s `FeWriteFn`: fe's printer
holds a payload pointer across that callback, and kg's callback must keep
growing a host buffer rather than allocating an fe object.

The pin then moved for section 23.1 of the same plan, where the substrate
itself lands (fe `b208d66`..`44efb18`). Neither macro moves here either:
`FE_API_VERSION` stays at **12** — `fe.h` is untouched again, and the bump
belongs to 23.2's commit, the one that extends `FeArenaStats` —
`FE_LANGUAGE_VERSION` stays at **15**, and `FeVersion` at **"18.0"**. What
lands is release code no release program can reach yet: the payload region
and its allocator, the collector's payload marking arm and the compactor, a
new `(payload-exhaustion)` condition, and a third build knob,
**`FE_PAYLOAD_TEST_OBJECT`**, which gives fe's own `payload_tests.c` a type
that owns a payload. No type a shipped interpreter builds owns one — strings
migrate in Phase 25 — so the substrate is complete and dormant, which is
exactly why the language version does not move.

**`FeOpenContext` carves no payload bytes**, which the Phase 22 ADR requires
of it ("This is the SPIKE's split, not a shipped constant … with
`FeOpenContext` keeping today's behaviour as the default"); the ADR's 25%
split lands as `PayloadArenaPercent`, which the internal
`OpenContextWithPayload` takes and which 23.2's options-bearing public API
will default to. So the region costs kg's arena **zero bytes** at this pin.

What does move is `FeMinimumArenaSize()`, by **280 bytes** — 66264 →
**66544** — and every figure below is re-measured at this pin rather than
carried: 224 of those bytes are the fourteen objects the new
`payload-exhaustion` condition row builds at context open (its symbol at 8
slots, its `error-message` string at 4 and its plist at 2), and 56 are the
seven `FeContext` fields the region's bookkeeping adds to the arena's fixed
header. A FIXED arena answers that by moving bytes between its pools: kg's
1 MiB arena partitions to **56145 object slots / 1086 frames** (56147 / 1087
before), the 2 MiB one `$KG_LISP_ARENA_BYTES` opens to **115127 / 2179**
(115129 / 2179), and the 640 KiB floor to **34026 / 677** (34028). Live after
a bare `FeOpenContext` is **906** (892), the same fourteen. That two-slot
move is the whole kg-side adaptation and it is not free: it is asserted
literally in five PTY cases and quoted in three comments, all corrected in
the pin-move commit — `test/pty/lisp-arena-stats-command.yaml`,
`lisp-arena-bytes-knob.yaml`, `lisp-exhaustion-mid-init-visible.yaml`,
`lisp-exhaustion-mid-command-recovers.yaml`,
`lisp-exhaustion-mid-hook-reports.yaml`, `src/lisp_core.c`'s two arena
comments and `doc/lisp-api.md`'s frame-capacity aside. The prelude census
rises by the same fourteen in both of its live figures
(`peak_live_objects` 10979 → **10993**, `reachable_live_objects` 10002 →
**10016**), with `embedded_bytes` and `definition_count` unmoved, and is
re-baselined in that commit.

The pin then moved for section 23.2, the payload region's host surface (fe
`c2515c6`). **`FE_API_VERSION` moves 12 → 13** — the one bump this phase
gets, and the commit that earns it is the one that puts anything in `fe.h` at
all — with `FeVersion` `"18.0"` → **`"19.0"`** and `FE_LANGUAGE_VERSION` at
**15**. The precedent for moving the release string on an API-only change is
Fe 17.0, `FeCollectGarbage`'s own bump; the language version does not move
because no fe type owns a payload, so nothing a Lisp program evaluates can
observe that a region exists at all. `src/lisp_core.c`'s two `static_asserts`
move with it, which is the tripwire working as designed.

What lands is `FeOpenContextWithOptions(ptr, size, options)` with the
`FeOpenOptions` record it takes (one field today, `payload_percent`,
zero-initialized to fe's own default), and five payload fields on
`FeArenaStats` — capacity, live bytes, high-water bytes, compactions and
allocation failures — appended after the existing ones, none of which changes
meaning or position. `FeOpenContext` is unchanged in behaviour, byte for
byte, and is now that call with `FePayloadPercentNone`. An out-of-range
percentage is REFUSED with a null context rather than clamped.

**kg passes `FePayloadPercentNone` explicitly** (`src/lisp_core.c`'s
`lisp_arena_options`), not the record's default of 25%: nothing kg runs can
own a payload until Phase 25, so a carve would cost a quarter of the cells
(42335 instead of 56145 at 1 MiB) for storage nothing can allocate from. The
arena partition is therefore **unmoved at this pin** — 56145 slots / 1086
frames at 1 MiB, `FeMinimumArenaSize()` still 66544 bytes, the prelude census
still 10993 / 10016 — and no PTY case, oracle snapshot or arena figure
changes.  (Every arena figure in this table is quoted at 1 MiB, and stays
quoted there so the pins remain comparable, but 1 MiB stopped being kg's
compiled default after this pin: Phase B of
`doc/plans/2026-08-19-fe-simplification-and-cheap-compat.md` made it 10 MiB,
which the same fe partitions to 586986 slots / 10917 frames, and 440466
slots under a 25% carve.) What kg gains is the reporting: the five fields
are appended, last, to `M-x lisp-arena-stats`, `test/kgbatch -g` and
`test/prelude_gc_probe`, are mirrored into `KG_PERF_LISP_PAYLOAD_*`, and
are held at **zero** by
`.ci/prelude-startup-census.json` and by `test/test_perf.c` — a ceiling, so
that payload use appearing before its phase is caught on the run it appears.

The census row that constrains kg is unchanged and still stands: fe's printer
holds a payload pointer across an `FeWriteFn`, and `src/lisp_io.c`'s callback
must keep growing a host buffer rather than allocating an fe object. Phases
23.1 and 23.2 touched no printer path, so A6-A10's re-derive-per-use property
is exactly as the sweep found it.

Fe is MIT licensed. Copyright belongs to rxi and Chris Palmer; the complete
license text is in `fe/LICENSE`.

kg compiles `fe/fe.c`, `fe/fe_eval.c` and `fe/fe_run.c` (sub-plan 03B split
the evaluator out of `fe.c` into its own translation unit, and Fe sub-plan
11B split the run driver and the public `FeEvaluate*`/`FeCall*` surface out
of `fe_eval.c` into a third, both behind the private `fe/fe_internal.h` the
three share) and their public header `fe/fe.h`. kg's Makefile names those
translation units one by one — `FE_OBJ`/`FUZZ_FE_OBJ` and their rules — so
an fe split is a named kg-side pin adaptation, not something a wildcard
picks up. The
`fex*` files, `auto.*`, and `main.c` are deliberately excluded so Fe's
optional I/O, process, regular-expression and time extensions are not
exposed. The maths natives (`sin`, `sqrt`, `expt`, …) are in `fe.c` itself
and therefore are available. Only the `src/lisp_*.c` adapter implementation
files may include `fe.h`, and only their private `src/lisp_internal.h` —
which includes fe.h itself, being a standalone header-check unit — may do
the same; `make lisp-include-check` enforces both, and the public
`src/lisp.h` stays Fe-free. `fe/fe_internal.h` is private to Fe itself —
`lisp-include-check` also forbids it anywhere in `src/` — and kg never
includes it, directly or through `lisp_internal.h`.

To update Fe:

1. Fetch `origin` in `fe/` and check out the tip of `more-elisp` (or the
   branch you are moving the pin to).
2. Review the complete submodule diff, including the kg-side divergences
   listed below — they live on that branch and must survive the update.
3. Confirm both `FE_API_VERSION` and `FE_LANGUAGE_VERSION` and adapt kg if
   either changed — an API change means adapting kg's C call sites, a
   language change means auditing kg's Lisp sources exactly as sub-plan
   02D did for language version 2.
4. Rerun `fe/test.sh` (or `make -C fe check`) and kg's full CI pipeline,
   including `.ci/ci-08-with-lisp-0.sh`.
5. Commit the submodule pointer in the superproject so the recorded SHA moves
   with the branch.

### Where a Phase 11 sub-plan named something fe does not have

Three items the Phase 11 acceptance review found recorded in no file at
all — the delivery routed around each correctly, but a reader of the
sub-plans would hunt for something that was never there. They are kept
here, beside the pin, because that is where the next person to read the
submodule against a plan document will be.

- Sub-plan 11B's enumerated test 3 raises the quit "via
  `FeRequestInterrupt`". **No such symbol exists in fe** — `fe.h`
  declares `FeInterruptFn` and nothing else of that shape. The delivery
  used `FeRaiseCompletion(ctx, FeCompletionQuit, …)`, which converges on
  the same `RaiseCondition(…, FeCompletionQuit, "quit", …)` the poll
  path uses: materially equivalent, not weaker.
- Sub-plan 11B asks for the `FE_LANGUAGE_VERSION` 9 rationale "in
  `doc/language.md`'s version table". `fe/doc/language.md` has no
  version table; every version rationale in the project lives in
  `fe/doc/c-api.md`'s version history, and that is where it landed. The
  Phase 11 fix cycle closed the gap from the other end:
  `fe/doc/language.md`'s special-variables section now names its version
  and links to the one place that holds them all, rather than splitting
  the history in two.
- Sub-plan 11B's enumerated grid labels **A8b**, **A10b** and
  **A12-shape** have no case in fe carrying those labels. The coverage
  is real and was checked case by case:
  - **A8b**, "`defconst` is let-rebindable", is subsumed by A1's shape —
    the `dvs`/`dvsf` block at the top of
    `fe/scripts/special-variables.fe`. fe has no `defconst` at all
    (`defvar`/`defconst` are kg's prelude macros over
    `internal--mark-special`), so the Emacs-comparable form is kg's, and
    it exists: the `phase11-dynamic-defconst-rebindable` oracle case,
    pinning `(2 1)` on both sides.
  - **A10b**, "a one-argument `defvar` leaves the symbol unbound", IS
    asserted in both fe places — `(assert-nil (boundp 'unb))` in
    `fe/scripts/special-variables.fe` and `CHK("(boundp 'uv)", "nil")`
    in `fe/test_api.c` — but the comment above each says A10a, which is
    the neighbouring claim about what a `let` over an unbound special
    binds. One label covering two rows, not a missing test.
  - **A12-shape**'s "first-wins *value*" half has no fe spelling,
    because `internal--mark-special` sets no value; kg's
    `test_phase11_dynamic_binding` in `test/test_lisp.c` is where that
    half lives.

  The gap is in the record of where the enumerated items went, not in
  what they cover.

## kg-side divergences from upstream rxi/fe

These changes live on `bjodah/fe`'s `more-elisp` branch. They exist because
kg presents Fe to users as an Emacs Lisp dialect, and the remaining Emacs
surface is bought in kg's own prelude in `lisp/prelude.el`. Anything
that could be done in the prelude was done there instead.

| Divergence | Why | Cost |
| --- | --- | --- |
| `if` is Emacs Lisp's `(if COND THEN ELSE...)`, with the trailing forms an implicit `do` | Upstream read them as an alternating elif chain, so `(if c a b1 b2)` silently evaluated `b1` as a *condition* and never ran `b2`. Two forms cannot coexist in one language; the elisp one wins. | `scripts/concatenate.fe` and `scripts/life.fe` were rewritten as nested `if`s; `doc/language.md` updated |
| The `fn` primitive's canonical name is `lambda`; `fn` remains bound to the same primitive | `(lambda (x) …)` is what elisp users write, and closures now *print* as `(lambda …)` rather than `(fn …)` | one entry in `primitive_names`, a `primitive_aliases` table, `type_names[FeTFn]`, one string in `FeWrite` |
| Reader macros `` `x ``, `,x`, `,@x` read as `(quasiquote x)`, `(unquote x)`, `(unquote-splicing x)` | Backquote is the difference between writing macros and fighting them. The *semantics* stay in kg's prelude; only the punctuation had to move into the reader. | `` ` `` and `,` became symbol delimiters (nothing in fe or kg used them inside symbols) |
| **The writer abbreviates `(quote X)` to `'X`** (fe sub-plan 11C, adopted at the Phase 11 pin) | Recorded as a deliberate non-change by Phase 10's 10C, and reversed by Phase 11 (11A Decision 4) because it is the divergence a user *sees* most often — every `M-:` echo and every `%S` of a quoted form carried it, even though it changed no computed value. The implementation is the symmetric copy of the `(function f)` → `#'f` block that has been in `WriteObject` since Phase 4, with the same discrimination measured against the pinned Emacs 31.0.90: exactly one element after the head and the form proper, so `(quote x y)`, `(quote)` and `(quote . x)` all keep printing as the pairs they are. Recursive, so `''x` is `'x` and `(a 'b c)` comes out as Emacs prints it. **Backquote is deliberately NOT included**, and it is not more of the same work: Emacs abbreviates over reader-produced symbols that ARE `` ` ``/`,`/`,@`, while kg's reader expands them to the ordinary symbols `quasiquote`/`unquote`/`unquote-splicing` (the row above), so closing it means changing what the reader produces and breaking any Lisp that pattern-matches on `quasiquote`. It stays the recorded `phase8-reader-backquote-symbol-names` divergence. | ~8 lines in `WriteObject`; kg-side blast radius was two sites — `test_writer_quote_abbreviation` and the `writer-quote-abbreviation` manifest row, both flipped in the pin commit because the behaviour is inherited there and the XPASS rule allows no other sequencing |
| **Special variables and shallow dynamic binding** (fe sub-plan 11B, `FE_LANGUAGE_VERSION` 9) | Recorded as a deliberate non-change by Phase 10's 10C — "fe has no dynamic binding at all" — and reversed by Phase 11, whose §17 extension lifted the parent plan's exclusion for the measured subset in 11A Decision 2. The reason is the sharpest divergence either manifest carried: `(progn (defvar dvs 1) (defun dvsf () dvs) (let ((dvs 2)) (dvsf)))` was **2** under the pinned Emacs 31.0.90 and **1** here, silently, which made the ordinary Emacs temporary-setting idiom compute a different program. A symbol now carries two flags — *special*, which `special-variable-p` answers, and *let-dynamic*, which `let` consults — set only by the core primitive `(internal--mark-special SYMBOL FULL-P)`; fe gains no `defvar` of its own, because the Emacs-shaped `defvar`/`defconst` macros are kg's and they call that primitive. Binding is **shallow**: a `let` over a marked name swaps the global value cell, records the old contents or the fact that there were none, and restores on all five completion kinds (return, error, throw, quit, budget). Two things deliberately did NOT change: closure and `lambda`/`defun` **parameters** bind lexically unconditionally even when named after a marked symbol — Emacs 31's own measured answer under `lexical-binding: t`, so the flag is consulted at `let`'s binding paths and nowhere else — and there are still no buffer-local variables and no whole-file `lexical-binding: nil` mode. `special-variable-p` answers `t` for `nil`, `t` and keywords, which is Emacs' answer for a constant even though nothing can bind one. | Two symbol flags, one new frame kind (`FeFrameDynamicLet`) and the binding-list paths; a new fuzz arm with its re-derived seeds. The frame record's growth re-partitions kg's 1 MiB arena from 56224 object slots / 1096 frames to **56226 / 1095**, and `FeMinimumArenaSize()` from 57680 to **57960 bytes**. kg-side: `defvar`/`defconst` mark, and the prelude `let` moved off its lambda-application expansion onto the core bindings-list form, which is what put kg's `let` on a path that consults the flag at all |
| **Phase 11 fix-cycle corrections** (fe `0860ba4` through `82347b3`, the phase's second pin move) | The five commits kg's and fe's Phase 11 acceptance reviews put on the branch. Three are behaviour, and only one of them is kg-visible. (1) `FeTryEvaluateStringWithOptions` returned an **unrooted** result: the epilogue restored `ctx->evaluation_result` and then called `FeRestoreGC`, which between them stripped every root, so the next collection freed the value — `ctx->evaluation_result` IS the root, exactly as `ctx->call_result` is for `FeTryCallWithOptions`, and the fix is to leave the field alone. kg's `load` goes through that entry and never saw it only because `native_load` answers `t` rather than the loaded value. (2) `ResumeDynamicLet`/`InstallLetBindings` walked the live binding list with raw `CAR`/`CDR` *after* arbitrary evaluation, so a value form that mutated its own binding list steered the walk off it — type confusion on a caller-controlled word, now a checked advance through `NextLetBinding`. kg's prelude builds every binding list itself, so no kg program reaches it. (3) `RaiseCompletionCore` re-read `ctx->condition` after its cleanup drain, so a containment inside an `unwind-protect` cleanup left an enclosing `condition-case` bound to the CONTAINED call's condition object. That one is kg-visible, because kg's `run-hooks` contains: `(condition-case e (unwind-protect (/ 1 0) (run-hooks 'h)) (error e))` with a raising hook answered `(wrong-type-argument listp 6)` before the pin and `(arith-error)` after it. | No version moved: `FE_API_VERSION` stays **7** and `FE_LANGUAGE_VERSION` stays **9**, so `src/lisp_core.c`'s two `static_assert`s are unchanged; all three are corrections to contracts already published. fe's caps are unchanged at scc **806** and pmccabe **1121**. kg's 1 MiB arena is unchanged at **56226 object slots / 1095 frames** (re-measured through `kg_lisp_arena_stats()` at this pin, not carried). kg-side: no adaptation was needed and no oracle case flipped — `make check` measures the same 131 / 120 / 11 / 0 either side of the pin — and `test_cleanup_containment_keeps_condition` in `test/test_lisp.c` is kg's regression for (3). Documentation the cycle also carried: ten stale `fe/compat/cases/macroexpand-*` printer notes retired, `fe/README.md`'s feature list qualified for special variables and for the catch/throw native-boundary wall (the debt kg's 11E filed back rather than editing across the pin), and `fe/doc/language.md` cross-referencing `fe/doc/c-api.md` for the version rationale. |
| **Phase 14's symbol surface, reader escapes and printer inverse** (fe `6a25ec9`, `FE_LANGUAGE_VERSION` 12, `FeVersion` "13.0") | The phase's single pin, one fe commit. (1) **Eight primitives**: `intern`, `intern-soft`, `symbol-name`, `make-symbol`, `gensym`, `put`, `get`, `symbol-plist`, all ordinary functions with `primitive_is_function[]` rows from the start (Phase 13.1 had to add those after the fact for `signal`/`error`/`keywordp`). The one contract worth naming is `intern-soft`'s: nil on a miss, and NO interning, pinned by a double probe on both sides, because an intern-on-miss implementation turns the real `(while (setq x (intern-soft (format ...))) ...)` idiom into an arena exhaustion instead of a termination. (2) **Reader escapes**: a backslash takes the next byte into a symbol's name literally, one escape anywhere suppresses number classification for the token, an ESCAPED dot is an ordinary list element where a bare one is the dotted-tail marker (the two read to the same interned symbol, so a reader flag and not the object tells them apart), and `##` is the empty-name symbol -- the one `#` dispatch this phase opens. What is still rejected is a backslash with nothing after it. (3) **The printer is the inverse**, escaping exactly what Emacs 31.0.90 was measured to escape. (4) **Uninterned symbols**, whose two policy questions are recorded rather than left implicit: they print as their bare name (Emacs' own answer with `print-gensym` nil, its default; fe has no `print-gensym` and no `#:`, so printing is not injective for them on either side), and they compare by identity alone. (5) **`keywordp` asks whether the interner self-bound the name**, so an uninterned `:a` is an ordinary symbol as on Emacs. (6) **Storage**: the property list lives in the symbol object, not in a context-side registry in the special-variable list's shape, because an uninterned symbol is the first symbol fe has that the collector may reclaim and a registry keyed by symbol would pin every one that ever carried a property. | `FE_LANGUAGE_VERSION` 11 → **12** (`src/lisp_core.c`'s `static_assert` moved with the pin and fired at compile time, which is what the scheme is for); `FE_API_VERSION` stays **8**. fe's caps re-set pre-pin at measured actuals: scc **832** (per-file cap unmoved at 520 -- the evaluator grows two decision points because the eight primitives are contiguous and routed by a range test, so fe_eval.c goes 511 → 513 and the bodies live in fe.c, 152 → 174), pmccabe **1210** across 386 symbols, with five per-symbol increases banked and one improvement. A symbol object one cons bigger raises `FeMinimumArenaSize()` 58080 → **60344 bytes**, and kg's 1 MiB arena re-partitions from 56225 objects / 1095 frames to **56239 / 1093** -- the object count RISES as the frame count falls, the partition moving bytes and not slots. kg-side adaptations at the pin: four arena PTY cases and the `test_lisp.c` / `test_perf.c` / `doc/lisp-api.md` / `test/lisp-compat/README.md` figures; the `phase8-reader-policy-rejections` kg-policy case narrowed to what is still rejected; two recorded divergences cleaned up rather than ignored, fe's `reader-symbol-escape` flipping `divergent` → `supported` and the `reader-empty-symbol` case moving out of `reader-hash-syntax-unsupported` into its own row. `save-excursion` and `with-current-buffer` now bind their saved state to a `gensym`, which is kg's own change and not fe's. Oracle: 174 kg cases, 163 passed / 11 recorded divergences / 0 failed, and 407 fe cases, 343 passed / 64 known gaps / 0 failed. |
| **Phase 20's string order, buffer-edge conditions and the evaluator split** (fe `dfe323f`, `18eb933`, `bee1c8f`, `fc2afd0`; `FE_LANGUAGE_VERSION` 14, `FE_API_VERSION` stays 10, `FeVersion` "15.0") | Four fe commits under one pin. (1) **The second seam split**, which this table's Phase 19 row flagged as no longer optional: `fe_eval.c` was at 519 of a 520 per-file cap, and the completion machinery -- the ambient evaluation-control record, the condition hierarchy and its handler search, the cleanup registry and every raise -- moved to a new `fe_unwind.c`. No code changed; eight statics became declarations in `fe_internal.h`, plus `PerformThrow` the other way. scc is exactly conserved (519 = 404 + 115) because the checker sums per file and the new file's boilerplate is comments and includes, which it does not count. (2) **`string<` and `string>`**, Emacs' lexicographic order, taking a string or a SYMBOL on either side and raising `(wrong-type-argument stringp X)` for anything else; strictly binary. fe compares by BYTE where Emacs compares by CHARACTER, and the two agree for every string either dialect can hold because UTF-8 preserves codepoint order -- asked at the boundary that would show it, `(string< "é" "z")` being nil on both sides. There is no `string=`: `is` and `eql` already compare strings by value, and kg has carried the Emacs spelling as a native since before this phase. (3) **`end-of-buffer` and `beginning-of-buffer` in `condition_parents[]`**, children of `error` with Emacs' own `error-message` text -- two data lines and no code, as `file-missing` was in 12C. (4) **The oracle shim's encoding**: `json-serialize` returns a unibyte UTF-8 string and the shim handed it to `princ`, which encoded it a second time, so any record containing a non-ASCII character was not decodable UTF-8 and could not be checked in. `send-string-to-terminal` writes it verbatim; regenerating the whole corpus rewrote nothing. | `FE_LANGUAGE_VERSION` 13 → **14** (the `static_assert` in `src/lisp_core.c` fired at this pin); `FE_API_VERSION` stays **10**, `fe.h`'s diff being comment text only. fe's caps re-set pre-pin at measured actuals with the bisect in its Makefile and its commits: scc **850** (the +10 is entirely fe.c 176 → 186, `StringOperandChain`'s three type tests and `StringOperandLess`'s loop; fe_eval.c does not move at all, `case` not being one of scc's complexity keywords), pmccabe **1267** across 401 symbols (+9, two new symbols and no existing one changed), `SCC_FILE_COMPLEXITY_MAX` unmoved at 520 with fe_eval.c now at 404. `FeMinimumArenaSize()` 63592 → **64200 bytes**; kg's 1 MiB arena re-partitions 56259/1090 → **56263 / 1089**. kg-side adaptations at the pin: the fourth fe object in four `Makefile` places; the two condition symbols raised by `forward-char`/`backward-char`/`delete-char` through a new `lisp_raise_buffer_edge()`, which flips three manifest rows from `divergent` to `supported` and closes `doc/TODO.md`'s buffer-edge row; the prelude `string<` deleted in favour of the primitive, which raised `apropos-max-results` from 40 to 120 against a measured ceiling that moved 54 → 158; the four arena PTY cases and the `test_lisp.c` / `test_perf.c` / `doc/lisp-api.md` / `test/lisp-compat/README.md` figures. Oracle: 271 kg cases, 249 passed / 22 recorded divergences / 0 failed, and 427 fe cases, 362 passed / 65 known gaps / 0 failed. |
| **The form feed is reader whitespace** (fe `cbcea0e`, `FE_LANGUAGE_VERSION` 15, `FE_API_VERSION` stays 12, `FeVersion` "18.0") | Emacs' `read1` retries on exactly five bytes — space, form feed, newline, tab and carriage return — and fe had four of them, so the page separator every Elisp file uses between its sections was an ordinary symbol constituent rather than a token boundary. Measured before the pin, a file holding `nil`, a form feed, a newline and `nil` answered `void-variable nil\` — the trailing backslash is the printer escaping the byte the reader had taken into the name — and a page break alone on its line answered `void-variable \`. That is how the Phase 21 capabilities sweep found it, walking `s.el:770` and `f.el:39`. The set is now ONE definition, `#define ReaderWhitespace " \f\n\t\r"`, pasted into each of the four places the reader asks a whitespace question — the skip loop in `Read`, the atom delimiter in `ReadAtom`, the `?` literal's own in `RequireCharacterDelimiter` and the radix digits' in `ReadRadixDigits` — because four literals were four chances to add a byte to one of them and forget it in the others, which is the shape of the bug being fixed. **Three arms deliberately do not move**: an ESCAPED form feed is a symbol constituent, as an escaped space is — the escape decides, not the byte; a form feed inside a string body was never reader syntax and `ReadStringLiteral` still consults no set; and a comment still ends at a newline and nothing else, which is Emacs' rule too, so a page break inside one is comment text. | `FE_LANGUAGE_VERSION` 14 → **15**; the `static_assert` in `src/lisp_core.c` moved with the pin and fired at compile time, which is what the scheme is for. Neither fe ratchet moves — scc **867** and pmccabe **1285** across 408 symbols, both unchanged, a `#define` and compile-time string concatenation adding no branch and no symbol — and no arena figure moves either, kg's 1 MiB arena still measuring **56147 object slots**. fe's evidence for the pin move: its own `make check`, `make perf-check` and the full `.ci/run-ci-steps.sh` (all ten steps) green, and `make compat` at 433 cases, **368 passed** (362 before), 65 known gaps, 0 failed. kg-side: the `reader-form-feed-page-break` oracle case, whose expression carries a real 0x0C, and `test_reader_form_feed_page_break` in `test/test_lisp.c`, which walks the page break, the four sites that moved and the three arms that did not through `kg_lisp_eval_string`. That snapshot is stamped **Emacs 31.0.91** where the corpus's other 417 carry 31.0.90: this box's `/opt-3` pin has moved since they were taken and `run-emacs-oracle.py` refuses to overwrite a snapshot recorded under a different version, so the mixture is the honest state and re-pinning the rest is its own decision with its own diff. |
| **Phase 12's four fe changes** (fe `95965f0`, `092e978`, `b030d0a`, `38caa63`, `FE_LANGUAGE_VERSION` 10, `FeVersion` "11.0") | The phase's single pin, and the four fe commits it adopts. (1) **Condition handlers inside cleanups are honored.** `RaiseCompletionCore` tested `ctx->cleanup_catch` and replayed to `RunOneCleanupEntry`'s `setjmp` *before* it ever called `FindConditionHandler`, so while any cleanup entry ran, every raise took that jump and the cleanup's own handler frames were never examined: `(unwind-protect 'body (ignore-errors (car 6)))` was `body` under the pinned Emacs 31.0.90 and escaped to the host here, with nothing being unwound. A found handler is now accepted only when its frame index is at or above the floor `RunOneCleanupEntry` already saved for the current entry, which leaves a **native** cleanup bit-identical — kg has three, and they do not republish `run_base`, so nothing is above their floor. **06A Decision 4 is unchanged and is now a pinned guard**: an *unhandled* cleanup raise still replaces the completion being unwound. (2) **`eval` exists**: Emacs' `(eval FORM &optional LEXICAL)`, evaluating FORM in the caller's own run through the existing `FeFrameRelay` redispatch, so conditions, throws and quits out of it reach enclosing handlers and catches. Its environment is the GLOBAL one, because Emacs' LEXICAL argument *selects* an environment and never inherits the caller's — measured, `(let ((qq 1)) (eval 'qq))` is `(void-variable qq)` on 31.0.90 — and a non-nil LEXICAL is rejected by name. (3) **`file-missing` under `file-error`**, one data line in `condition_parents[]`; `file-error` was already there, so the `doc/TODO.md` claim that the hierarchy blocked this was stale. It is what lets kg `(signal 'file-missing ...)` at all, since `IsConditionSymbol` gates `signal`. (4) **A one-argument `defvar`'s mark is scoped to its input unit**: `EvaluateInput` — entered exactly once per `load`, `require`, batch file or prelude install — stamps each let-dynamic-only mark with a monotone token, and `SymbolIsLetDynamic` compares for **equality**, which is the measured rule in both nesting directions. Full marks (two-argument `defvar`, `defconst`) stay global, as in Emacs. Outside any input unit — `FeCall` paths, i.e. kg's hooks, command dispatch and process callbacks — every mark is visible. | `FE_LANGUAGE_VERSION` 9 → **10** (`src/lisp_core.c`'s `static_assert` moved with the pin); `FE_API_VERSION` stays **7**. fe's caps re-set pre-pin at its measured actuals: scc **808**, pmccabe **1133** across 361 symbols. kg's 1 MiB arena re-partitions from 56226 object slots to **56225**, frames unchanged at **1095**, and `FeMinimumArenaSize()` 57960 → **58080 bytes** (re-measured at this pin through `kg_lisp_arena_stats()` and a direct probe, not carried). kg-side adaptations at the pin: `test_phase11_dynamic_binding`'s A7b probe had to declare and bind in ONE `kg_lisp_eval_string()` call, since each of those is its own input unit — which is exactly what Emacs answers for `(eval '(defvar v) t)` followed by a separate `(eval '(let ...) t)`; the three arena PTY cases moved by one slot; a new `supported` manifest row `phase12-cleanup-handler-visibility` with six oracle cases the gap never had; and `phase11-one-arg-defvar-file-scope`'s rationale rewritten to say what it actually pins (the oracle shim's per-form scoping, not the leak) with the two-file probe `test_phase12_one_arg_defvar_file_scope` carrying the fix's evidence. **No oracle case flipped at the pin** — 131/120/11/0 before, 137/126/11/0 after, the six new cases being the whole difference. ONE NARROWING is recorded rather than defended, in fe's own `one-arg-defvar-scope-carrier` row: Emacs threads the mark through the file's lexical environment, so a `defun` written after the defvar in file A stays dynamic when called from file B, and fe — consulting the flag where the `let` runs — answers lexically there. |
| **Phase 12 fix-cycle corrections and the input-unit trio** (fe `2d53a78` through `add7b58`, the phase's second pin move; `FE_API_VERSION` 7 → **8**, `FE_LANGUAGE_VERSION` stays **10**, `FeVersion` stays `"11.0"`) | The nine commits the Phase 12 acceptance reviews put on the branch. Three are behaviour. (1) **The input-unit blocker**: an UNCONTAINED raise — one reaching the host `error_fn` through `FeEvaluateStringWithOptions`/`FeEvaluateFileWithOptions`, which is kg's `M-:`, `eval-buffer`, `eval-last-sexp` and the `init.el` load — left the abandoned unit's scope number in `ctx->input_scope` for the life of the context, so one failed `M-:` permanently inverted one-argument-`defvar` visibility in both directions. Fixed at `RaiseCompletionCore`'s host exit (`EnterHostInputContext`, beside the `ClearEvaluationControl` that exists for the same reason), all three abnormal kinds. The same cycle found and fixed a second leak of the same shape: the containment barriers restored the scope but not the diagnostic label, so a contained nested failure cost the enclosing file its file name in every later diagnostic; scope, label and position now travel as one value (`FeInputUnit`). (2) **The input-unit trio**, the API bump: `FeEnterInputUnit`/`FeReadInputForm`/`FeLeaveInputUnit`, letting a host drive a read-eval loop inside one input unit in the CURRENT run — per-form line published for diagnostics, no new run started, so conditions, throws and quits out of a hosted form reach handlers and catches established outside the loop. This is the entry set kg's prelude `load` loop is built on; the kg review of Phase 12 proved (by enumeration of all 60 public entries) that no prior API allowed it. (3) A native `unwind-protect` cleanup that VIOLATES `FeCleanupFn`'s no-evaluator contract and runs Lisp now has a `condition-case` in that Lisp honored (the cleanup-handler floor's own rule; matches Emacs and the pure-Lisp form); the bit-identity claim is narrowed to contract-honouring cleanups. Plus: the GC-rooting compat case now actually collects (60000 conses, 4 collections measured), two pre-existing cleanup-replacement divergences from Emacs are documented and pinned (`unwind-protect-cleanup-raise-residuals`, status divergent), and the fe docs debt from the docs review is swept. | `FE_API_VERSION` 7 → **8**: `src/lisp_core.c`'s assertion moved with the pin — the tripwire fired at compile time, which is the behaviour the scheme exists for. fe's caps re-set pre-pin at measured actuals: scc **808** (unmoved; every added line is below scc's known string-state undercount in `fe.c`), pmccabe **1146** across 368 symbols. kg-side at the pin itself: **nothing observable moved** — `make check` measures oracle 143/132/11/0, unit 32/32, PTY 443/443 on both sides of the pin, and the arena tests pin the same 56225 object slots / 1095 frames. The consumer of the trio — the prelude `load` loop, with the `load-throw-reachability` flip that a current-run loader finally makes possible — lands in the commits after the pin, each with its own measurements. |
| Calling a non-function whose head is a symbol raises `void-function NAME` | Upstream's `tried to call non-callable value` never said which name was unbound | one helper in `Evaluate` |
| `GcStackSize` 512 → 4096 | The GC stack, not the C stack, bounded recursion upstream: it died at about 70 frames, too few to write ordinary list code. That stopped being true of Lisp nesting at all once the frame machine replaced the recursive evaluator (see the dedicated frame-machine row below) — every completed frame used to retain two redundant entries on this stack (`FePushGC(env)`/`FePushGC(rest)`) until its whole activation unwound, which still made `(deep 1021)` the practical ceiling before `(deep 1022)` raised `GC stack overflow`. The fix (`675bcec`, kg pin `e465d21`) found that retention was pure redundancy — every completed frame's result is delivered straight into the frame below's `callee` with no allocation in between, and every frame field is already an exhaustive mark-phase root, so only the run's own result needs a push, once, at the barrier. Measured after: `peak_gc_stack_depth` is a **constant 14** at every nesting depth tested — 10, 1000 and 100000 (`TestGcStackConstantInNesting` in `fe/test_api.c` pins it as a constant, not a threshold) — where it used to be `2N + 9`. 4096 remains generous headroom for the reader's and writer's own native C recursion, which does still consume this stack, but it is no longer a Lisp-recursion bound of any kind, designed or accidental; sub-plan 03F's two bounds (see below) are what actually limit Lisp nesting and native re-entry now. | `FeMinimumArenaSize()` measured 53840 bytes when this row landed (up from 36784 once `boundp`/`makunbound` were added, pre-frame-substrate; the frame substrate's arena-resident 64-frame floor (`MinFrameCapacity`) plus 32-frame cleanup reserve (`CleanupFrameReserve`), each a 96-byte `FeEvalFrame`, account for the rest — see the kg-side sub-plan set's 03A Decision and its 03D follow-up for the retune history). A `KG_LISP_ARENA_SIZE` override below `FeMinimumArenaSize()` fails to open a context; kg's default 1 MiB arena partitioned to `frame_capacity` 1100 then, under `FrameArenaPercent`'s 10% split of the remainder. Both are 03F-era measurements; the Lisp-2 row below carries the current figures. |
| Lisp-2 value and function namespaces (sub-plans 04B–04D) | Phase 4 of kg's Emacs-subset program replaces Fe's single per-symbol cell with the two Emacs namespaces. 04B made the representation (a function cell beside the value cell), 04C added the accessors and the nine primitives behind a transitional value-cell fallback so existing lookup kept working, and 04D cut: the bootstrap callables moved into function cells, the fallback is deleted, call position resolves a symbol's function cell *only* (an empty cell is `void-function NAME` even when the value cell is full), `#'x` reads as `(function x)`, and `FeDefineNative` now registers into the function cell. | `CDR(sym) = ((name . function) . value)` is private behind `SymbolName`, `SymbolBindingCell`, `SymbolFunction`, and `SetSymbolFunction`; `FeSetFunction`/`FeGetFunction`/`FeIsFBound` and nine primitives (`function`, `fset`, `defalias`, `symbol-function`, `symbol-value`, `fboundp`, `fmakunbound`, `funcall`, `apply`); the writer prints `(function X)` as `#'X`; fe keeps its own `FeTMacro` rather than Emacs' `(macro . FUNCTION)` cons — a recorded, tested representation divergence observable only through `symbol-function` of a macro (the manifest's `lisp2-macro-representation` is `kg-policy`, not an Emacs-comparison gap). `FeVersion` `"3.0"` → `"4.0"` and `FE_API_VERSION`/`FE_LANGUAGE_VERSION` 2 → 3 together. The new primitive symbols raise `FeMinimumArenaSize()` to **55616 bytes** (from 54656); kg's 1 MiB arena measures **1098 frames / 56221 object slots**. |
| `FeIsFunction()`, `funcall`/`apply` operand rules, and Emacs' zero-operand arithmetic identities | The follow-on slice to the Lisp-2 cut. kg's `functionp` had to decide the same question the evaluator does, and was spelling out its own `FeTFn || FeTNativeFn || FeTPrimitive` test, which said `t` for `if` — Emacs says `nil` for a special form, and for a macro. Fe now owns that classification and exposes it, so the host predicate and the interpreter's own `invalid-function` rejection cannot disagree. Two smaller Emacs alignments ride along: `funcall`/`apply` handed a macro or special form raise `invalid-function X` rather than calling it, and name the symbol the caller actually wrote in a `void-function` error; and `(+)` is `0`, `(*)` is `1`, `(-)` is `0` while `(/)` is `wrong-number-of-arguments`. | `FeIsFunction(FeContext*, FeObject*)` — a purely additive C entry point, so `FE_API_VERSION` stays 3 and `src/lisp_core.c`'s two assertions are unchanged; no new primitive symbols, so `FeMinimumArenaSize()` stays **55616 bytes** and kg's arena still measures **1098 frames / 56221 object slots**. kg-side: `native_functionp` (`src/lisp_cmd.c`) asks `FeIsFunction` about the resolved designator, and `utils/check_lisp_compat.py` stopped demanding exactly one manifest entry per source name, since fe's manifest now pins two `funcall` behaviours separately. |
| `FeGetFunction()` answers `nil` for a cyclic designator chain instead of raising | It is the resolver a *C host* calls, and a C caller cannot catch an Fe error: `FeHandleError` longjmps into whatever evaluation is running, which for a host resolving a hook or process-callback name is an *outer* run whose guarded frame the jump skips past, or no run at all. `(fset 'x 'x)` plus `(add-hook 'before-save-hook 'x)` plus a save was a segfault in kg for exactly that reason — the last uncontained case on the path b4c5ddc moved from raising to reporting. Not copied from Emacs, which has none to copy: Emacs 31.0.90's own `fset` signals `cyclic-function-indirection` and leaves the cell untouched, so `indirect-function` never sees a cycle. | `ResolveFunctionCallable` gains a `cycle` out-parameter (null = raise), which is what both evaluator call sites pass; call position, `funcall`, `apply` and `FeIsFunction` all keep raising `cyclic-function-indirection`, and only `FeGetFunction` maps a cycle to `nil`. Additive behaviour on one entry point, so `FE_API_VERSION` stays 3, `FeMinimumArenaSize()` stays **55616 bytes** and kg's arena still measures **1098 frames / 56221 object slots** (re-measured). kg-side: `lisp_callable_designator` (`src/lisp_core.c`) reports `void-function NAME` for a cycle too — `FeIsFBound` cannot separate it from a chain dying in an empty cell, since `(fset 'a 'b)` with `b` unbound has a full first cell as well — and `(functionp 'x)` now answers `nil` rather than raising. |
| Numeric tower groundwork (sub-plan 05C) | Host-created `FeTInteger` values now flow through arithmetic, exact/chained comparisons, `is`, predicates, and the core math natives while the reader intentionally remains double-only until 05D. Integer-only arithmetic preserves `int64_t`, with checked overflow and division errors; mixed operations promote to double. | Five primitives (`>`, `>=`, `/=`, `integerp`, `floatp`) and the math/native changes raise `FeMinimumArenaSize()` from **55616 to 56112 bytes** (+496 bytes, 31 objects); the row below's **56304** is the figure *after* 05D added `eq` and `eql` for a further +192 bytes / 12 objects, and this row used to claim the whole +688 for 05C. Re-measured per commit at pin-move time (`af18906` 55616, `439d654` 56112, `17ac959` 56304). The five names are claimed in Fe's compatibility manifest so kg's pin-time compatibility check sees no unowned primitive. |
| Two number types, with Emacs' integer lexing and printing (sub-plans 05B–05D) | Phase 5 of kg's Emacs-subset program gives Fe its second number type and cuts the reader/printer over to it in one versioned break. 05B added `FeTInteger` (`int64_t i` in the existing `Value` union — `sizeof(Value)` stays 8 on both CI compilers, per 05A's measured spike), dormant: host-constructible, printable, collected, producible by no Lisp program. 05D's cut replaced `ReadAtom`'s bare `strtod` with an Emacs number lexer (`42` reads as an integer, `42.0` as a float, and `0x10`, `inf`, `nan`, `1e` are all symbols again), killed the printer's integral-double shortcut so floats always print with a `.` or exponent in shortest-round-trip form and the exceptional values print Emacs' `1.0e+INF`/`-0.0e+NaN` spellings, and landed `eq` and `eql` as core primitives with Emacs semantics — `eq` is pointer identity or both-integers-equal, `eql` adds same-type floats equal by bits, so `(eq 3 3)` is `t` and `(eq 3.0 3.0)` is `nil`. | `FeTInteger` in the `Value` union; `FeMakeInteger`/`FeToInteger`; `type_names[FeTInteger]` = `"integer"` (which is what makes kg's `(type-of 1)` say `integer` with zero kg code); the seven primitives from 05C/05D (`>`, `>=`, `/=`, `integerp`, `floatp`, `eq`, `eql`); `FeVersion` `"4.0"` → `"5.0"` and `FE_API_VERSION`/`FE_LANGUAGE_VERSION` 3 → 4 together; `FeToDouble` widened to read an integer so kg's numeric argument readers work unchanged. **No bignums**: int64 overflow raises an `arith-error`-style message and an integer literal past `INT64_MAX` reads as a double — both recorded divergences against Emacs' bignums. Fe scc 459 → 538 and pmccabe 660 → 752 across 05B–05D (actuals; the tower 05C is the bulk of both); `FeMinimumArenaSize()` 56304 bytes with kg's 1 MiB arena at 1097 frames / 56225 object slots (05A's measured projection, verified). |
| Phase 5 post-close review fixes (fe `c0e0ad8`..`e351b67`) | The eleven commits kg's Phase 5 review put on the branch, all behavioural corrections inside the numeric tower rather than new surface. `floor`, `ceil`, `round` and `abs` refuse a NaN operand and a zero divisor with `arith-error` instead of leaving the C conversion undefined; the comparators (`=`, `<`, `<=`, `>`, `>=`, `/=`) took Emacs' rule that one operand is `t` after a type check and that a chain short-circuits, so a non-number after a failed link is never reached; `is` regained its epsilon tolerance across an integer and a float; and the reader requires an explicit sign in a nonfinite exponent, so `1eINF` reads as a symbol again. | No C entry point moved, so `FE_API_VERSION` stays **4** and `src/lisp_core.c`'s two `static_assert`s are unchanged; no primitive was added, so `FeMinimumArenaSize()` stays **56304 bytes** and kg's 1 MiB arena still measures **1097 frames / 56225 object slots** (re-measured at the pin move, not assumed). kg-side: the NaN rounding fix is reachable *only* from kg — fe's own `floor` is shadowed by Fex in the standalone interpreter, kg links core fe without it — so `(floor (/ 0.0 0))` raising `arith-error` is pinned in `test/test_lisp.c` as kg's evidence for it. `lisp_finite()` (`src/lisp_buffer.c`) also stopped leaking fe's `expected double, got X` text at kg's own argument seam: it tags the operand itself and raises `wrong-type-argument`, which is what `(goto-char "x")` now says. |
| `catch`/`throw` non-local exits (sub-plan 06C) | Emacs Lisp needs a non-local exit that stops at the innermost matching dynamic catch rather than unwinding every evaluator frame to the host. `catch` evaluates its tag and body as a special form; function-shaped `throw` evaluates exactly two operands, compares tags with `eq` (except `nil`, which never matches), drains intervening `unwind-protect` cleanups, and delivers its value to the catch. An uncaught throw remains a `no-catch TAG VALUE` message until condition objects arrive in 06D. A throw cannot cross a native re-entry boundary: Fe contains it as `no-catch` rather than discarding live C activations. That was a recorded divergence from Emacs for `save-excursion` and `with-current-buffer` until Phase 11 removed the *frame* rather than the wall — both are prelude macros over Lisp `unwind-protect` now, so nothing native stands between the throw and the catch. The wall itself is unchanged and still applies to every callback kg invokes from its own C: hooks, process filters and sentinels, a nested `command-execute`, and the containment barrier the loader installs. | `FeFrameCatch`, the `FeCompletionThrow` cleanup-drain path, and `catch`/`throw` primitives; test coverage includes value delivery, nested catches, tag identity, forced GC, cleanup order, frame limits, and the native-boundary wall. The two names and primitive objects plus the `run_base` field raise `FeMinimumArenaSize()` **56304 → 56504 bytes**; kg's 1 MiB arena remains **1097 frames** and measures **56226 object slots**. |
| Condition objects, `signal`, `error`, `condition-case` (sub-plan 06D) | Errors are now condition objects `(SYMBOL . DATA)`, constructed at raise time. `signal` and `error` are evaluate-then-raise primitives: `(signal 'ARITH-ERROR DATA)` raises `(ARITH-ERROR . DATA)`, `(error "fmt" ARGS)` formats at signal time and raises `(error "formatted-text")`. `condition-case` is a special form reusing 06C's catch machinery: handler specs walk a static condition hierarchy (`wrong-type-argument`, `void-function`, `arith-error` etc. are under `error`; `quit` is a separate branch not under `error`), select the first textually-matching handler, bind the var, and run the handler body as an implicit `progn`; unmatched conditions re-signal unchanged. The cleanup-raise policy matches Emacs: a raising cleanup's error replaces the in-flight one -- narrowed by Phase 12 to a cleanup error nothing *inside* the cleanup handles, which is Emacs' rule and was always what these probes measured. `FE_LANGUAGE_VERSION` 4 → **5** (catch/throw/condition-case/signal/error changed what programs mean); `FE_API_VERSION` 4 → **5** (the accessor surface and completion contract are one visible break); `FeVersion` `"5.0"` → `"6.0"`. kg's `static_assert(FE_API_VERSION == 5)` fires at the pin. | `FeCompletion`'s three dead kinds (Quit, Throw, Budget) become true at their producers; `FeGetCompletion()`/`FeGetCondition()` accessors readable from the host error callback; the static condition hierarchy table (`error` → `wrong-type-argument`/`wrong-number-of-arguments`/`void-function`/`void-variable`/`args-out-of-range`/`arith-error`/`file-error`/`cyclic-function-indirection`/`invalid-function`/`no-catch`; `quit` is separate); 27 condition-named fe raise sites become structured signals, 70 prose sites become `(error "text")` with byte-identical message text; `condition-case` and `signal`/`error` primitives with full test coverage including the parent's five-kinds gate matrix. The new primitives, symbols and condition hierarchy raise `FeMinimumArenaSize()` **56504 → 56824 bytes**; kg's 1 MiB arena measures **1097 frames / 56225 object slots** (the **1088 / 56287** this row first claimed was a transcription error — re-measured through `kg_lisp_arena_stats()` at the pin move below, which is also where the *current* figures live). **Deliberately excluded by 06A:** `:success` handlers, `handler-bind`, no debugger hooks, no Lisp-visible `error-conditions`/`get`. Budget exhaustion is not catchable by `condition-case` (Emacs has no budget concept — recorded as a divergence). |
| Phase 6 post-close review fixes (fe `374e52f` through `1d96a58`) | The twelve commits kg's Phase 6 review put on the branch, `374e52f` first and `1d96a58` last. One of them is a *removal*: `FeSaveEvalState`/`FeRestoreEvalState` are gone, because restoring an evaluation-control record into a frame Fe has already unwound past is undefined — which is precisely what kg's hook and process-callback seams were doing, and what the red `test_hook_throw_containment` was reporting as a corrupted GC stack. Its replacement is the protected call, `FeTryCallWithOptions()`, whose `setjmp` lives inside Fe in a frame that is live for exactly as long as the call: a non-normal completion is *returned* rather than thrown past the host's C frame, `error_fn` is not called, the callee's cleanups run and its frames and GC entries are discarded, and `FeGetCompletion()`/`FeGetCondition()`/`FeGetCompletionMessage()` describe it. `FeResignal()` puts a contained completion back in flight in the enclosing run with kind, condition object and message intact, which is what a *wrapping* native (kg's `save-excursion`, `with-current-buffer`) needs and a *containing* one (a hook, a filter, a sentinel) deliberately does not use. Six behavioural corrections ride along: `FeRaiseCompletion` now accepts only Error/Quit/Budget and asserts on the two kinds no host may raise; a cleanup's `throw` reaches the catch it names rather than being answered `no-catch` when that catch lies below the cleanup's own nested run; a caught condition no longer disarms the interrupted program's remaining steps, frame wall and interrupt; a real host quit is catchable by the handler that names `quit`; `error`'s format directives took Emacs' answers for `%s`, `%S`, extra arguments and a no-format string; and the condition hierarchy was trimmed back to the depth 06A Decision 1 scoped it to. | No version moved: `FE_API_VERSION` and `FE_LANGUAGE_VERSION` both stay **5**, so `src/lisp_core.c`'s two `static_assert`s are unchanged — the removal is of a pair of entry points added inside this same unreleased phase, and everything else is additive or behavioural. `FeMinimumArenaSize()` **56824 → 56856 bytes**, and kg's 1 MiB arena measures **1097 frames / 56225 object slots** (both re-measured at the pin move through `FeMinimumArenaSize()` and `kg_lisp_arena_stats()`, not carried over). kg-side, in the same commit as the pin: `src/lisp_hooks.c` and `src/lisp_process.c` contain through the protected call instead of a host `setjmp`, and re-signal a quit or a budget completion rather than swallowing it, so C-g is never eaten by whichever hook was running; `src/lisp_core.c`'s `lisp_call_body()` gives `save-excursion` and `with-current-buffer` the wrapping treatment, which makes them transparent to an enclosing `condition-case` for the first time (`throw` stays walled at the native re-entry boundary — `test/lisp-compat/features.json`'s `catch-throw-reachability` records that divergence); and `kg_lisp_last_error_kind()` (`src/lisp.h`) lets `src/cmd.c` tell a quit from an error by kind instead of comparing the reported message to the string `"Quit"`. |
| `FeToString(dst, 0)` writes nothing and returns 0 | `size - 1` underflowed, so a zero-size destination received the whole rendering plus a terminator past it | the contract is now written down: bytes stored, never more than `size - 1`, always terminated. No snprintf-style required length, because measuring is unbounded on a cyclic object |
| Malformed dotted lists are syntax errors | `'(a . b c)` read as `(a c)`, `'(a . b . c)` as `(a . c)` and `'(. a)` as `a`, all silently | three new reader diagnostics; `(a .)` says `missing value after '.'` instead of `stray ')'` |
| A macro expands on every call instead of overwriting its call site | copying the expansion over the call site cloned it, and `nil` and interned symbols are compared by address: a macro expanding to `nil` produced a truthy nil, and one expanding to a symbol missed every lexical binding | one expansion per invocation, charged against the step budget; kg's "a macro expands once per call site" caveat is gone from `README.md` and `doc/kg.1` |
| The writer is bounded and terminates on cycles | `(setcdr x x)` then rendering hung kg with no C-g escape: `M-:`, `eval-buffer`, `C-j`, `format`'s `%s`/`%S` all render | `#<cycle>`, `#<deep>` and `#<truncated>` are stable output; `FeWriteWithOptions()` carries the budgets and reports completion; the writer no longer allocates, and spends the step budget while an evaluation is active |
| `&optional` and `&rest` in parameter lists | Emacs Lisp spells them that way, and kg had to delete `&optional` from every arglist because the binder could not see it | `internal--arglist` is gone from kg's prelude; `&optional` and `&rest` cannot be parameter names |
| Strict argument-count checking (sub-plan 07B) | `((lambda (x) x))` and `((lambda () 1) 2)` now raise `wrong-number-of-arguments`; `&optional` binds nil and `&rest` collects remaining arguments | unconditional in Fe; `FeSetStrictArity()`/`FeGetStrictArity()` and the `-a` mode are removed; `FE_API_VERSION`/`FE_LANGUAGE_VERSION` 5 → 6 together and `FeVersion` `"6.0"` → `"7.0"`. The arity table's per-call `(FUNCTION NARGS)` condition data raises `FeMinimumArenaSize()` **56856 → 56880 bytes**; kg's default 1 MiB arena measures **1097 frames / 56223 object slots**. (This row first claimed 56856 bytes and 56225 slots, which were Phase 6's figures carried forward unchecked; the numbers here are measured at the strict-arity commit itself.) |
| Phase 7 post-close review fixes (fe `b6bf07d` through `e576a0b`) | The eleven commits kg's Phase 7 review put on the branch, `b6bf07d` first and `e576a0b` last; all corrections inside the strict-arity work rather than new surface. Five change behaviour a program can observe. A long argument list no longer overflows the GC root stack: `ArgsToEnv`'s double cons of the argument list is gone (which also restores a macro's ability to `setcar` the caller's raw source, silently lost in 035614d), the two resume loops restore their own `gc_checkpoint` per delivered value, and `FePushGC` keeps a 64-slot reserve so the overflow report — which used to recurse through `FeHandleError` into the full stack and reach SIGSEGV — allocates nothing. An improper argument list leaves the arity path entirely: `(car 1 . 2)` is `(wrong-type-argument listp 2)`, which is both what fe answered before Phase 7 and what Emacs 31.0.90 answers, rather than `wrong-number-of-arguments` with a nil FUNCTION and a meaningless count. `apply` drops to a `{1, …}` minimum so `(apply 'f)` reaches the proper-list check instead of an arity error, and `lambda`/`fn`/`macro` to `{1, …}` so a body-less closure is constructible and `((lambda (x)) 1)` answers nil, as in Emacs. Every primitive gets one arity policy — the nine-primitive prose exception list (`function`, `boundp`, `makunbound`, `symbol-function`, `symbol-value`, `fboundp`, `fmakunbound`, `integerp`, `floatp`) is deleted, so they raise `wrong-number-of-arguments` with `(FUNCTION NARGS)` like every other row, which is also what Emacs measures — and `(setq a 1 b)` gains the identity and count Decision 4 required, `(setq 3)`. Three latent defects go with them: a `condition-case` inside a host-driven nested run no longer strips the enclosing native's `(FUNCTION NARGS)` record, `CollectGarbage` marks `ctx->native_identity` directly instead of relying on the invoking frame, and a raise landing on the arena's last free cell collects before calling it exhaustion rather than silently substituting a nil condition that matches no handler. | No version moved: `FE_API_VERSION` and `FE_LANGUAGE_VERSION` both stay **6** and `FeVersion` stays `"7.0"`, so `src/lisp_core.c`'s two `static_assert`s are unchanged. No primitive was added or removed, so `FeMinimumArenaSize()` stays **56880 bytes** and kg's 1 MiB arena still measures **1097 frames / 56223 object slots** (all three re-measured at this pin move through `FeMinimumArenaSize()` and `FeGetArenaStats()`, not carried over). kg-side: no source or test change was required — kg's prelude and natives never called `apply` with one operand, never constructed a body-less closure, and never asserted the nine primitives' prose arity text — so this pin move is the gitlink and this row. Fe's own caps at the pin: scc 670/760, `fe_eval.c` 453/520, pmccabe 886/980, worst function 15/22, no cap raised. |
| Identical dispatch branches merged (fe `a14b43b`) | kg's `ci-06` runs `clang-tidy` with `-warnings-as-errors` over `fe_eval.c` (kg compiles it), and the Phase 7 review's dead-code removal had left three runs of byte-identical `case` bodies in `DispatchPrimitive` that `bugprone-branch-clone` rejects; the runs are merged into shared fall-through labels, comments preserved per label | zero behaviour change (label set identical, `-O0` disassembly differs only by the deleted duplicate body); no version, arena or cap movement; kg-side this pin move is the gitlink and this row |
| **Protected constants, self-evaluating keywords, and a strict reader** (sub-plans 08B–08C, fe `399a757` and `c40ca6c`) | Phase 8 of kg's Emacs-subset program gives Fe the two things an ordinary `init.el` trips over first. 08B (`399a757`): `nil`, `t` and every keyword symbol are constants — `setq`, `set`, `let` binding position, `fset`/`defalias` and lambda parameters all refuse them with the new `setting-constant` condition (a new row under `error` in the static hierarchy) instead of silently corrupting `if`/`cond`/`and`/`or` for the session; a keyword interns self-evaluating, so `:foo` is `:foo` and `(eq :a ':a)` is `t`; `keywordp` is a new primitive; and `let` accepts Emacs' binding *list*, which is what retires kg's prelude workaround for the one-binding form. 08C (`c40ca6c`): the reader stops misreading — `?a`/`?\n`/`?\C-a`/`?é` are character literals reading to codepoint integers, `#x`/`#o`/`#b` are radix integers, string escapes share one table, and everything Fe does *not* implement (`[1 2 3]`, `#:sym`, `#s(…)`, a bare `#`, symbol escapes, unknown string escapes) is a named read error rather than a silent misreading. `FE_LANGUAGE_VERSION` 6 → **7** and `FeVersion` `"7.0"` → `"8.0"`; `FE_API_VERSION` stays **6**, since no C entry point moved. | The `#` reader break is the one that reaches source: a bare `#` and `#`-initial symbols no longer read (fe's own `scripts/life.fe` had to spell its live-cell glyph `"#"`), and `[`, `#:` and unknown string escapes now raise where they used to be read as something else. kg-side the audit found nothing to adapt — `lisp/prelude.el` and `lisp/auto-fill.el` contain no executable `?`, `[`, radix `#`, bare `#` or symbol-escape spelling, and neither do kg's `src/lisp_*.c` Lisp string literals or the `test/pty/lisp-*.yaml` corpus — so no kg source, test or PTY case changed for either half. Measured figures for the pair are in the row below, at the commit kg's gitlink actually moves to; `keywordp`, `setting-constant` and the reader's new symbols are what raise `FeMinimumArenaSize()` there. |
| Phase 8 post-close review fixes (fe `fba716d` through `acc94f7`) | The eight commits kg's Phase 8 review put on the branch, `fba716d` first and `acc94f7` last; corrections inside 08B/08C rather than new surface. `fba716d` merges `RaiseCompletion`'s two byte-identical label+line branches, which `bugprone-branch-clone` rejects and which had turned kg's `ci-06` lane red at the c40ca6c pin — the same defect class `a14b43b` removed one row above. `2a6f8b9` stops five kinds of literal being misread, each row measured against Emacs 31.0.90: the control modifier is **not** `& 0x1f` (`?\C-?` is 127, `?\C-%` is 67108901, `?\C-é` is 67109097 — `@`..`_` and `a`..`z` fold, everything else keeps its value with the 2²⁶ bit set); `?` literals must end at a delimiter, so `?ab`, `?\1a` and `?\C-C-a` are `invalid-read-syntax` rather than one character plus a leftover token; `\x` is greedy and variable-width, bounded at U+10FFFF; Emacs' modifier spellings Fe does not implement (`\s-`, `\S-`, `\A-`, `\H-`, `\^`) are rejected *by name*; a mid-token backslash (`a\ b`) is rejected anywhere in a token, not only at its first character; `#x`'s digit cap is gone (a long literal takes the recorded overflow-to-double policy); `?` at end of input is an error; and the top-form line latch now skips comments *before* latching, so a comment-prefixed file reports the form's real line. `7d04370` re-measures three constant rows 08B guessed at: `:` alone **is** a keyword (self-evaluating, `keywordp` → `t`, `(setq : 1)` → `setting-constant`), and a `let` binding list refuses `&optional`/`&rest` targets by name instead of leaking them into the parameter decoder. `06ce9ea` deletes the test harness's expectation-rewriting heuristic (it could not express a two-digit line number at all); `cedda8a` adds `scripts/reader.fe`; `7a55a95` records all 13 divergence classes in fe's compat corpus; `491e793` re-banks pmccabe honestly; `acc94f7` corrects `doc/implementation.md`. | No version moved: `FE_API_VERSION` stays **6**, `FE_LANGUAGE_VERSION` stays **7**, `FeVersion` stays `"8.0"`, so `src/lisp_core.c`'s two `static_assert`s are unchanged. `FeMinimumArenaSize()` is **57016 bytes** (56880 at the Phase 7 pin; the +136 is 08B's `keywordp`/`setting-constant` and the reader's new symbols), and kg's default 1 MiB arena measures **1096 frames / 56223 object slots** — all three re-measured at this pin move through `FeMinimumArenaSize()` and `kg_lisp_arena_stats()`, not carried over, and `src/lisp_core.c`'s arena comment is corrected to them in the same commit. Two divergences from Emacs are fe's own, recorded in fe's corpus rather than kg's: a string escape decoding to 0 or to a value above 255 is a named error (Emacs stores a NUL and reads `"\400"` as U+0100), and a lambda parameter named `nil` or a keyword is refused (Emacs binds both; a parameter named `t` may shadow in both). kg-side: no source change was required — `make check` is green at the new pin at its full discovered count, and `ci-06`'s `clang-tidy` run over `fe.c`/`fe_eval.c` is clean. Fe's own caps at the pin: scc 746/760 (`fe.c` 140, `fe_eval.c` 489, per-file cap 520), pmccabe 1056/1056 across 339 symbols with the cap re-set at the measured actual, worst function 14/22; compat 306 cases, 246 passed, 60 known gaps, 0 failed. |
| **Exhaustion is a condition a handler can name, and a mark phase with a flat C stack** (Phase 9, sub-plans 09A–09C, fe `5a0503c` through `56c60f9`) | Five commits, and two of them change behaviour. `5a0503c` (09A) only *pins* what the tree did: arena exhaustion, GC-root-stack overflow and any named raise made while the arena was full were catchable by `(condition-case e X (t …))` and by nothing else, because the raise path degraded the condition object to nil and `ConditionMatches` — which must walk a pair to reach the hierarchy — then answered false for every named handler. `f282d60` is the funded cap raise (09A Decision 7). `306f258` (09B) is the fix: `FeOpenContext` interns and conses `(arena-exhaustion)` and `(evaluation-stack-exhaustion)` once, before any host code runs, and `CollectGarbage` marks both as roots for the context's whole life, so signalling either allocates nothing — which is the entire point. `FeHandleError` signals the first where it set nil (only under `!ArenaCanAllocate`, so kg's own natives, which call `FeHandleError(ctx, "out of memory")` for *host* `malloc` failures against a healthy arena, still raise an ordinary `(error "out of memory")`), `RaiseGcStackOverflow` signals the second, and `RaiseCondition` falls back to `(arena-exhaustion)` when a named **Error** condition cannot be built — deliberately not for a quit, which `ConditionMatches` decides by completion *kind* before it looks at the object's shape, so `(quit …)` keeps catching a nil-object quit. Neither name is new: both were already rows of `fe_eval.c`'s static hierarchy under `error`, reserved and never raised. `main.c`'s escaping-raise trace, which printed one line per live evaluator frame (measured 3128 lines for one OOM on a 3 MB arena), is bounded at 256 with a `… N more frames` tail. `d9269ea` (09C) replaces `FeMark`'s `car` recursion — the last unbounded data recursion in Fe, 48 bytes of C stack per `car` level, SIGSEGV between 130 000 and 150 000 levels on an 8 MiB stack — with Deutsch-Schorr-Waite pointer reversal that stores its return path in the objects it walks: no stack, no allocation, `sizeof(Value)` unwidened (bit 0 stays clear so `FeGetType` never lies, bit 1 is the existing `GcMarkBit`, bit 2 is the new `GcMarkCdrBit`). The recursive path is deleted, not wrapped. A host `mark_fn` may now read only the object it was handed — the graph is scrambled mid-walk, and `doc/c-api.md` says so. `56c60f9` re-sets fe's two Phase 9 caps at their actuals. | **Neither version moved, and this row records that decision against `fe.h`'s own contract text rather than inheriting it.** `FE_API_VERSION` stays **6**: the macro's own scope is “the C functions, types, and callback signatures below”, and 09B/09C add, remove and re-type none of them — `git diff acc94f7..56c60f9 -- fe.h` is empty, kg compiles at the new pin with no source change, and every call kg makes means what it meant before. `FE_LANGUAGE_VERSION` stays **7**: no name, form or reader rule joined the language; both condition names were already namable in a handler spec, and what changed is that a *host-resource* event — exhaustion of an arena whose size is an embedding parameter, a thing Emacs has no concept of — reports itself truthfully instead of substituting nil. The controlling precedent is two rows above: the Phase 7 review-fix cycle repaired a strictly narrower instance of this same defect (“a raise landing on the arena's last free cell collects before calling it exhaustion rather than silently substituting a nil condition that matches no handler”) and moved no version either. **The decision is contestable and the counter-argument is recorded here rather than left to a review.** Two of this table's own precedents point the other way. `FE_LANGUAGE_VERSION` 4 → 5 (06D) was bought with “every raise carries a symbol and a data list”; arena exhaustion was the last raise class for which that was false, so 09B can be read as *completing* the V5 language change rather than as repairing an implementation detail underneath it. And 5 → 6 (07B) moved because forms that used to answer now raise — a changed answer to a question a program can ask. `(condition-case nil BIG (arena-exhaustion 'caught))` is exactly such a question, and its answer moved: it used to escape and now returns `caught`. The decision stands on two grounds, neither of them “the old behaviour was a bug”, which would prove too much: the changed answer is about a *host-resource* event whose very existence is an embedding parameter, so no program that is portable across hosts could have depended on either answer; and Phase 7 decided the strictly narrower instance of this same question the same way, so moving the version now would make the two inconsistent without making either correct. A reader who weighs the V5/V6 readings higher should read this as a version bump kg declined, not as a question kg did not ask. `FeVersion` stays `"8.0"`, so `src/lisp_core.c`'s two `static_assert`s are unchanged and the pin's tripwire is silent by design. **One thing fe still owes, found in this audit and not fixed here** (09D edits no fe file but the gitlink): `fe.h`'s `FeGetCondition()` comment still reads “It is nil when the completion cannot construct an object, such as arena exhaustion” — the one example it names is now the one case that *does* construct one. 09B updated `doc/c-api.md` and left the header behind. It is a stale comment rather than a contract kg reads (kg calls `FeGetCompletion()` and `FeGetCompletionMessage()`, never `FeGetCondition()`), so it does not block this pin; it is reported for a fe-side correction. Measured at this pin move, not carried forward: `FeMinimumArenaSize()` **57304 bytes** (57016 at the Phase 8 pin; the +288 is 09B's two interned names and two pairs, which `GetCoreObjectCount` counts so an arena opened at exactly the minimum still holds them), and kg's default 1 MiB arena partitions to **56224 object slots / 1096 frames** with **465** live after a bare `FeOpenContext` (56223 / 1096 / 448 before) — one slot more, seventeen more live, frame capacity unchanged. `src/lisp_core.c`'s arena comment is corrected to those numbers in the same commit. kg-side adaptation: **none was required, measured rather than hoped.** kg never calls `FeGetCondition()`; it classifies by `FeGetCompletion()`'s kind and renders `FeGetCompletionMessage()`'s text, and 09B changes neither — an exhaustion still renders `out of memory` through `Lisp error:`, `Hook error:` and `Init file error:`, now with a condition object behind it. `make check` at the new pin before any 09D feature: **32 native / 429 PTY, 0 fail, 0 skip**. Fe's own caps at the pin: scc **757/757** (`fe_eval.c` 490, `fe.c` 147, per-file cap 520), pmccabe **1065/341 symbols** (limit 1065), worst function 14/22 — both totals re-set at their measured actuals by `56c60f9`, so Phase 9 spent +11 scc and +9 pmccabe of the 74 and 64 points it was funded. |
| Phase 9 post-close review fixes (fe `feb6d65` through `7499f71`) | The twelve commits kg's Phase 9 review put on the branch, `feb6d65` first and `7499f71` last; corrections inside 09B/09C rather than new surface, and two of them change what a host can do. `26fea30` (the review's blocker): since 09C the mark phase stores its return path *inside* the objects it walks, so a `mark_fn`/`gc_fn` that leaves non-locally abandons the arena half-reversed — reproduced as a SIGSEGV on 56c60f9 against an intact heap on the recursive walk it replaced. The contract is now stated in `fe.h` at `FeSetMarkFn`/`FeMark` and in `doc/c-api.md` (a callback must return normally, must not raise, and must not allocate), *and* enforced: `FeContext::collecting` is true for exactly the duration of `CollectGarbage`, which refuses re-entry, `RaiseCompletionCore` refuses a raise from inside it, and both print which rule broke and `abort()`. The one in-tree route to it is closed rather than merely documented — `WriteObject` charges no step and polls no interrupt while collecting, so the obvious printing callback stays legal. `a8d91cd`: 09B's two pre-built conditions are shared for the life of the context and handed to the handler itself, so one `(setcar e 'poisoned)` disabled the whole mechanism permanently (reproduced: every later out-of-memory then escaped `(error …)` and `(arena-exhaustion …)` alike, and a `setcdr` payload reached the *next*, unrelated exhaustion and was kept alive by the context-lifetime root). `PublishExhaustion` re-stamps symbol and nil into the pair before every publish — two stores, no allocation, so the property these objects exist for is untouched. `fd8ece9` corrects `fe.h`'s `FeGetCondition()` comment, which is the debt the row above reports as still owed: it now states the rule in both directions, and `test_api.c` asserts what the *host* sees rather than only what Lisp reads. `f1eda54` exports the writer's depth bound as `FeWriteDefaultMaxDepth` so `main.c`'s trace bound stops restating it, adds a `static_assert` for the pointer-reversal alignment premise, and makes the sweep assert that no `GcMarkCdrBit` leaked. `15e0d7a` adds the `deep-trace` golden that first executes 09A Decision 5's dropping arm (`… 7 more frames`); `ba3b5b0` re-derives the fuzz seeds 09C's grammar change silently unsteered; `b2d29a4` replaces a vacuous depth witness and pins the other two Budget walls; `0c90b3d` replaces the figures nobody could reproduce (including the `ulimit -s 1280` claim kg itself falsified) with measured ones; `13959bf` and `a801013` are an fd leak in the new fork test and a `.gitignore` gap. `feb6d65` funds the cycle's caps and `7499f71` re-sets them at the actuals (scc 765/765, pmccabe 1072/1072). | **Neither version moved, and neither did `FeVersion`.** `FE_API_VERSION` stays **6**: `fe.h`'s diff over this range is comment text plus one *additive* declaration, `enum { FeWriteDefaultMaxDepth = 256 }`. Nothing is removed, renamed or re-typed, so every existing host — kg included — compiles and behaves as before; kg does not use the new enum and has no printer of its own to bound. `FE_LANGUAGE_VERSION` stays **7**: no name, form or reader rule joined or left the language. The re-stamping fix restores the behaviour 09B's row already claims rather than defining a new one, and the mark-callback rule is a C-embedding contract with no Lisp surface at all. `FeVersion` stays `"8.0"`, so `src/lisp_core.c`'s two `static_assert`s are unchanged and the pin's tripwire is silent by design — verified by compiling, not by reading. **The one new host-facing rule, checked against kg rather than assumed:** a raise or an allocation from a `mark_fn`/`gc_fn` is now fatal. kg never calls `FeSetMarkFn` (`grep` over `src/`: no call site), and its one `FeSetGCFn` callback, `lisp_obj.c:lisp_object_gc`, clears two fields of a host pool record and returns `&nil` — no allocation, no raise, normal return — so the new abort is unreachable from kg and **no adaptation was required for it.** Measured at this pin move, not carried forward: `FeMinimumArenaSize()` **57328 bytes** (57304 at the 56c60f9 pin; the +24 is the three new `FeContext` fields — `collecting` and the two interned exhaustion names), and kg's default 1 MiB arena partitions to **56222 object slots / 1096 frames** with **465** live after a bare `FeOpenContext` (56224 / 1096 / 465 before) — two slots fewer, frame capacity and live count unchanged. **That two-slot move is the whole kg-side adaptation, and it is not free:** the slot count is asserted literally in four PTY cases and quoted in three comments, all corrected in this same commit — `src/lisp_core.c`'s arena comment (and its 57304), `test/test_lisp.c`'s margin comment, `test/pty/lisp-arena-stats-command.yaml`, `lisp-exhaustion-mid-init-visible.yaml` (`0 free, peak 56222`), `lisp-exhaustion-mid-command-recovers.yaml`, `lisp-exhaustion-mid-hook-reports.yaml`, `test/lisp-compat/features.json` and `doc/lisp-api.md`. The free-slot figures those cases document moved with it and were re-measured, not adjusted: 51034 free after the prelude (51036), 51503 after the recovers case's post-exhaustion collection (51505). `make check` at the new pin: **32 native / 434 PTY, 0 fail, 0 skip**. Fe's own caps at the pin: scc **765/765**, pmccabe **1072/343 symbols** (limit 1072). |
| **Expansion without evaluation: `macroexpand-1` and `macroexpand` as primitives** (Phase 10, sub-plan 10B, fe `d293128` through `a51f031`) | Five commits, and the pair is the whole Phase 10 language surface. `d293128` adds `macroexpand-1 FORM &optional ENVIRONMENT` (one step) and `macroexpand` (the fixpoint), both ordinary function-shaped primitives, so FORM is an evaluated operand and a caller quotes it, and both reachable through `funcall`/`apply` as in Emacs. There is no second transformer-application path: the `FeTMacro` arm of `DispatchResolvedCall` was factored out as `EnterMacroBody` and is now the only place a transformer is applied, which is what makes strict arity under a reflective expansion the *same* check with the same `(FUNCTION NARGS)` data as an ordinary macro call. Termination is the step budget's job rather than a special case, measured in `TestMacroexpandBudget`: a self-expanding macro ends at `evaluation step limit exceeded` with `peak_frame_depth` under 16, so it is the step wall that stops it and not the frame wall. `macroexpand-all` is a primitive that raises a *named* `unsupported feature: macroexpand-all`, deliberately not `void-function`, which is byte-identical to what a typo produces (10A Decision 5); a non-nil ENVIRONMENT is refused the same way (`unsupported feature: macroexpand environment`) rather than ignored, so a caller passing Emacs' alist is told the feature is missing instead of getting an answer computed as if the argument were not there. Both are catchable message-level conditions. `a5f8566` is fe's own post-implementation differential against the same pinned oracle, and it corrects the alias rule to `subr.el`'s: an alias head is rewritten **only when the target is itself a macro**, so `(macroexpand-1 '(pf 1))` where `pf` aliases an ordinary function is `(pf 1)` and not `(plainfn 1)`, and an alias to an unbound name is likewise returned untouched. That resolution also turned an alias ring from a step-budget death into an immediate `cyclic-function-indirection` — fe policy, not an oracle reading, because Emacs' own `defalias` refuses to close the ring at definition time and so has no answer here to copy. `69444da` is the compat half (fourteen new `comparison: emacs` cases, five manifest entries, every snapshot runner-produced) and `a51f031` re-sets fe's caps at the slice's actuals. | **`FE_LANGUAGE_VERSION` moved 7 → 8 and `FeVersion` `"8.0"` → `"9.0"`; `FE_API_VERSION` stays 6**, so kg's language `static_assert` fired at this pin exactly as designed and the API one did not — verified by compiling, not by reading: `make` at the moved gitlink stopped at `src/lisp_core.c:52: static assertion failed`, and that one-line edit is the reconciliation. The bump is recorded above in this file's version paragraph together with the argument fe's own commit makes against it (`doc/c-api.md`'s "compatible additions do not require a bump", the rule 04C's seven primitives landed under) and for it (the macro's single consumer is this `static_assert`, and a macro that does not move cannot tell kg whether the fe it links against has the names). kg agrees with the bump and did not re-litigate it. **kg-side adaptation, measured rather than assumed:** no kg C, prelude or Lisp source calls, defines or shadows any of the three names (`grep` over `src/`, `lisp/`, `test/`), and no kg source uses a `defalias` ring, so nothing in the editor changed behaviour. The three new fe primitives are claimed by fe's own manifest, so kg's `check_lisp_compat.py` source-coverage gate passes with the census at **59 fe primitives/aliases, 81 kg natives, 77 kg prelude definitions** and **357 features across both manifests, 0 problems** (352 before). What the pin *did* cost is arithmetic on kg's fixed arena, and every figure below was re-measured here, never carried forward: `FeMinimumArenaSize()` **57680 bytes** (57328 at the `7499f71` pin; the +352 is the three primitive symbols, their names and the two `unsupported feature:` message paths), and kg's default 1 MiB arena partitions to **56224 object slots / 1096 frames** with **487** live after a bare `FeOpenContext` (56222 / 1096 / 465 before) — two slots *more*, twenty-two more live, frame capacity unchanged. Those two slots are exactly the two Phase 9's three new `FeContext` fields had taken, so the count is back where Phase 8 left it, and every literal that names it is corrected in this same commit: `src/lisp_core.c`'s arena comment (and its 57328), `test/test_lisp.c`'s margin comment, `test/pty/lisp-arena-stats-command.yaml`, `lisp-exhaustion-mid-init-visible.yaml` (`0 free, peak 56224`), `lisp-exhaustion-mid-command-recovers.yaml`, `lisp-exhaustion-mid-hook-reports.yaml`, `test/lisp-compat/features.json` and `doc/lisp-api.md`. The derived figures those comments document were re-measured too, not adjusted by the delta: 51014 free after the prelude (51034), 51483 after the recovers case's post-exhaustion collection (51503), `peak_live` 5210 after the prelude (5188) and 5751 with `lisp/auto-fill.el` on top of it (5729). **One of them did not reproduce and is corrected rather than shifted:** `test/test_lisp.c`'s "8230 at this point in this function" measures **8120** at the old pin and **8142** at this one on this build, so the comment now carries a measured pair and says the 8230 never reproduced. `make check` at the new pin, before any 10C feature work: **32 native / 434 PTY, 0 fail, 0 skip**. Fe's own caps at the pin: scc **787/787**, pmccabe **1088/347 symbols** (limit 1088), `fe_eval.c` **517/520** — three points of file-cap headroom, which fe's own closing commit flags as the number to price against before touching that file again. |
| Phase 10 acceptance-review fix (fe `6355f7f`) | `macroexpand-all`'s reject-by-name stub had been entered only from direct call position. Although the documentation called it a primitive, Fe did not classify it as function-shaped, so `(funcall 'macroexpand-all ...)` and `(apply 'macroexpand-all ...)` raised `invalid-function` instead of the promised `unsupported feature: macroexpand-all`, and the operands of those reflective calls were not evaluated under the same path. The fix sends direct, `funcall` and `apply` calls through the ordinary evaluated-argument continuation and then raises the same named refusal; API and script tests pin all three routes and operand side effects. The review also corrected the stale macroexpand fixpoint account (self-expansion is budget-limited; an alias ring is rejected immediately as `cyclic-function-indirection`) and made the three deliberately unsupported manifest rows state their actual plain-`void-function` behavior rather than implying a known-name channel Fe does not have. | No version or arena figure moved: the named primitive and its representation were already part of the `a51f031` pin, and this corrects call routing plus records. The change is scc/pmccabe-neutral: fe remains **787/787**, **1088/1088 across 347 symbols**, with `fe_eval.c` **517/520**. Focused acceptance evidence: `make check`, `make complexity-check`, `make pmccabe-check`, `make format-check`, and `make compat` (**322 cases, 260 pass, 62 known gaps, 0 fail**). |
| `after-change-functions` callback shape | Emacs calls callbacks with `(BEG END PRE-LENGTH)`; kg passes `(BUFFER BEG END OLD-LEN)` | deliberate kg divergence, retained until the Phase 8 init-file compatibility work |
| An unassigned symbol is `void-variable`, not `nil` | a typo was silently false; Fe's own `TODO.md` asked for this | `boundp` and `makunbound` are new primitives and `FeIsBound()` is a new API; kg's `defvar` asks `(boundp 'name)` rather than evaluating the name |
| `FeCallWithOptions()` — a controlled `FeCall()` | kg ran Lisp commands through a source-string trampoline (`(internal--run-pending-command)`) solely to reach the evaluator's step-budget/interrupt/GC accounting; the trampoline is gone now that a callable can be invoked under the same options | one declaration in `fe.h` and a thin wrapper reusing `BeginEvaluationControl`/`EndEvaluationControl`; tested in `test_api.c`, no `FE_API_VERSION` bump (compatible addition) |
| `unwind-protect` and `FeProtectWithCleanup()` — cleanup stack and host protection | Lisp `unwind-protect` and C `FeProtectWithCleanup` share a single LIFO registry; cleanups run on normal return, Lisp error, C-g interrupt, and step-budget exhaustion | new primitive `unwind-protect`, `FeProtectWithCleanup()` API in `fe.h`, fresh per-entry cleanup step budget and interrupt re-arming in `RunCleanupsAfterError` |
| An explicit depth counter first bounded recursion, in addition to `GcStackSize`; superseded below | Early hardening work (before the frame machine existed) added an `evaluation_depth` counter checked on every pair-form `Evaluate()`, because the GC stack bounded recursion only by accident, via how many slots a call happened to consume: a build with fatter per-call C frames than the default build (any sanitizer, `-O0`, a debug build) could exhaust the real C stack before the GC stack noticed, crashing instead of raising a catchable error — confirmed under `.ci/ci-05`'s MSan flags, which crashed at `(deep 418)`. That counter, and the single number it bounded, is gone: the frame-machine row below and the two-bounds row after it are what replaced it. | superseded; see below |
| **The recursive evaluator replaced by an explicit frame machine** (sub-plans 03A–03E, kg-side) | `Evaluate()` and its helpers called each other recursively in C, one C stack frame per Lisp nesting level — the `evaluation_depth` counter above only detected that a build's C stack was about to run out, it did not stop Lisp nesting from costing C stack at all. The frame machine (03C's arena-resident substrate, 03D's call frames, 03E's special-form frames, finishing with the recursive evaluator's deletion) roots Lisp nesting — nested calls, nested special forms, self-expanding macros, deep argument lists — in the context-owned arena instead: each level pushes a `FeEvalFrame` and returns to a `RunEvaluationLoop` trampoline rather than recursing in C, so Lisp nesting costs a *constant* amount of C stack no matter how deep it goes. This is what makes `(deep 100000)` runnable at all (see the `GcStackSize` row above) and what makes MSan-safe deep recursion possible without an unbounded C stack. | Fe: `fe_eval.c`'s frame machine (`FeEvalFrame`, `frame_stack`, `RunEvaluationLoop`, per-special-form `Resume*` continuations replacing the old recursive `case` arms); `fe.c`/`fe_eval.c` split (sub-plan 03B) to keep both under the per-file scc cap; `EvaluatePrimitive`'s decomposition (03E) paying back some of the split's own cost. kg: no source change — the frame machine is behaviour-neutral by design; only the arena-layout comment (03C) and, in this slice, the API-facing bound rename below actually touch kg. |
| A cons's second operand rooted only across the whole reduction, not across the allocation that could collect it (`f1c0dde`) | `ResumeBinary`'s pair-argument continuation cleared `frame->callee` — the second operand's only collector root — before calling `PCons`, whose `FeCons` allocates; a collection landing inside that allocation produced a pair whose cdr pointed at freed memory. Found and fixed alongside the seed that triggers it (`fuzz/seeds/eval/cons-second-operand-gc`), not by inspection. | Fe: `ResumeBinary`'s `frame->callee = &unbound` moves from before the primitive switch to after it, so the field still roots `second` across `PCons`'s `FeCons`; the stated rule is that a frame field stays live until the last operation that might allocate has finished with it. A regression seed is checked in under `fuzz/seeds/eval/`, replayed by `make fuzz-*-smoke`, because the trigger depends on accumulated arena state across many expressions and so cannot practically be pinned by a deterministic `test_api.c` case. |
| **Two separate bounds replace the single evaluation-depth counter, with an `FE_API_VERSION` break** (sub-plan 03F) | The frame machine's arena-resident frames made a *slot-count* bound possible instead of a C-recursion-count one, and re-exposed a real gap: `save-excursion`/`with-current-buffer`-style natives that call back into the evaluator synchronously still cost real C stack per level, a bound the old single counter conflated with ordinary Lisp nesting. `FeEvalOptions.max_depth` split into two fields with different meanings, so every host that set the old field now gets a compile error instead of silently different behaviour — exactly why this is an `FE_API_VERSION` bump (1 → 2) and not a compatible addition, unlike `FeCallWithOptions()` below. | Fe: `FeEvalOptions.max_frames` (Lisp nesting; zero selects the arena's `frame_capacity`, "evaluation frame limit exceeded" on the next push once the effective limit is reached) and `max_native_reentry` (nested evaluator runs started synchronously from a native; zero selects `DefaultNativeReentry` = 32, "native evaluation re-entry limit exceeded" past it) replace `max_depth`; `DefaultNativeReentry` is 10x the deepest synchronous re-entry sub-plan 03F found by grepping kg's own Lisp prelude and PTY corpus (`with-current-buffer` wrapping `save-excursion`, depth 2, plus one level for a hook/process callback invoking that pattern — 3 in the deepest real construct found). `FE_API_VERSION` 1 → 2; `FeVersion` `"2.0"` → `"3.0"`; `FE_LANGUAGE_VERSION` unchanged at 2, since no language behaviour moved. kg: gitlink move, `FE_API_VERSION == 2` compile-time assertion, `struct kg_lisp_arena_stats`/`kg_lisp_arena_stats()`/`KG_PERF_LISP_*` mirror the renamed and new fields (`src/lisp.h`, `src/lisp_core.c`, `src/perf.h`, `src/perf.c`); `eval_options` in `src/lisp_core.c` already omitted the old `max_depth` field, so both new zero defaults needed no caller edit. `test/test_lisp.c`'s `test_recursion_depth` now expects `evaluation frame limit exceeded` at a literal (`(deep 1000000)`) measured well above kg's arena-derived `frame_capacity` (1100 on kg's default 1 MiB arena as 03F measured it, 1098 since the Lisp-2 row's new primitive symbols; ~3 frames per `(deep N)` recursion level) rather than at the old fixed `(deep 5000)`. |
| `FeGetArenaStats()` — read-only arena/evaluator statistics | Neither Fe nor kg had any way to answer "how close is the fixed arena to full", which sub-plan 00D of kg's Emacs-subset program needs as a Phase 0 baseline, ahead of the Phase 9 diagnostic surface this pulls forward from. Sub-plan 03F renamed three of its fields alongside the bound split above: `peak_evaluation_depth` split into `frame_capacity` (the arena's host-usable frame capacity), `peak_frame_depth` (high-water mark of live ordinary frames, not the old logical pair-depth) and `peak_native_reentry`. | `FeArenaStats` struct and accessor in `fe.h`/`fe.c`: `total_slots`, `free_slots`, `peak_live_objects`, `collection_count`, `peak_gc_stack_depth`, `frame_capacity`, `peak_frame_depth`, `peak_cleanup_stack_depth`, `peak_native_reentry`, `allocation_failures`; counter fields on `FeContext` updated at their existing sites (`MakeObject`, `CollectGarbage`, `FePushGC`, the frame-machine's push/pop, `PushCleanup`); allocates nothing itself. kg mirrors all of it in `struct kg_lisp_arena_stats` (`src/lisp.h`). No kg-visible command yet — that is Phase 9. |
| Core `setq`/`set`, and `=` cut from assignment to left-to-right chained numeric equality — `FE_LANGUAGE_VERSION` 1 → 2 | Emacs' `setq` updates the innermost lexical binding (writing a new global otherwise) and `set` always writes the global cell straight through a same-named lexical binding; neither is `=`, which Emacs uses for numeric comparison. Sub-plans 02A–02D pinned the oracle answers, landed both new forms beside the old assignment `=` for one coexistence window, then cut it: kg's own prelude `setq` macro (built on the old assignment primitive) is gone, replaced by the core special form, and every kg-owned `(= NAME VALUE)` became `(setq NAME VALUE)`. There is no assignment-`=` compatibility alias — §0.4 of the parent plan: no known external users, so no reason to keep the old spelling reachable. | Fe: new `PSetq`/`PSet` enums and `EvaluateSetq()`/`EvaluateSet()`, the old assignment enum and switch arm deleted, a chained `EvaluateNumericEqual()` replacing the old two-argument `=`; `FeVersion` `"1.1"` → `"2.0"`; `fe/doc/language.md` rewritten. kg: gitlink move, `FE_LANGUAGE_VERSION == 2` compile-time assertion beside `FE_API_VERSION == 1`, `lisp/prelude.el`'s `setq` macro definition deleted and its remaining 53 top-level forms rewritten from `=` to `setq`, `.fe` discovery (init file, bare `load`/`require`) cut to `.el` with no fallback — an explicit `.fe` *path* (containing `/`) still loads, since the cut is discovery-only |
| `fe.c` split into `fe.c` + `fe_eval.c` behind a private `fe_internal.h` — structural, no language or embedding-API change | Sub-plan 03A priced Phase 3's frame machine and found the file cap, not the aggregate, was what forced a split: `fe.c` scored 106 of a 112 file cap with the frame machine's substance not yet written, and the evaluator was the region scc's `'"'`-literal desync (from `fe.c:1010`) hid from measurement entirely. Sub-plan 03B landed the cut on its own, mechanically: evaluation control, the `unwind-protect`/`FeProtectWithCleanup` cleanup registry, `FeHandleError` (moved with the registry per 03B's own recommendation — everything it calls on the error path was moving anyway), the recursive evaluator (`Evaluate`, `EvaluateHead`, `EvaluatePrimitive`, `EvaluateList`, `DoList`, `Bind`, `ArgsToEnv`, `EvaluateSetq`, `EvaluateSet`, `EvaluateNumericEqual`, `CheckNumericEqualOperand`, `HandleVoidSymbol`, `HandleNonCallable`, the `EVAL_ARG`/`ARITH_OP`/`NUM_CMP_OP` macros) and its entry points (`FeEvaluate`, `FeCall`, `FeCallWithOptions`, `FeEvaluateWithOptions`) moved into the new file; the reader, writer, and root management (`FeCreateRoot`/`FeGetRoot`/`FeReleaseRoot`) stayed in `fe.c`, as did the kept wrappers (`EvaluateInput`, `FeEvaluateString*`, `FeEvaluateFile*`) that call the now cross-TU `BeginEvaluationControl`/`EndEvaluationControl`. Proved mechanical by pmccabe's per-function sum, conserved exactly: 500 across 202 symbols before and after. | Fe: `fe_internal.h` (object layout, `CAR`/`CDR`/etc. macros, `struct FeContext`, the `Primitive` enum, and the small accessors both files use — `GetDouble`, `GetNativeFn`, `SetType`, `CheckType`, `GetBound`, `MakeObject`, `Equal`, `IsNamedSymbol`, `Format`, plus `unbound`, all of which lost `static`); `fe/test_internal_header.c` compiles the private header standalone under both core compilers; `Makefile`'s `SRCS`/`CORE_OBJS`/fuzz link rules all name both files (`FE_CORE_OBJS` list). kg: `FE_OBJ`/`FUZZ_FE_OBJ` became two-object lists, eleven Makefile sites (link lines, the submodule-population guard, `clean`) updated to match, and `lisp-include-check` extended to forbid `fe_internal.h` anywhere in `src/` the same way it already forbids `fe.h` outside the adapter. |
| **The protected string evaluation `FeTryEvaluateStringWithOptions()` — `FE_API_VERSION` 7** (Phase 11, fe sub-plan 11C) | The sibling `FeTryCallWithOptions` (version 5) had needed since a host that *loads* Lisp from inside an evaluation has the same problem a host that calls a callback has, and could not solve it the same way. kg's `lisp_eval_file()` re-entered fe through `FeEvaluateString`, a nested run dressed as a top-level call, so a condition raised by the loaded text transferred straight to the outermost barrier — past every `condition-case` between the `(load ...)` and the raise. The new entry contains the completion and returns false with the caller's frame still live, so kg unwinds its own loader bookkeeping and re-raises with `FeResignal` once the enclosing run's floor is back. A `throw` out of the evaluated text is contained as the barrier-wall error it already is, exactly as the protected call's is; making *that* reach an enclosing `catch` needs `load` to be an fe primitive with a frame kind of its own and is deliberately out of scope (11A Decision 5), recorded as kg's `load-throw-reachability` row. | `FeTryCallWithOptions` nearly verbatim with `EvaluateInput` in place of the call, plus a save/restore of `ctx->evaluation_result` the protected call does not need. kg-side: `src/lisp_io.c`'s `lisp_eval_file` and the three bookkeeping lines that were unreachable on the error path before |
| `fe_eval.c` split into `fe_eval.c` + `fe_run.c` behind the same private `fe_internal.h` — structural, no language or embedding change (fe sub-plan 11B) | `fe_eval.c` stood at 517 of its 520 per-file complexity cap with the dynamic-binding work still to land in its binding section. A translation-unit split is what keeps that cap binding at full strength rather than raising it; the split is complexity-neutral by construction, since `check_scc_complexity.py` sums per file, and it moves no code — the run driver, the barrier installation and the public `FeEvaluate*`/`FeCall*` surface are the functions `fe_eval.c` already held, unchanged. | kg compiles submodule translation units **by name**, so this is a named kg-side pin adaptation and not something a wildcard picks up: `FE_OBJ` and `FUZZ_FE_OBJ` in kg's `Makefile` gained `src/fe_run.o` and `test/fe_run_fuzz.o`, each with the same `fe/fe.h` + `fe/fe_internal.h` prerequisites commit `4414999` established |

## The nested tiny-regex-c submodule

The pin chain is **kg → fe → tiny-regex-c**. `fe/.gitmodules` pins
`tiny-regex-c` on the **`adapt-to-fe` branch** of `github.com:bjodah/tiny-regex-c`,
and moving that pin means moving fe's gitlink and then kg's, in that order.

This is not at arm's length. kg compiles `fe/tiny-regex-c/re.c` **directly**
into the editor (`Makefile`'s `$(OBJDIR)/tiny_regex.o`, plus the regex fuzzer
and the differential driver, with `-Ife/tiny-regex-c` on both `CFLAGS` and
`FE_CFLAGS`), so every engine change ships to kg users whether or not Fe's own
`fex_re.c` is built. It is also compiled in **both** `WITH_LISP` configurations.
`src/regex.c` and `src/regex.h` are kg's only consumers of `re.h`.

To move the pin: land the change on `adapt-to-fe` with the submodule's own
`make check` and `.ci/run-ci-steps.sh` green, move fe's gitlink with
`make -C fe check`, then move kg's and run `make check`,
`make check-regex-differential`, `make fuzz-regex-seed-replay`,
`.ci/run-ci-steps.sh` and `.ci/ci-08-with-lisp-0.sh`.

Growing `re_status` is an ABI change that has to move `fe/fex_re.c`,
`src/regex.c` and `test/regex_differential.c` in the same pin step; prefer
reusing an existing value.

### kg-visible divergences from upstream kokke/tiny-regex-c

| Divergence | Why | Cost |
| --- | --- | --- |
| Emacs-style escaped operators: `\(...\)`, `\|`, `\{n,m\}`; bare `(`, `)`, `\|`, `{` are literal | kg presents regexps as Emacs' | patterns in kg's docs, tests and Lisp are Emacs-shaped |
| The matcher steps by UTF-8 character, not by byte | `å*` must repeat `å`, and `[åä]` must not match the `0xC3` they share | spans and `start_offset` stay byte offsets; `re.h` says so |
| A construct the engine cannot honour is `RE_STATUS_BAD_PATTERN`, never literal text | `[[:blank:]]` used to match `a`, out of the letters spelling the class name | an exact documented subset in `re.h`; kg surfaces it as `KG_REGEX_BADPAT` |
| An unclosed `\(`, an unmatched `\)` and an unterminated bracket expression are bad patterns; a `]` in first position is a member | `\(\(a\)` and `[a` used to compile and quietly mean something else | one compile-time parse stack replaces two scans over the half-written program |
| A quantifier on an already-quantified atom (`a++`, `a\{2\}\{3\}`, `a*?`) is a bad pattern | Emacs folds `a++` and `a\{2\}\{3\}` into one repetition and reads `a*?` as its non-greedy operator; kg has neither spelling, and used to compile a node that could never match | `utils/regex_differential.py` generates it as the tagged `double-quantifier` acceptance difference |
| A pattern with a tenth `\(` is a bad pattern | `RE_MAX_SPANS` is 10 — the whole match plus nine groups — so the tenth has nowhere to be reported; Emacs has no limit | tagged `group-count` in `utils/regex_differential.py`; `test/test_regex.c` walks 1 to 11 groups |
| Reaching a budget abandons the whole attempt | the alternatives not yet tried and the start positions not yet swept go with the branch that ran out, so `\(a\)\{300\}\|b` over 256 `a`s and a `b` is `TOO_COMPLEX` where Emacs reports the `b` | conservative by design: kg never reports "not found" because it stopped looking |
| `*`, `+`, `?` and `\{` with nothing to repeat are the literal characters | Emacs reads them that way at the start of a pattern, group or alternative | `\(?` is accepted as a literal `?` where Emacs reserves it for shy groups |
| Caller storage must be `RE_STORAGE_ALIGNMENT`-aligned or the compile is refused | the program's multi-byte fields are read in place; a misaligned buffer aborted under UBSan | `struct kg_regex` declares `alignas(RE_STORAGE_ALIGNMENT)` rather than relying on its own layout |
| The matching budget is per execution, with `re_exec_with_options()` for per-call limits and cancellation | it used to live in file-scope statics, so a nested or concurrent match spent the running one's allowance | kg still calls plain `re_exec()`; the options are available for a future C-g-aware search |
| Reaching the group-repetition ceiling is `RE_STATUS_TOO_COMPLEX` | it was a false no-match, and for `\(a\)*` a match shorter than the pattern asked for | kg reports `KG_REGEX_TOO_COMPLEX`, which the search UI already distinguishes from "not found" |
| Under `__FreeBSD__` only, `re.h` includes `<unistd.h>` and then `#define re_exec tre_re_exec` (tiny-regex-c `63aa624`) | FreeBSD's `<unistd.h>` declares the removed 4.2BSD `int re_exec(const char *)` inside its `__BSD_VISIBLE` block, which is on by default there, so any translation unit including both headers is a "conflicting types for `re_exec`" error — and `src/def.h` includes `<unistd.h>`. Renaming beats asking for a feature-test macro that would hide the declaration, because the rest of that block (`mkdtemp()`, which kg's tests use) stays visible. | Nothing outside FreeBSD changes name or behaviour, and no caller spells the new name. The guarded `<unistd.h>` is what keeps it out of kg's source entirely: without it the rename is include-order-sensitive (a unit reaching `re.h` first renames unistd.h's declaration too and conflicts under the new name), and pushing `def.h` ahead of `regex.h` in `src/regex.c` is not a stable fix — clang-format's main-header rule puts `regex.h` first there, so `make format-check` exits 2 and undoes it. |

Fex — `fex*.c`, which kg does **not** compile — also gained a file-lifecycle
owner and finalizer, exact byte handling instead of fixed-size renderings, and
`waitpid` EINTR retry. Those are listed here only so an update is not surprised
by them; they cannot reach kg.

Deliberately **not** changed in `fe.c`, and why:

- **Quasiquote semantics.** `quasiquote` is a prelude macro in
  `lisp/prelude.el`, so it can change without moving the pin. The core
  only learns the punctuation.
- **Vectors, hash tables, keyword arguments, a byte compiler.** All need
  new object types beyond what the current core provides. Vectors are
  additionally a *named read error* rather than a misreading, per the
  Phase 8 reader row above.  This bullet used to open with "Dynamic
  binding"; Phase 11's special-variable row above is where that stopped
  being true, and the reason it was wrong is instructive — shallow
  binding needs no new object type at all, only two bits on a symbol and
  a frame that remembers what it overwrote.

This list used to carry a third entry, `?a` character literals, whose
rationale was that Fe has no character type and that a byte-at-a-time
`FeReadFn` would read `?é` as its first UTF-8 byte. Both halves died —
the first with Phase 5's `FeTInteger`, the second on inspection, since a
UTF-8 lead byte states its own length — and Phase 8's reader row above
implements the literals: `?é` is 233, exactly as in Emacs.
