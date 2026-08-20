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
(81 primitives/aliases at this pin, plus the handful of fe-owned divergences that
live in the reader/writer/evaluator). `test/lisp-compat/features.json`
(this directory) owns kg's 134 natives (`native_bindings[]`,
`src/lisp_prelude.c`) and kg's prelude definitions
(`lisp/prelude.el`'s top-level `(defalias 'NAME ...)` forms, currently 126) -- kg-owned,
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
  features.json       the manifest: 134 kg natives + 126 prelude definitions
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

## A group recorded before its implementation exists (`vectors`)

The 35 `vector-*` cases and their ten `category: vectors` rows were
recorded by Phase 24.0
(`doc/plans/2026-08-20-elisp-data-model-phase24-execution.md`), whose
entry gate is: *the oracle cases exist and are green as divergences
before any fe reader change lands.* They are the first group in this
corpus written entirely **ahead** of the behaviour they describe, so
they read strangely on purpose — 33 of the 35 are recorded divergences,
and they are supposed to be.

Two things make that honest rather than a placeholder:

* every row is classified by what kg **measured**, not by what the phase
  expects to change. 22 of the 33 divergences are the reader stopping at
  `unsupported read syntax: vector brackets`; the other 11 contain no
  bracket at all and record a real, different kg answer — ten are
  `void-function`, from the first of `vectorp`/`aref`/`make-vector`/
  `vector` the case reaches (none of the six names `vectorp`, `aref`,
  `aset`, `make-vector`, `vector`, `vconcat` is bound at this pin), and
  one is `wrong-type-argument`, the case where kg has the name and
  answers differently anyway (`vector-append-string`, where kg's
  `append` rejects a string Emacs flattens to `(97 98)`).
* two cases carry `"expect": "agree"` inside divergent rows, and they
  are what makes the other 33 attributable rather than a blanket
  "vectors are missing". `vector-sequence-list-baseline` shows every
  generalised name (`length`, `elt`, `mapcar`, `mapconcat`,
  `copy-sequence`) already answering exactly what Emacs answers when
  given a list, and `vector-elt-list-out-of-range` shows kg already
  matching Emacs' measured asymmetry (`elt` past the end of a list is
  `nil`; past the end of a vector it raises). A change that generalises
  these names by breaking the list case fails the run instead of passing
  it.

Where a row's `kg_test` is `null` that is the honest answer at 24.0 and
not an oversight: no kg-side test asserts anything about vector identity,
printing, access or construction, because kg has no vectors. Those
citations land in the same commits as the implementation, and until then
this runner's XPASS rule is the pin — each case must keep diverging on
every `make lisp-oracle-check`.

## The same shape, for a type that already exists (`strings`)

The 26 `string25-*` cases and their fourteen `category: strings` rows were
recorded by Phase 25.0
(`doc/plans/2026-08-20-elisp-data-model-phase25-execution.md`), whose entry
gate is the `vectors` gate one phase on: *the cases exist and are green as
recorded answers* before the representation moves. They read differently
from the `vectors` group on purpose, because the subject is different. kg
HAS strings, so nine of the 26 are agreements rather than two of 35, and
each one is a control that makes a neighbouring divergence attributable:

* `string25-multibyte-character-view` shows `length`, `substring`,
  `string-to-char` and `%S` already answering exactly what Emacs answers on
  multibyte text — so `string25-multibyte-aref-vs-elt`, where `aref` gives
  195 and `elt` gives 233 for the same character of the same string, is
  about ONE unwrapped name and not about UTF-8.
* `string25-match-data-clobbered` shows that a second `string-match` moves
  the match data, which is what gives
  `string25-string-match-p-non-perturbing` its meaning: a probe that could
  not see a move cannot testify that something did not make one.
* `string25-substring-in-range` and `string25-aref-string-bounds` show the
  in-range and out-of-bounds halves that already agree, beside the
  clamp-versus-raise and the refuse-to-mutate halves that do not.

Two mechanical points a reader of these cases should know:

* **Some cases ask for a condition's MESSAGE or DATA as their value**, on
  purpose. The runner compares condition SYMBOLS for structured records, and
  three of the contracts here are invisible at that resolution: both of
  Emacs' `aset` width-change refusals are plain `error`, as is kg's single
  refusal, and `substring` out of range raises in Emacs and returns a
  clamped string in kg. A `condition-case` that returns `(cdr e)` or the
  message makes the difference part of the value, which is compared exactly.
* **A case file cannot carry a raw invalid byte.** A case's `expr` is a JSON
  string and JSON must be valid UTF-8, so the lone `0xE2` the master plan's
  "invalid byte input" bullet asks about is written `\342` — the octal
  escape both readers turn into that byte. It is the nearest expressible
  form; a case carrying the byte itself would have to be a PTY case. The
  limitation is recorded in `string25-raw-byte-length-and-aref`'s own note.

Four rows carry `kg_test: null` for the 24.0 reason: no kg-side test asserts
anything about `aref`/`aset` on a string, an embedded NUL, or the writer's
treatment of an invalid byte. The rows for names kg does not have yet
(`string-match-p`, `save-match-data`, `match-data`, `set-match-data`) cite
the test that pins the match surface kg DOES have, and say so in their
rationale; those names' own citations land with the implementation.

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
| **buffer-local-style configuration where supported** | **Real since Phase 18**, and this row said "nominal only" until then. `setq-local` gives the current buffer a binding of its own and `setq-default` writes the default; both rows are `supported`. The fixture still uses `setq-local` and reports its value, and `test/pty/lisp-buffer-local-two-buffers.yaml` is the case that shows two buffers answering differently for one name. One named gap survives (`phase18-automatically-buffer-local`); the two `let` interactions that were gaps closed with Phase 18's follow-up and are pinned, with their interactions, by `lettag-let-binding-buffer-tag`. `add-hook`'s LOCAL argument is still a different mechanism. |
| loading helper files | `lisp-init-phase8-library.yaml` (`(require 'phase8-pkg)` from a package planted beside the init file); `lisp-init-load-pkg.yaml`, `lisp-require-filename-el-suffix.yaml`. **Honest row:** `load` does not search `load-path` — a bare name resolves to `<config>/kg/lisp/NAME.el` and nothing else, and `require` is the only form that searches. Recorded as the `load-path-search` divergence. |
| error handling | `lisp-init-phase8-library.yaml`, added by 10D: a guarded `(require ...)` of a package that is not installed, a `condition-case` naming `wrong-type-argument` rather than catching wholesale, an `ignore-errors`, and a form after them all proving the init file kept going. All four values are in the asserted output. |

Two more things a reader of this table should know, because they are
properties of the fixture rather than of any one bullet: an init file
that errors *outside* a handler leaves the forms before it in effect and
reports `file:LINE: CONDITION` (`lisp-init-error.yaml`,
`lisp-init-runtime-error.yaml`), and `defvar` marks a symbol special, so
an init file that rebinds one around a call gets Emacs' answer (the
`prelude-defvar` row and the sixteen `phase11-dynamic-*` cases).  That
second sentence said the opposite until Phase 11: rebinding used to be
lexical, and the answer used to be silently different.

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

**All three of those constraints were Phase 11 targets, and two of them
are now satisfied rather than avoided.**  The package is deliberately
left as it was written — the point of the design note is what it took to
make Proof 3 an agreement rather than a coincidence *at the time*, and
rewriting it would erase that — but the constraints no longer bind:

* `prelude-defvar` and `writer-quote-abbreviation` are `supported`.  A
  Proof-3-shaped package written today may rebind a `defvar`'d variable
  around a call and may return a `(quote X)` form; both print and
  compute as they do in Emacs.
* `catch-throw-reachability` is `supported` for exactly the two forms the
  bullet names: a `throw` now crosses `save-excursion` and
  `with-current-buffer`, which are prelude macros over `unwind-protect`.
  Every *other* native re-entry — a hook, a process filter or sentinel, a
  nested `command-execute`, the loader's containment barrier — is still a
  wall, so the bullet's advice survives with a narrower scope.
* the `lexical-binding: t` cookie's meaning has changed, and it still
  matters.  It used to be the difference between Emacs binding
  everything dynamically and kg binding everything lexically; it is now
  the difference between Emacs binding everything dynamically and *both*
  binding lexically except where a `defvar` marked the name.  kg still
  reads the cookie as the comment it is — kg has no whole-file dynamic
  mode to select — so the line stays a note to Emacs alone.

## The compatibility milestone gate (§14), item by item

The parent plan's §14 says the initial program is complete when nine
things hold. Each row below names the command or test that decides it,
so the gate is run rather than read. Every figure is measured on the
tree at **Phase 11's close** (the gate itself is not reopened — §14 and
§18 closed with Phase 10; Phase 11 moved four rows of the divergence
inventory the gate cites and opened two, and this is where the counts are
re-recorded).

| §14 item | Status | Decided by |
| --- | --- | --- |
| the three proofs pass | PASS | Proof 1: `lisp-auto-fill-mode-break.yaml`, `-undo.yaml`, `-error-disarms.yaml`, `-no-lisp-regression.yaml`, plus `make lisp-package-check`. Proof 2: `lisp-init-phase8-library.yaml` and the corpus mapped above. Proof 3: `lisp/pipeline.el` + `lisp/pipeline-text.el`, nine oracle cases and `lisp-proof3-pipeline-{init,commands,errors}.yaml` |
| all `supported` `comparison: emacs` entries pass against the oracle | PASS | `make lisp-oracle-check` — **148 cases**, 136 passed, **12 recorded divergent cases**, 0 failed (143/132/11 at Phase 12's first close, 131/120/11 at Phase 11's, 113/100/13 at Phase 10's). Units matter and this row conflated them once: 12 is a count of *cases*, and the manifest carries **17 divergent features**, of which ten are `comparison: emacs` and account for those cases while the other seven are `comparison: kg-policy` and have no snapshot at all. The runner has no tolerance for a divergence that starts agreeing (the XPASS rule above), and it self-tests first |
| all `supported` `comparison: kg-policy` entries pass their kg tests | PASS | `make check` runs every cited test; `make lisp-compat-check` verifies each citation resolves — the file exists, a C citation names a function, and that function is defined there |
| unsupported entries fail clearly | PARTIAL, and re-worded (10A Decision 5) | Reader syntax kg does not implement is rejected **by name** (`unsupported read syntax: vector brackets`, `phase8-reader-vector`), and so is `macroexpand-all` (`unsupported feature: macroexpand-all`). Unknown *functions* answer plain `void-function`, which is byte-identical to a typo. A curated known-name channel would be new language machinery with an unbounded name list; the debt is in `doc/TODO.md` |
| intentional divergences are documented and tested | PASS | 11 `divergent` `comparison: emacs` **cases** run every time `make lisp-oracle-check` does, and each must still diverge. The two that had never been exercised (`native-type-of`, `native-commandp`) were found by that rule and closed. Phase 11 then **fixed four** of the 13 the Phase 10 close recorded — `prelude-defvar`, `writer-quote-abbreviation`, `load-error-condition-reachability` and `catch-throw-reachability` — and **opened two** in their place, both consequences of those fixes rather than oversights: `load-throw-reachability` (the containment barrier that makes a loaded file's error catchable is a throw wall) and `phase11-one-arg-defvar-file-scope`. Phase 12 then closed the *behaviour* the second of those described — a one-argument `defvar` is scoped to its input unit now, and the two-file probe `test_phase12_one_arg_defvar_file_scope` is the evidence — without the case flipping, because that case never pinned the leak: the Emacs shim evaluates each setup form in its own scope while kg's runner concatenates them into one file, so both sides answer correctly to different questions. Its rationale was rewritten in the same commit rather than its snapshot, which is the other half of the XPASS discipline: a fix that deliberately does not flip its case must still correct what the case claims. `load-throw-reachability` flipped at the phase's FIX CYCLE — fe's input-unit trio let `load` become a prelude loop in the caller's run, the case went expect: diverge → agree in the same commit as the behaviour, and three fresh `load-dynamic-extent` cases pin the consequences. The same cycle opened `cleanup-raise-residuals` (two pre-existing cleanup-delivery shapes the docs review found) — closed and opened divergences both go through this table's rule. The XPASS rule is what forced each fix's manifest edit into the same commit as its behaviour change |
| kg starts and operates with both Lisp configurations | PASS | `make check` (32 native / 443 PTY, 0 fail, 0 skip) and `make WITH_LISP=0 clean all check` (32 native / 341 pass + 102 skip, 0 fail); CI stage `.ci/ci-08-with-lisp-0.sh` |
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
two-file chain), opening a one-character file and quitting.  **The
readings below are the Phase 10 measurement and are left as taken**;
they are the answer to a question §15 asked once, not a live figure.
The two denominators have moved five times since: at the Phase 11 pin,
when fe's dynamic-binding frame record grew; by one object slot at the
Phase 12 pin; at the Phase 14 pin, where a symbol object one cons bigger
plus eight primitives grew `FeMinimumArenaSize` enough to move two frame
slots' worth of bytes to the object side; at the Phase 19 pin, where the
seeded `error-message` properties moved three more; and at the Phase 20
pin, where two string primitives and two condition rows moved one; and at
the let-binding-buffer-tag pin, where a host tag on every cleanup entry
moved two more.  The 1 MiB arena partitions to 56147 object slots and 1087
frames now, against the 56224 and 1096 the table names.  That arena is no
longer the default either: Phase B of
`doc/plans/2026-08-19-fe-simplification-and-cheap-compat.md` made it
10 MiB, which the same fe partitions to 440489 slots and 10916 frames
once Phase 24's payload carve has taken its quarter, so the percentages
below are read against a denominator eight times smaller than a default
build's.

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

## Counts, with their units (re-stated at the Phase 12 close)

Cases and features are different units, and every count below names the
one it is in.  Measured on the tree at Phase 12's fix cycle, not
carried (the first-close table said 209/17/192, 246 cases, 143
snapshots, and carried a stale 327 for fe's snapshots where the measured
count was 355 — the number below is measured):

| Unit | kg (`test/lisp-compat/`) | fe (`fe/compat/`) |
| --- | ---: | ---: |
| features in the manifest | **220** | **168** |
| ... of which `divergent` | **17** | **31** |
| ... of which `supported` | 203 | 124 |
| ... `planned` / `unsupported` | 0 / 0 | 9 / 4 |
| case files | **251** | **367** |
| checked-in Emacs snapshots | **148** | **357** |
| `make lisp-oracle-check` | 148 cases, 136 passed, **12 divergent cases**, 0 failed | — |
| `make -C fe compat` | — | 367 cases, 303 passed, 64 known gaps, 0 failed |

The two numbers most often confused are in the last two rows of the kg
column: **11 divergent cases** and **17 divergent features** are both
true and are not the same measurement.  Ten of the 17 are
`comparison: emacs` and produce those 11 cases; the other seven are
`comparison: kg-policy`, have no snapshot, and are pinned by the test
their `kg_test` names.

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
whether *kg* and the snapshots agree. It runs no Emacs, takes well under
a second for its 143 cases, and self-tests first -- `--self-test` builds a corpus in a
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
