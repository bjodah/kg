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
  cases/*.json         one file per case, fe/compat's exact schema
                       (id, setup, expr, note)
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

Unlike fe's own manifest, nothing in this directory drives a
standalone interpreter against `cases/*.json` automatically: kg has no
batch-mode Lisp REPL outside the full editor, and most kg-owned
constructs need kg's buffer/window/process state to mean anything at
all. Instead:

* `comparison: emacs` entries get a real, checked-in Emacs snapshot
  (`oracle/<id>.json`, produced by `fe/utils/run-emacs-oracle.py` exactly
  as in `fe/`) recording what the pinned oracle does for the case's
  `expr`. That snapshot is the target contract.
* Every entry -- regardless of comparison mode -- names the kg native or
  PTY test (`kg_test`) whose assertions already pin kg's own side of the
  same behavior. `utils/check_lisp_compat.py` (kg's checker, see below)
  verifies the field is present and non-empty; it does not re-execute kg
  or diff kg's output against the snapshot programmatically, because kg's
  existing native (`test/test_lisp.c`) and PTY (`test/pty/lisp-*.yaml`)
  suites already are that programmatic check, run by `make check` itself.
  A human (or a future automated step, once kg grows a batch Lisp mode)
  reads the case and the snapshot side by side with the named test's
  assertions to confirm agreement; every `comparison: emacs` entry in
  this manifest was confirmed this way when it was added.

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

`make lisp-compat-oracle` regenerates/verifies this directory's
snapshots against the resolved Emacs, using `fe/utils/run-emacs-oracle.py`
directly. It is a regeneration/verification target, not part of ordinary
`make check` -- checked-in snapshots keep the normal suite Emacs-free,
exactly as in `fe/`.
