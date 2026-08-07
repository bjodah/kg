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
(55 primitives, 1 alias, plus the handful of fe-owned divergences that
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
  `kg_lisp_eval_string()`, with kgbatch's `-p` for prin1-shaped printing
  and `-b` for a live scratch buffer -- and classifies the result from
  the exit status, never by pattern-matching prose. No Emacs is invoked:
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

### Condition records are compared by substring, not by symbol

kg has no host-visible condition *symbol*: `src/lisp.h` exports the
completion kind (error/quit/budget) and the message text, not the
condition object. So a condition record is compared the weaker way fe's
runner already documents for its own message-source records -- the
oracle's condition name must appear in kg's message. kg's messages lead
with the condition name (`void-function no-such-fn`), so it is a real
check, but it is a substring claim and the runner's header says so
rather than letting a reader assume symbol equality. Narrowing it is
condition-data work in `src/lisp.h`, not runner work.

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
   resulting 55 + 1 + 81 + 77 source names is claimed by at least one
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
101 cases, and self-tests first -- `--self-test` builds a corpus in a
temp directory whose snapshot says 4 where kg answers 3 and requires the
run to fail, which is what makes its "0 failed" worth reading. Under
`WITH_LISP=0` the target reports that there is no evaluator to compare
and does nothing.

`make lisp-compat-oracle` regenerates/verifies this directory's
snapshots against the resolved Emacs, using `fe/utils/run-emacs-oracle.py`
directly. It is a regeneration/verification target, not part of ordinary
`make check` -- checked-in snapshots keep the normal suite Emacs-free,
exactly as in `fe/`.
