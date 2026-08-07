# kg's half of the Emacs-oracle compatibility manifest

This is kg's sibling of `fe/compat/`, built by sub-plan 00C of
[the elisp-subset plan](../../doc/plans/2026-08-03-elisp-subset-and-fe-evaluator.md)
on top of the mechanism sub-plan 00B built in the `fe/` submodule. (Both
sub-plan documents are retired and deleted; the plan itself, and its
sub-plan directory's README, are what survive them.) Read `fe/compat/README.md` first: the record
protocol, the case/oracle-snapshot schema, the Emacs resolution and
version-pinning rules, and the four (now five, with `timeout`) record
kinds are defined there once and reused here verbatim. This file only
covers what is different about kg's half.

## Why a second manifest, not one

Ownership decides which manifest an entry lives in; comparability is a
separate axis. `fe/compat/features.json` owns Fe's core language surface
(59 primitives/aliases at this pin, plus the handful of fe-owned divergences that
live in the reader/writer/evaluator). `test/lisp-compat/features.json`
(this directory) owns kg's 81 natives (`native_bindings[]`,
`src/lisp_prelude.c`) and kg's prelude definitions
(`lisp/prelude.el`'s top-level `(defalias 'NAME ...)` forms, currently 77) -- kg-owned,
even though most of the prelude definitions are themselves oracle-comparable
Emacs Lisp forms
(`let`, `defun`, `cond`, `mapcar`, ...). Putting kg's half inside the
branch-pinned `fe/` submodule would make every kg-side inventory edit a
Rule-10 two-commit dance for no reason; keeping it in kg keeps that free.

Every entry declares `"comparison": "emacs"` or `"comparison":
"kg-policy"` independently of who owns it. A kg-owned prelude macro that
is a pure function of its arguments (`let`, `mapcar`, `cond`, ...) is
`comparison: emacs` and carries a real, version-stamped Emacs snapshot
under `oracle/`. A kg native that observes or mutates kg's own buffer,
window, marker, keymap, or process state is `comparison: kg-policy`: per
`fe/compat/README.md`'s own precedent (buffers, markers, keymaps,
interactive commands, and editor primitives cannot run meaningfully under
`emacs -Q --batch` against kg's semantics), its correctness is pinned by
the native/PTY test named in `kg_test`, not by an Emacs snapshot.

## Layout

```text
test/lisp-compat/
  README.md          this file
  features.json       the manifest: 81 kg natives + 77 prelude definitions
                       (plus a handful of kg-owned cross-cutting
                       divergences and the defcustom entry), each
                       with a status, an owner, a comparison mode, and the
                       case(s)/kg_test that back it
  cases/*.json         one file per case, fe/compat's schema
                       (id, setup, expr, note) plus kg's optional
                       "expect": "agree"/"diverge" (see the XPASS rule
                       below)
  oracle/*.json         checked-in Emacs snapshots for every
                        comparison=emacs entry, version-stamped exactly
                        like fe's
  oracle/emacs-shim.el   byte-for-byte the same shim fe/compat/oracle
                          uses (00B's tooling is generic over a corpus
                          root specifically so this does not need its own
                          copy of the *runner*, but the shim file itself
                          still has to exist at this path since
                          run-emacs-oracle.py invokes
                          `<corpus_root>/oracle/emacs-shim.el`)
```

No `tools/` or `utils/` directory here: the runners are reused directly
from the submodule (`fe/utils/run-emacs-oracle.py <corpus_root>`,
`fe/utils/check_compat_manifest.py --manifest ... --other-manifest ...`)
rather than copied, per 00B's explicit design ("`run-emacs-oracle.py`
takes the corpus root as its first argument ... precisely so you can
point kg's corpus at it without copying the runner").

## What actually executes a kg-owned case

* `comparison: emacs` entries get a real, checked-in Emacs snapshot
  (`oracle/<id>.json`, produced by `fe/utils/run-emacs-oracle.py` exactly
  as in `fe/`) recording what the pinned oracle does for the case's
  `expr`. That snapshot is the target contract, and since sub-plan 10C
  **`make lisp-oracle-check` runs kg against every one of them**
  (`utils/check_lisp_oracle.py`, part of `make check`). It drives
  `test/kgbatch` once per case -- the editor's own objects and its own
  `kg_lisp_eval_string()`, with kgbatch's `-r` for tagged values, exact
  condition symbols and quits, and `-b` for a live scratch buffer. No Emacs is invoked:
  the snapshots are the oracle. This paragraph used to say kg had no
  batch-mode Lisp outside the full editor and that a human read the case
  and the snapshot side by side. Both halves are obsolete.
* Every entry -- regardless of comparison mode -- also names the kg
  native or PTY test (`kg_test`) whose assertions pin kg's own side of
  the same behavior. `utils/check_lisp_compat.py` verifies that every
  cited file exists and that every cited function is defined there, so a
  renamed test or a moved PTY case fails `make check` instead of leaving
  a row citing evidence that is not there. `comparison: kg-policy`
  entries have no snapshot by design and are pinned by that test alone.

### The XPASS rule, and where kg's runner deliberately differs from fe's

`fe/utils/run-fe-compat.py` prints `agrees early` and counts a pass when
a case whose feature is *not* `supported` matches the oracle anyway.
kg's runner does not: a `divergent` case that agrees is a **failure**,
for the same reason `XPASS` fails kg's PTY suite. A recorded divergence
that stopped diverging is a manifest lying about the tree, and the row
(and usually a `doc/fe-upstream.md` entry beside it) has to be corrected
before the run goes green. `planned` keeps fe's softer treatment.

The rule paid for itself on its first run: `native-type-of` and
`native-commandp` were both `divergent` with cases that fully agreed
with the oracle -- the divergence each row described was never
evaluated by anything -- and `native-string-length` was `supported`
against a `void-function` snapshot it could never match.

A feature's status is a property of the feature, and one feature can
legitimately have cases on both sides. A case says so for itself with
`"expect": "agree"` or `"expect": "diverge"` in its own file; with no
such field the feature's status decides. `native-type-of` is the worked
example: `(type-of 1)` agrees, `(type-of 1.0)` does not.

### Condition records are normally compared by exact symbol

kg exposes no condition symbol to a C host, but kgbatch's `-r` wrapper
catches ordinary errors in Lisp, where the condition object is available,
and emits its exact car in an `E:` record. The runner compares that symbol
for equality. Reader errors happen before the wrapper evaluates, and an
uncatchable budget happens outside it; only those message-source records use
fe's weaker fallback, requiring the oracle condition name to occur in kg's
diagnostic. The runner's self-test pins the structured path.

## Proof 2 — the representative user init, bullet by bullet

The parent plan's §14 asks for "a tracked, isolated `.config/kg/init.el`
fixture" covering eight things, run in a temporary HOME through PTY
tests. Sub-plan 10A Decision 6 declares the existing corpus that fixture
rather than writing a ninth copy of it: about 97 PTY cases already plant
`config_files:` HOMEs (roughly 70 with a `.config/kg/init.el`), and
`test/pty/lisp-init-phase8-library.yaml` is 08A's representative init
reconstructed construct for construct. A new monolithic file would only
be a second thing to drift.

`test/pty/lisp-init-phase8-library.yaml` is the named representative
fixture, and its own header says so. Its second command inserts every
value the init file computed, so the case observes the *values*, not
merely that the file loaded.

| §14 bullet | Where it is proven |
| --- | --- |
| `setq`, `defvar`, `defconst`, the supported `defcustom` subset | `lisp-init-phase8-library.yaml` (all four, plus `custom-set-variables`); `defcustom`'s own case distinctions are the `defcustom` manifest row, whose rationale `utils/check_lisp_compat.py` gates |
| `defun` and interactive commands | `lisp-init-phase8-library.yaml` (`phase8-command`, `phase8-report`, both reached from key bindings); `lisp-defun-interactive.yaml`, `lisp-define-command.yaml` |
| macros and backquote | `lisp-init-phase8-library.yaml` (`` `(head . ,x) ``, a nested backquote, `dolist`, `add-to-list`, `declare`); `test_lisp.c:test_quasiquote` |
| hooks | `lisp-init-phase8-library.yaml` (an `after-change-functions` lambda whose count is reported); `lisp-auto-fill-mode-break.yaml`, `test_lisp.c:test_hooks` |
| key bindings | `lisp-init-phase8-library.yaml` (`global-set-key` + `kbd`, both bindings used); `lisp-bind-key.yaml` |
| **buffer-local-style configuration where supported** | **Nominal only, and recorded as such.** `setq-local`/`setq-default` are documented aliases of `setq` and write the one global binding; both manifest rows are `divergent` since 10C for exactly this reason. The fixture uses `setq-local` and reports its value, which is what "supported" amounts to here. `add-hook`'s LOCAL argument is real and is a different mechanism. |
| loading helper files | `lisp-init-phase8-library.yaml` (`(require 'phase8-pkg)` from a package planted beside the init file); `lisp-init-load-pkg.yaml`, `lisp-require-filename-el-suffix.yaml`. **Honest row:** `load` does not search `load-path` — a bare name resolves to `<config>/kg/lisp/NAME.el` and nothing else, and `require` is the only form that searches. Recorded as the `load-path-search` divergence. |
| error handling | `lisp-init-phase8-library.yaml`, added by 10D: a guarded `(require ...)` of a package that is not installed, a `condition-case` naming `wrong-type-argument` rather than catching wholesale, an `ignore-errors`, and a form after them all proving the init file kept going. All four values are in the asserted output. |

Two more things a reader of this table should know, because they are
properties of the fixture rather than of any one bullet: an init file
that errors *outside* a handler leaves the forms before it in effect and
reports `file:LINE: CONDITION` (`lisp-init-error.yaml`,
`lisp-init-runtime-error.yaml`), and `defvar` does not create a special
variable, so an init file that rebinds one around a call gets a silently
different answer (the `prelude-defvar` row).

## Proof 3 — the higher-order package, verified against Emacs 31

§14's Proof 3 asks for "a self-contained package" exercising Lisp-2 name
separation, `funcall`/`apply`, closures, macro expansion, catch/throw,
`condition-case`, `provide`/`require` and multiple files, and adds: "the
package may be written for kg, but its pure-language portions should
also run unchanged under Emacs 31."

The package is `lisp/pipeline.el` (pure) plus `lisp/pipeline-text.el`
(editor-facing, and it `(require 'pipeline)`, which is the multiple-files
bullet). Splitting it that way is what makes the Emacs 31 claim
*measurable*: the nine `pipeline-*` cases in this corpus load
`lisp/pipeline.el` — the tracked file, not a copy — on **both** sides,
so their snapshots are the pinned Emacs' own answers for the code kg
ships and `make lisp-oracle-check` fails the moment the two stop
agreeing.

The one form that does the loading is dialect-neutral, in the shape fe's
own 10B cases use:

```elisp
(if (fboundp 'expand-file-name)
    (load (expand-file-name "lisp/pipeline.el") nil t)
  (load "lisp/pipeline.el"))
```

`(fboundp 'expand-file-name)` is `t` in Emacs, whose `load` searches
`load-path` for a relative name and so needs an absolute one, and `nil`
in kg, whose `load` takes a `/`-containing name as a literal path. Both
dialects *read* both branches; only one runs.

| §14 bullet | Case (compared against Emacs 31) |
| --- | --- |
| Lisp-2 name separation | `pipeline-lisp2-cells` |
| `funcall` | `pipeline-closures-funcall` |
| `apply` | `pipeline-apply` |
| closures | `pipeline-closures-funcall` |
| macro expansion | `pipeline-macroexpand-1` (one step), `pipeline-macroexpand-fixpoint` (the fixpoint), `pipeline-macro-call-p` (expansion used as a predicate), `pipeline-macro-use` (the macros evaluated) |
| catch/throw | `pipeline-catch-throw` |
| `condition-case` | `pipeline-condition-case` |
| provide/require, multiple files | `lisp/pipeline-text.el`'s `(require 'pipeline)`, exercised end to end by `test/pty/lisp-proof3-pipeline-init.yaml` |

Three PTY cases cover what an oracle cannot: the require chain reached
from a real `init.el` (`lisp-proof3-pipeline-init.yaml`), the commands
reached through `M-x` (`lisp-proof3-pipeline-commands.yaml`), and both
error paths — handled and unhandled — with the editor still usable
afterwards (`lisp-proof3-pipeline-errors.yaml`).

Two things the package deliberately does **not** do, because they would
have made the agreement a coincidence rather than a property, and one it
cannot:

* no recorded silent divergence is load-bearing in the pure file:
  nothing rebinds a `defvar`'d variable around a call (`prelude-defvar`),
  and no returned value contains a `(quote X)` form
  (`writer-quote-abbreviation`) — which is why the macros expand to plain
  `if`/`+` forms and why the reflective helpers are handed their form as
  data built with `list`;
* no `throw` crosses a native re-entry boundary
  (`catch-throw-reachability`): every catch and its throws are inside one
  `pipeline.el` function, with no `save-excursion` or
  `with-current-buffer` between them;
* `lisp/pipeline.el` carries a `lexical-binding: t` cookie on line 1.
  kg reads it as the comment it is, but without it Emacs' `load` binds
  dynamically and the closures capture nothing — measured, as
  `void-variable (n)`, before the cookie was added.

## The compatibility milestone gate (§14), item by item

The parent plan's §14 says the initial program is complete when nine
things hold. Each row below names the command or test that decides it,
so the gate is run rather than read. Every figure is measured on the
tree at Phase 10's close.

| §14 item | Status | Decided by |
| --- | --- | --- |
| the three proofs pass | PASS | Proof 1: `lisp-auto-fill-mode-break.yaml`, `-undo.yaml`, `-error-disarms.yaml`, `-no-lisp-regression.yaml`, plus `make lisp-package-check`. Proof 2: `lisp-init-phase8-library.yaml` and the corpus mapped above. Proof 3: `lisp/pipeline.el` + `lisp/pipeline-text.el`, nine oracle cases and `lisp-proof3-pipeline-{init,commands,errors}.yaml` |
| all `supported` `comparison: emacs` entries pass against the oracle | PASS | `make lisp-oracle-check` — 113 cases, 100 passed, 13 recorded divergences, 0 failed. It has no tolerance for a divergence that starts agreeing (the XPASS rule above), and it self-tests first |
| all `supported` `comparison: kg-policy` entries pass their kg tests | PASS | `make check` runs every cited test; `make lisp-compat-check` verifies each citation resolves — the file exists, a C citation names a function, and that function is defined there |
| unsupported entries fail clearly | PARTIAL, and re-worded (10A Decision 5) | Reader syntax kg does not implement is rejected **by name** (`unsupported read syntax: vector brackets`, `phase8-reader-vector`), and so is `macroexpand-all` (`unsupported feature: macroexpand-all`). Unknown *functions* answer plain `void-function`, which is byte-identical to a typo. A curated known-name channel would be new language machinery with an unbounded name list; the debt is in `doc/TODO.md` |
| intentional divergences are documented and tested | PASS | 13 `divergent` `comparison: emacs` cases run every time `make lisp-oracle-check` does, and each must still diverge. The two that had never been exercised (`native-type-of`, `native-commandp`) were found by that rule and closed; the two that were unrecorded (`prelude-defvar`, `writer-quote-abbreviation`) are now rows with cases, `doc/fe-upstream.md` entries and `doc/TODO.md` work items. The acceptance review added `load-error-condition-reachability`, whose loaded-file evaluation error crosses kg's nested Fe barrier instead of reaching the caller's handler |
| kg starts and operates with both Lisp configurations | PASS | `make check` (32 native / 439 PTY, 0 fail, 0 skip) and `make WITH_LISP=0 clean all check` (32 native / 341 pass + 98 skip, 0 fail); CI stage `.ci/ci-08-with-lisp-0.sh` |
| no assignment `=` remains | PASS | `=` is chained numeric equality since Phase 2 (`FE_LANGUAGE_VERSION` 2). Measured now, not remembered: `(= 1 1)` is `t`, and `(= x 1)` on an unbound `x` is `void-variable`, not an assignment (`prelude-equality-family`, `fe/compat`'s numeric rows) |
| strict arity is unconditional | PASS | `FeSetStrictArity()`/`FeGetStrictArity()` do not exist to turn it off (Phase 7, `FE_LANGUAGE_VERSION` 6); `arity-strict` and `arity-lambda-too-few-nargs` compare against the oracle, and a wrong-arity *macro* call raises the same condition with the same data through 10B's reflective expansion path |
| Lisp-2 behavior is complete for the supported subset | PASS | `pipeline-lisp2-cells` (one symbol, two cells, measured against Emacs), the `lisp2-*` rows in `fe/compat`, and `#'`/`funcall`/`apply`/`fboundp`/`symbol-function`/`fset`/`fmakunbound`/`defalias` all present since Phase 4 |

## §15 — the bytecode decision, answered from measured counters

§15 forbids starting a bytecode project during the program and lists
nine measurements to take afterwards, of which at least one of five
triggers must fire to justify one. 10A Decision 9 funded two new
counters rather than instrumenting fe; this is what they say.

Measured with the counting build (`test/perfobj/kg`, `$KG_PERF_OUT`),
three runs, on a representative `init.el` (the 08A corpus plus
`(require 'auto-fill)` and `(require 'pipeline-text)`, the latter a
two-file chain), opening a one-character file and quitting:

| §15 measurement | Counter | Reading |
| --- | --- | --- |
| prelude load time | `lisp_prelude_ns` | **0.817-0.821** ms |
| user-init load time | `lisp_user_init_ns` | **0.587-0.592** ms (excludes the prelude) |
| package load time | `lisp_package_load_ns` | **0.432-0.437** ms, counted once per require chain, inside the init time above |
| object allocation per loaded form | `lisp_arena_peak_live` | 8402 of 56224 slots — **14.9%** live after everything above |
| AST retention cost | `lisp_arena_free_slots`, `lisp_gc_count` | 47822 free, **0 collections**: nothing loaded has yet cost a collection |
| GC time | — | not instrumented (fe work; 10A Decision 9 declined it) |
| time spent reading versus evaluating | — | not instrumented (fe work) |
| evaluator dispatch overhead | — | not instrumented (fe work) |
| interactive command latency attributable to Lisp | — | not instrumented; `lisp_peak_frame_depth` 13 of 1096 is the only related shape measured |

Against §15's five triggers:

| Trigger | Fires? | Why |
| --- | --- | --- |
| 1. Lisp evaluation materially affects interactive latency | **no** | the whole of startup Lisp is ~1.4 ms, and no interactive path measured has shown Lisp cost at all |
| 2. init or package startup exceeds an agreed product target | **no** | prelude + init together are ~1.4 ms; there is no product target this is near |
| 3. retained ASTs consume an unacceptable fraction of the arena | **no** | 14.9% live, 0 collections, 0 allocation failures |
| 4. distributing precompiled extension packages becomes a requirement | **no** | no such requirement exists; packages are `.el` files `make install` ships |
| 5. profiling identifies evaluator dispatch as a dominant cost | **unmeasured** | the instrumentation for it is fe work at zero headroom, and 10A Decision 9 declined to build it for a decision the other four already settle |

**No trigger fires. The explicit-frame AST interpreter is retained**, as
§15 directs when none applies. A future phase that re-opens bytecode
funds trigger 5's instrumentation then; the three unmeasured rows above
are what it would have to build first.

Wall times are the weakest evidence in this repository (`src/perf.h`'s
own header rule), which is why the counters that decide triggers 3 and 4
are counts rather than durations, and why the durations above are given
as the range of three runs rather than a single figure.

## The checker

`utils/check_lisp_compat.py` (kg's `utils/`, not `fe/utils/`) is the
00C-specific script. It:

1. Shells out to `fe/utils/check_compat_manifest.py` for both manifests
   (this one and `fe/compat/features.json`), each with
   `--other-manifest` pointed at its sibling, reusing 00B's schema
   validation and id-collision check rather than reimplementing it.
2. Parses `fe/fe.c`'s `primitive_names[]`/`primitive_aliases[]` and
   `src/lisp_prelude.c`'s `native_bindings[]` and the `(defalias 'name ...)`
   top-level forms in `lisp/prelude.el`, and checks every one of the
   resulting 59 + 81 + 77 source names is claimed by at least one
   feature entry's `"source_name"` field, across both manifests combined.
   (The counts are re-derived from the sources on every run and printed;
   they are written here only so a reader knows the order of magnitude.)
   This is the check that keeps the inventory from rotting: a native or
   prelude definition added without a manifest entry fails `make check`.
3. Checks the `defcustom` entry exists with the shape 00C's gate
   requires -- the entry is `supported` since 08E implemented the macro,
   and the gate now reads the case distinctions out of its rationale.
4. Checks every `status: planned` entry's `rationale` names a phase
   (`Phase <digit>`), extending `fe/utils/check_compat_manifest.py`'s own
   rule set by one clause 00B's version did not need yet.

Wired into `make check` next to `docs-check` (kg's closest existing
analogue: a dumb, structural check that a table and the source it
describes have not drifted apart).

`utils/check_lisp_oracle.py` is the other half, added by sub-plan 10C and
wired into `make check` as `make lisp-oracle-check`: where the checker
above asks whether the manifest and the sources agree, this one asks
whether *kg* and the snapshots agree. It runs no Emacs, takes 0.29 s for
113 cases, and self-tests first -- `--self-test` builds a corpus in a
temp directory whose snapshot says 4 where kg answers 3 and requires the
run to fail, then verifies an ordinary error is captured by exact condition
symbol. That is what makes its "0 failed" worth reading. Under
`WITH_LISP=0` the target reports that there is no evaluator to compare
and does nothing.

`make lisp-compat-oracle` regenerates/verifies this directory's
snapshots against the resolved Emacs, using `fe/utils/run-emacs-oracle.py`
directly. It is a regeneration/verification target, not part of ordinary
`make check` -- checked-in snapshots keep the normal suite Emacs-free,
exactly as in `fe/`.
