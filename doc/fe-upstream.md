# Fe upstream

kg embeds the core of [Fe](https://github.com/bjodah/fe) through the `fe/` git
submodule. The submodule tracks the **`analyzers-etc` branch** of
`github.com:bjodah/fe`; that branch name is the pin. The exact commit is
recorded automatically by git as part of `fe/` being a submodule — the
superproject's tree stores the SHA the working tree is checked out at, and
`git submodule status` prints it — so no commit hash is repeated here. A hash
written into prose only goes stale, as it did before this document was
rewritten.

The supported embedding interface is `FE_API_VERSION 1`; `src/lisp_core.c`
asserts it at compile time. Fe's *language* — its evaluated behaviour,
independent of the C embedding contract — is versioned separately as
`FE_LANGUAGE_VERSION 2`, which `src/lisp_core.c` also asserts at compile
time, beside the API assertion. The two move independently: language
version 2 is the `setq`/`set`/numeric-`=` hard cut below, which broke no
C function, type, or callback contract, so `FE_API_VERSION` stayed at 1
through it.

Fe is MIT licensed. Copyright belongs to rxi and Chris Palmer; the complete
license text is in `fe/LICENSE`.

kg compiles `fe/fe.c` and `fe/fe_eval.c` (sub-plan 03B split the evaluator
out of `fe.c` into its own translation unit, behind a private
`fe/fe_internal.h` the two share) and their public header `fe/fe.h`. The
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

1. Fetch `origin` in `fe/` and check out the tip of `analyzers-etc` (or the
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

## kg-side divergences from upstream rxi/fe

These changes live on `bjodah/fe`'s `analyzers-etc` branch. They exist because
kg presents Fe to users as an Emacs Lisp dialect, and the remaining Emacs
surface is bought in kg's own prelude in `src/lisp_prelude.c`. Anything
that could be done in the prelude was done there instead.

| Divergence | Why | Cost |
| --- | --- | --- |
| `if` is Emacs Lisp's `(if COND THEN ELSE...)`, with the trailing forms an implicit `do` | Upstream read them as an alternating elif chain, so `(if c a b1 b2)` silently evaluated `b1` as a *condition* and never ran `b2`. Two forms cannot coexist in one language; the elisp one wins. | `scripts/concatenate.fe` and `scripts/life.fe` were rewritten as nested `if`s; `doc/language.md` updated |
| The `fn` primitive's canonical name is `lambda`; `fn` remains bound to the same primitive | `(lambda (x) …)` is what elisp users write, and closures now *print* as `(lambda …)` rather than `(fn …)` | one entry in `primitive_names`, a `primitive_aliases` table, `type_names[FeTFn]`, one string in `FeWrite` |
| Reader macros `` `x ``, `,x`, `,@x` read as `(quasiquote x)`, `(unquote x)`, `(unquote-splicing x)` | Backquote is the difference between writing macros and fighting them. The *semantics* stay in kg's prelude; only the punctuation had to move into the reader. | `` ` `` and `,` became symbol delimiters (nothing in fe or kg used them inside symbols) |
| Reader macro `#'x` reads as plain `x` | Upstream read `#'car` as a symbol named `#'car`, which evaluated to `nil` *silently*. Fe has one namespace, so the elisp function quote is the identity. | `#` stays an ordinary symbol character; only the two-character `#'` is special |
| Calling a non-function whose head is a symbol raises `void-function NAME` | Upstream's `tried to call non-callable value` never said which name was unbound | one helper in `Evaluate` |
| `GcStackSize` 512 → 4096 | The GC stack, not the C stack, bounds recursion: upstream died at about 70 frames, which is too few to write ordinary list code. Sub-plan 03E (Phase 3's last slice) deleted the recursive evaluator entirely, so this is no longer even a recursion bound under the default configuration — `evaluation_depth`'s own 1000-unit ceiling fires first, at logical depth 333 for the canonical `(deep N)` chain, hundreds of levels before this array is anywhere near full. It becomes live again, as a fixed, arena-independent ceiling distinct from the physical frame wall, only once a caller raises `max_depth` far past its default: the frame machine's lambda-body wrapper retains two entries on it (`FePushGC(env)`/`FePushGC(rest)`) per still-open level, not popped until that level's whole activation unwinds, so `(deep 1021)` succeeds and `(deep 1022)` raises `GC stack overflow` with `max_depth` raised to clear both other walls. (03E's own development found and fixed a related regression first: an `if`'s single-form false branch, its own most common shape, must push that form directly rather than through a generic implicit-body wrapper, or the *physical frame* wall — not this GC-stack one — binds at N ~ 274 instead of the intended N = 333, since kg's 1 MiB arena's 1100-frame capacity was sized for 3 simultaneously-open frames per level, not 4.) `fe/test_api.c`'s `TestFullDeepFlatness` records both; 03F's two-bounds redesign is where the GC-stack one should be resolved (a larger array, or moving `env`/`rest` protection off this stack into the already arena-resident frame the way 03A/03C's whole frame-substrate decision did for everything else). | `FeMinimumArenaSize()` grew by 28 KiB, and to 36784 bytes once `boundp` and `makunbound` were added; the frame substrate adds its arena-resident 64-frame floor and 32-frame cleanup reserve, now 53832 bytes (10% frame-arena split, 96-byte `FeEvalFrame`; see the kg-side sub-plan set's 03A Decision and its 03D follow-up for the retune history). `test_api.c` now uses a 1 MiB arena so the existing 200-level C-stack probe remains below the physical-frame wall. kg allocates the same size, so a `KG_LISP_ARENA_SIZE` override below roughly 72 KiB now fails to open a context. |
| `FeToString(dst, 0)` writes nothing and returns 0 | `size - 1` underflowed, so a zero-size destination received the whole rendering plus a terminator past it | the contract is now written down: bytes stored, never more than `size - 1`, always terminated. No snprintf-style required length, because measuring is unbounded on a cyclic object |
| Malformed dotted lists are syntax errors | `'(a . b c)` read as `(a c)`, `'(a . b . c)` as `(a . c)` and `'(. a)` as `a`, all silently | three new reader diagnostics; `(a .)` says `missing value after '.'` instead of `stray ')'` |
| A macro expands on every call instead of overwriting its call site | copying the expansion over the call site cloned it, and `nil` and interned symbols are compared by address: a macro expanding to `nil` produced a truthy nil, and one expanding to a symbol missed every lexical binding | one expansion per invocation, charged against the step budget; kg's "a macro expands once per call site" caveat is gone from `README.md` and `doc/kg.1` |
| The writer is bounded and terminates on cycles | `(setcdr x x)` then rendering hung kg with no C-g escape: `M-:`, `eval-buffer`, `C-j`, `format`'s `%s`/`%S` all render | `#<cycle>`, `#<deep>` and `#<truncated>` are stable output; `FeWriteWithOptions()` carries the budgets and reports completion; the writer no longer allocates, and spends the step budget while an evaluation is active |
| `&optional` and `&rest` in parameter lists | Emacs Lisp spells them that way, and kg had to delete `&optional` from every arglist because the binder could not see it | `internal--arglist` is gone from kg's prelude; `&optional` and `&rest` cannot be parameter names |
| Optional argument-count checking, `FeSetStrictArity()` | `((lambda (x) x))` was nil, `((lambda () 1) 2)` was 1 and `((lambda (1) 5) 2)` was 5 | off by default, so kg is unaffected; `fe -a` turns it on and Fe's script suite runs a third time under it |
| An unassigned symbol is `void-variable`, not `nil` | a typo was silently false; Fe's own `TODO.md` asked for this | `boundp` and `makunbound` are new primitives and `FeIsBound()` is a new API; kg's `defvar` asks `(boundp 'name)` rather than evaluating the name |
| `FeCallWithOptions()` — a controlled `FeCall()` | kg ran Lisp commands through a source-string trampoline (`(internal--run-pending-command)`) solely to reach the evaluator's step-budget/interrupt/GC accounting; the trampoline is gone now that a callable can be invoked under the same options | one declaration in `fe.h` and a thin wrapper reusing `BeginEvaluationControl`/`EndEvaluationControl`; tested in `test_api.c`, no `FE_API_VERSION` bump (compatible addition) |
| `unwind-protect` and `FeProtectWithCleanup()` — cleanup stack and host protection | Lisp `unwind-protect` and C `FeProtectWithCleanup` share a single LIFO registry; cleanups run on normal return, Lisp error, C-g interrupt, and step-budget exhaustion | new primitive `unwind-protect`, `FeProtectWithCleanup()` API in `fe.h`, fresh per-entry cleanup step budget and interrupt re-arming in `RunCleanupsAfterError` |
| An explicit `evaluation_depth` counter bounds recursion, checked on every pair-form `Evaluate()`, in addition to `GcStackSize` | The GC stack bounds recursion only by accident, via how many slots a call happens to consume; a build with fatter per-call C frames than the default build (any sanitizer, `-O0`, a debug build) could exhaust the real C stack before the GC stack noticed, crashing instead of raising a catchable error — confirmed under `.ci/ci-05`'s MSan flags, which crash at `(deep 418)` where the default build and `.ci/ci-04`'s ASan/UBSan flags both still raise `GC stack overflow` past `(deep 452)`. `evaluation depth limit exceeded` now fires first, deterministically, in every build. | `FeContext.evaluation_depth`/`evaluation_depth_limit`, `FeEvalOptions.max_depth` (0 selects `DefaultEvaluationDepth` = 1000, measured and explained where it is declared in `fe.c`), reset in `FeHandleError` alongside `call_list`, and held across `Evaluate()`'s macro arm rather than released before it — that arm is a tail call in Fe but not in C, so releasing early let a self-expanding macro recurse with the counter flat; kg's `test_recursion_depth` now expects the new error instead of `GC stack overflow` |
| `FeGetArenaStats()` — read-only arena/evaluator statistics | Neither Fe nor kg had any way to answer "how close is the fixed arena to full", which sub-plan [00D](plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans/00d-baselines-and-arena-observability.md) of kg's Emacs-subset program needs as a Phase 0 baseline, ahead of the Phase 9 diagnostic surface this pulls forward from | `FeArenaStats` struct and accessor in `fe.h`/`fe.c` (total/free slots, peak live objects, collection count, peak GC-stack/evaluation/cleanup-stack depth, allocation failures); seven new `FeContext` counter fields updated at their existing sites (`MakeObject`, `CollectGarbage`, `FePushGC`, `EnterEvaluationDepth`, `PushCleanup`); allocates nothing itself. No kg-visible command yet — that is Phase 9. |
| Core `setq`/`set`, and `=` cut from assignment to left-to-right chained numeric equality — `FE_LANGUAGE_VERSION` 1 → 2 | Emacs' `setq` updates the innermost lexical binding (writing a new global otherwise) and `set` always writes the global cell straight through a same-named lexical binding; neither is `=`, which Emacs uses for numeric comparison. Sub-plans [02A](plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans/02a-pin-the-target-semantics.md)–[02D](plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans/02d-kg-migration-and-el-cutover.md) pinned the oracle answers, landed both new forms beside the old assignment `=` for one coexistence window, then cut it: kg's own prelude `setq` macro (built on the old assignment primitive) is gone, replaced by the core special form, and every kg-owned `(= NAME VALUE)` became `(setq NAME VALUE)`. There is no assignment-`=` compatibility alias — §0.4 of the parent plan: no known external users, so no reason to keep the old spelling reachable. | Fe: new `PSetq`/`PSet` enums and `EvaluateSetq()`/`EvaluateSet()`, the old assignment enum and switch arm deleted, a chained `EvaluateNumericEqual()` replacing the old two-argument `=`; `FeVersion` `"1.1"` → `"2.0"`; `fe/doc/language.md` rewritten. kg: gitlink move, `FE_LANGUAGE_VERSION == 2` compile-time assertion beside `FE_API_VERSION == 1`, `lisp/prelude.el`'s `setq` macro definition deleted and its remaining 53 top-level forms rewritten from `=` to `setq`, `.fe` discovery (init file, bare `load`/`require`) cut to `.el` with no fallback — an explicit `.fe` *path* (containing `/`) still loads, since the cut is discovery-only |
| `fe.c` split into `fe.c` + `fe_eval.c` behind a private `fe_internal.h` — structural, no language or embedding-API change | Sub-plan [03A](plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans/03a-measure-and-fund-the-frame-machine.md) priced Phase 3's frame machine and found the file cap, not the aggregate, was what forced a split: `fe.c` scored 106 of a 112 file cap with the frame machine's substance not yet written, and the evaluator was the region scc's `'"'`-literal desync (from `fe.c:1010`) hid from measurement entirely. Sub-plan [03B](plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans/03b-translation-unit-split.md) landed the cut on its own, mechanically: evaluation control, the `unwind-protect`/`FeProtectWithCleanup` cleanup registry, `FeHandleError` (moved with the registry per 03B's own recommendation — everything it calls on the error path was moving anyway), the recursive evaluator (`Evaluate`, `EvaluateHead`, `EvaluatePrimitive`, `EvaluateList`, `DoList`, `Bind`, `ArgsToEnv`, `EvaluateSetq`, `EvaluateSet`, `EvaluateNumericEqual`, `CheckNumericEqualOperand`, `HandleVoidSymbol`, `HandleNonCallable`, the `EVAL_ARG`/`ARITH_OP`/`NUM_CMP_OP` macros) and its entry points (`FeEvaluate`, `FeCall`, `FeCallWithOptions`, `FeEvaluateWithOptions`) moved into the new file; the reader, writer, and root management (`FeCreateRoot`/`FeGetRoot`/`FeReleaseRoot`) stayed in `fe.c`, as did the kept wrappers (`EvaluateInput`, `FeEvaluateString*`, `FeEvaluateFile*`) that call the now cross-TU `BeginEvaluationControl`/`EndEvaluationControl`. Proved mechanical by pmccabe's per-function sum, conserved exactly: 500 across 202 symbols before and after. | Fe: `fe_internal.h` (object layout, `CAR`/`CDR`/etc. macros, `struct FeContext`, the `Primitive` enum, and the small accessors both files use — `GetDouble`, `GetNativeFn`, `SetType`, `CheckType`, `GetBound`, `MakeObject`, `Equal`, `IsNamedSymbol`, `Format`, plus `unbound`, all of which lost `static`); `fe/test_internal_header.c` compiles the private header standalone under both core compilers; `Makefile`'s `SRCS`/`CORE_OBJS`/fuzz link rules all name both files (`FE_CORE_OBJS` list). kg: `FE_OBJ`/`FUZZ_FE_OBJ` became two-object lists, eleven Makefile sites (link lines, the submodule-population guard, `clean`) updated to match, and `lisp-include-check` extended to forbid `fe_internal.h` anywhere in `src/` the same way it already forbids `fe.h` outside the adapter. |

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

Fex — `fex*.c`, which kg does **not** compile — also gained a file-lifecycle
owner and finalizer, exact byte handling instead of fixed-size renderings, and
`waitpid` EINTR retry. Those are listed here only so an update is not surprised
by them; they cannot reach kg.

Deliberately **not** changed in `fe.c`, and why:

- **Quasiquote semantics.** `quasiquote` is a prelude macro in
  `src/lisp_prelude.c`, so it can change without moving the pin. The core
  only learns the punctuation.
- **`?a` character literals.** `FeReadFn` yields one byte at a time and Fe has
  no character type, so `?é` would silently read as the first UTF-8 byte —
  reintroducing exactly the silent wrongness `#'` was fixed to remove.
  `(string-to-char "é")` is exact and already available.
- **Dynamic binding, `condition-case`, vectors, hash tables,
  keyword arguments, a byte compiler.** All need new object types or a real
  non-local exit mechanism in `Evaluate`.
- **Strict arity by default.** `FeSetStrictArity()` exists and kg does not call
  it. Turning it on would make every `(defun c (x) (interactive) …)` an arity
  error, because kg invokes interactive commands with zero arguments
  (`FeCallWithOptions(ctx, cmd, nullptr, 0, &opts)`). It becomes possible once
  commands are invoked with their arguments.
