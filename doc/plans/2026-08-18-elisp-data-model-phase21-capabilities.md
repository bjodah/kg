# Phase 21.3 — the capability shortlist

Status: **DONE.** This is the checked-in artifact `doc/plans/2026-08-18-elisp-data-model.md`
names as one of Phase 21's five gate deliverables (top cell-allocation
sources, top lookup/dispatch sources, which workloads collect, arena
margins, and this shortlist). It answers 21.3 alone; the other four are
21.1/21.2's job.

Pin: superproject `059dd8e`, fe `dd35a2b`, `FE_API_VERSION 12`,
`FE_LANGUAGE_VERSION 14`. `./test/kgbatch -a /dev/null` reports
`total=56147 free=50188 peak-live=6819 collections=1 failures=0` after
`kg_lisp_init()` — identical to the plan's own "Baseline at the plan's
pin" section, so this report is measured at the plan's pin, not a moved
one.

## What this is, in one paragraph

Two or three named things kg's own author, running kg's own init file
against a real ELPA tree, would actually reach for next — each with an
exact version/source/licence, the first blocker of every kind the reader
can hit (reader, type, special form, function family), whether that
blocker sits on the *unconditional* load path, and a small acceptance
case. It answers "is the data model material to a named target?", which
is the question Phase 21's gate is conditioned on. It does not answer
"should Phase 22 happen" by itself — that also needs 21.1/21.2's counter
evidence, produced separately — but it answers its own half honestly.

## Method, and a methodology finding that changes the answer

`test/kgbatch` was built plain (`make test/kgbatch`, ordinary `gcc -Os`,
no sanitizer, so the coordinator's `ulimit -v` warning about ASan's
shadow-memory reservation does not apply here) and pointed at real
package files under `/root/.emacs.d/elpa/`, read in place and never
copied into this tree.

`kgbatch`'s `-r` and `-p` flags wrap the whole file inside one
`(condition-case ... (format ... (progn SOURCE)))` expression before
handing it to `kg_lisp_eval_string()`. Reading *that* single wrapping
expression requires Fe's reader to descend into and parse the entire
file as one nested form before any evaluation happens at all — so a
reader error anywhere in the file wins over a runtime error anywhere
before it, regardless of which one a real `load` would hit first.

That is not how kg actually loads a file. `kg_lisp_load_file()` (used by
`require`/`load` and by `kg_lisp_load_init()`) calls
`FeEvaluateFileWithOptions()` → `FeEvaluateFile()` → `EvaluateInput()`
(`fe/fe.c:2769`), whose loop is read-one-form, evaluate-it,
read-the-next — the ordinary Lisp `load` shape. Under that loop, a
`void-function` error on line 34 fires and aborts the load long before
the reader ever attempts to parse a vector literal on line 455.

Plain `kgbatch FILE` (no `-r`, no `-p`) passes the file's bytes to
`kg_lisp_eval_string()` unwrapped, which is exactly `EvaluateInput()`'s
read-eval loop over the raw source — the same shape `load` uses. That is
the mode used below to answer "which blocker actually stops the load";
`-r`/`-p` are used only to see past one blocker without patching the
file, by prepending a one-line shim to a scratch copy (never committed,
never used to change what evidence means).

Measured, on `s.el` (below): plain mode reports
`eval:34: void-function autoload`; `-r` mode on the same unmodified file
reports `eval:457: unsupported read syntax: vector brackets` (line 455
plus `-r`'s two-line prefix). Both are real facts about Fe; only the
first is what a real `load` hits first.

## The shortlist

| # | Capability | Evidence package | Version / source / licence | First blocker (load order) | Unconditional load path? |
|---|---|---|---|---|---|
| 1 | Vectors | `s.el` | 20260522.135 / `7393fa6fa305403e628058c0ec78c35d610fab05` / github.com/magnars/s.el / GPLv3+ | reader: `[&or (function &rest form) fboundp]` at s.el:455, inside `(declare (debug ...))` | **Yes** — every top-level form must be read before `load` can finish, even a `declare` clause Emacs itself never evaluates at runtime |
| 2 | Hash tables | `ht.el` (motivation) + kg's own `utils/forecast/forecast-wordcount.el` (clean measurement) | ht.el 20230703.558 / `1c49aad1c820c86f7ee35bf9fff8429502f60fef` / github.com/Wilfred/ht.el / GPLv3+ | function family: `make-hash-table`/`gethash`/`puthash`/`maphash` all `void-function` | **No, measurably** — kg's own sketch loads clean; the blocker fires only on the one code path that calls the hash-table-based tally, which the file's own alist-based sibling avoids |

Two rows, not three. A third was actively hunted (records, `cl-lib`,
completion-framework packages — see "Rejected" below) and none of it
cleared the same bar these two clear: a named, licence-checked, small
package whose *own* first blocker is cleanly attributable to one family,
without a `require` chain into un-vendored dependencies muddying which
family actually stopped the load.

## Entry 1: Vectors

**Why kg wants it.** Not "kg's own author writes code with vector
literals in it" — the opposite. `[...]` shows up inside `(declare (debug
SPEC))` clauses that Emacs's byte-compiler consults for Edebug
instrumentation and otherwise **ignores at runtime**; it is Elisp's own
mini-language for macro argument shapes. A package does not need to use
vectors as a data structure to trip this — it only needs to document one
macro the way Emacs's own `cl-lib`, `pcase`, and countless small
utility libraries do. `s.el` never constructs a vector value anywhere in
793 lines; the one vector token in it is pure Edebug metadata.

**Exact version, source, licence.** `s.el`, "The long lost Emacs string
manipulation library", Package-Version `20260522.135`, Package-Revision
`7393fa6fa305`, upstream `https://github.com/magnars/s.el`, no
dependencies. Header: "This program is free software; you can
redistribute it and/or modify it under the terms of the GNU General
Public License ... either version 3 ... or (at your option) any later
version" — GPLv3-or-later.

**First blocker of each kind, and which one actually stops the load.**

- *Function family* (the one that actually fires first): `s.el:34`,
  `(autoload 'slot-value "eieio")`. kg has no `autoload` at all — not a
  primitive, not a native, not in `lisp/prelude.el`
  (`grep -n "'autoload" lisp/prelude.el src/*.c` : no hits). Measured:

  ```
  $ ./test/kgbatch -a /root/.emacs.d/elpa/s-20260522.135/s.el
  s.el: eval:34: void-function autoload
  ```

  This is cheap: kg's prelude already has the identical shape for
  `interactive` (`(defalias 'interactive (macro args nil))`,
  `lisp/prelude.el:776`) — an inert no-op macro, because kg has no
  interactive-form machinery that cares about a stray top-level call.
  `autoload` is the same story: kg has no lazy package loading to defer
  to, so a no-op `(defalias 'autoload (macro args nil))` is a complete,
  correct shim, unrelated to Phase 22's arena work. Verified by
  prepending exactly that one line to a scratch copy of `s.el` (never
  committed): the load proceeds past line 34 to the next blocker.

- *Reader* (what stops the load once the function-family gap above is
  patched): `s.el:455`,
  `(declare (debug (form &rest [&or (function &rest form) fboundp])))`.

  ```
  $ printf "(defalias 'autoload (macro args nil))\n" > /tmp/shim.el
  $ cat /tmp/shim.el /root/.emacs.d/elpa/s-20260522.135/s.el > /tmp/s-shimmed.el
  $ ./test/kgbatch -a /tmp/s-shimmed.el
  s-shimmed.el: eval:456: unsupported read syntax: vector brackets
  ```

  Source: `fe/fe.c:2461-2462` — `if (chr == '[' || chr == ']')
  FeHandleError(ctx, "unsupported read syntax: vector brackets");` — a
  named reject, not a misread. Confirmed by direct isolation:
  `./test/kgbatch -r` on a file containing only `[1 2 3]` answers
  `unsupported read syntax: vector brackets`.

- *Type*: none distinct from the reader blocker. `fe/fe.h`'s `FeType`
  enum (`FeTPair`, `FeTFree`, `FeTNil`, `FeTDouble`, `FeTInteger`,
  `FeTSymbol`, `FeTString`, `FeTFn`, `FeTMacro`, `FeTPrimitive`,
  `FeTNativeFn`, `FeTPtr`, `FeTFex0..2`) has no vector slot at all — the
  reader rejects the token because there is no object type to build, not
  because of an incomplete parser. There is no partial vector to bump
  into after the syntax; the syntax rejection *is* the type absence,
  which is exactly why Phase 22/23/24 gate a public vector type on a new
  storage substrate rather than a reader patch.

- *Special form*: none. `[...]` self-evaluates in Emacs, same as a
  string or number; nothing about it is special-form-shaped.

**Is this the same blocker fe's own compat manifest already tracks?**
Yes, and it already has an oracle snapshot: `fe/compat/cases/reader-vector-literal.json`
(`expr: "[1 2 3]"`) under the `reader-hash-syntax-unsupported` entry
(status `divergent`, `fe/compat/features.json`), and kg's own
`test/lisp-compat/cases/phase8-reader-vector.json` with a checked-in
Emacs snapshot (`"record": {"kind": "value", "type": "vector", "printed":
"[1 2 3]"}`, `test/lisp-compat/oracle/phase8-reader-vector.json`). Both
already exist; neither is `supported`. This is the acceptance hook
below.

**Acceptance case.** No new fixture needed — the case already exists and
is already wired to a real Emacs snapshot. When a vector type lands
(Phase 24), two existing entries flip from `divergent` to `supported`:
`fe/compat/features.json`'s `reader-hash-syntax-unsupported` (split so
its vector case can flip independently of `#s(...)` and `#:`, which stay
unsupported) and `test/lisp-compat/features.json`'s
`phase8-reader-unsupported-syntax`. A new, small native/PTY case pins the
value once accepted: `(let ((v [1 2 3])) (and (vectorp v) (= (aref v 1)
2) (= (length v) 3)))` answers `t` — three lines, no vendored fixture,
matching how `phase8-reader-vector`'s Emacs oracle already renders `[1 2
3]` as `type: vector, printed: "[1 2 3]"`.

## Entry 2: Hash tables

**Why kg wants it.** Not primarily "a real package needs it to load" —
measured below, no small real package's load actually stops on a hash
table before something else stops it first. The real motivation is
internal to kg twice over:

1. kg's own forecast corpus already named this as its *only* missing
   family: `utils/forecast/AUDIT.md`'s "Watch item: hash tables,
   vectors, records" table reports 4 references
   (`gethash`/`make-hash-table`/`maphash`/`puthash`), all from
   `utils/forecast/forecast-wordcount.el`, 0 for vectors and records in
   that same corpus.
2. kg's own `completing-read` divergence names hash tables as the
   feature it is missing: `test/lisp-compat/features.json`'s
   `phase16-completing-read-collection` entry (status `divergent`)
   says outright — "Emacs' COLLECTION may be a list of strings, an
   alist, an obarray, a hash table or a function; kg's is a list of
   strings and nothing else ... kg has neither obarrays nor hash tables
   (the latter is the plan's watch item...)".

`ht.el`, "the missing hash table library for Emacs," is named here as
the real-world motivating package precisely because it is the case where
a package's entire value proposition is hash tables — `dash.el`'s
`-uniq`/`-distinct`/`-intersection`/`-difference` (lines 2982-3103)
reach for `(make-hash-table :test test)` + `gethash`/`puthash` the same
way once list lengths cross a size threshold, so this is not an
isolated taste.

**Exact version, source, licence.** `ht.el`, Package-Version
`20230703.558`, Package-Revision `1c49aad1c820`, commit
`1c49aad1c820c86f7ee35bf9fff8429502f60fef`, upstream
`https://github.com/Wilfred/ht.el`, depends on `dash "2.12.0"`. Header:
same GPLv3-or-later grant as `s.el`.

**First blocker of each kind, and which one actually stops the load.**

- *Function family* (what actually stops `ht.el` itself, but for a
  reason unrelated to hash tables): `ht.el:32`, `(require 'dash)`.

  ```
  $ ./test/kgbatch -a /root/.emacs.d/elpa/ht-20230703.558/ht.el
  ht.el: eval:32: Cannot open load file: No such file or directory, dash
  ```

  This is an artifact of the "no vendoring" constraint, not language
  evidence: `dash` genuinely is not on kg's load-path because nothing is
  vendored. Pointing `load-path` at the real, in-place ELPA `dash.el`
  (a diagnostic-only `add-to-load-path` call, never committed, no bytes
  copied) gets one step further and hits `dash.el`'s own first blocker,
  `eval-when-compile` — a different, special-form-shaped gap, not a
  hash-table one. `ht.el` is included here as *motivation* for the
  family, not as the clean measurement.

- *Function family, clean measurement*: kg's own
  `utils/forecast/forecast-wordcount.el` (already checked in, already
  audited, no new corpus needed).

  ```
  $ ./test/kgbatch -a utils/forecast/forecast-wordcount.el
  forecast-wordcount.el: forecast-wordcount
  ```

  The file **loads clean** — every top-level form is `defvar`/`defun`,
  so nothing calls `make-hash-table` merely by being defined. Appending
  one call that actually exercises the hash-table path:

  ```
  $ cat utils/forecast/forecast-wordcount.el > /tmp/wc.el
  $ echo '(forecast-wordcount--tally (list "a" "b" "a"))' >> /tmp/wc.el
  $ ./test/kgbatch -r /tmp/wc.el
  wc.el: E:void-function
  ```

  `(make-hash-table :test 'equal)` is ordinary function-call syntax —
  Fe's reader parses it without complaint. The failure is purely
  "function family," confirmed directly:
  `./test/kgbatch -r` on a file containing only `(make-hash-table)`
  answers `E:void-function`.

- *Reader*: none. No hash-table literal syntax is even attempted by
  either `ht.el` or kg's own sketch — Elisp constructs hash tables by
  calling a function, never by a reader literal, so there is nothing for
  the reader to reject.

- *Type*: fe's `FeType` enum has no hash-table slot, and
  `fe/compat/features.json`'s `hash-tables` entry (status `unsupported`)
  says so explicitly: "Fe has no hash-table type, no reader syntax for
  one, and no primitives that construct or query one." Unlike
  `autoload`, this is not shimmable with a macro — there is no existing
  representation to fake a hash table's identity, mutation or iteration
  over.

- *Special form*: none. Hash tables are built and read purely through
  functions.

**Is this on the unconditional load path? No — measurably.** This is
the header-line finding of this entry and the reason it is scored
differently from vectors despite having the same underlying "no type at
all" severity. `forecast-wordcount.el` loads and most of its interactive
commands work today; the hash-table blocker only fires on the specific
call path through `forecast-wordcount--tally`, which the file's own
`forecast-wordcount--tally-alist` sibling avoids by construction — the
file's own commentary states exactly this trade explicitly ("The alist
spelling underneath `forecast-wordcount--tally-alist` is the same
algorithm without one, and it is here so the report can say what the
fallback costs rather than assert that there is one."). A package author
who wants hash-table ergonomics specifically (like `ht.el`) is blocked
outright; a package author who merely wants a dedup/tally cache can and
does write around it with an alist, at an O(n) cost this report does not
attempt to price (that is 21.1/21.2's job, not 21.3's).

**Acceptance case.** Already tracked and already has an oracle: `fe/compat/cases/unsupported-hash-table-p.json`
(`expr: "(hash-table-p (make-hash-table))"`, `fe/compat/features.json`
entry `hash-tables`, currently `unsupported`). When hash tables land
(named-consumer-gated, Phase 27), that entry flips to `supported`, and
`utils/forecast/forecast-wordcount.el` **graduates out of
`utils/forecast/`** into `lisp/wordcount.el` or similar shipped Lisp —
the exact mechanism `utils/forecast/README.md` already documents
("`forecast-grep.el` is the worked example ... Phase 15 measured its
residual demand at two names ... Phase 17 implemented both and shipped
the package as `lisp/grep-buffer.el`, so the sketch was deleted in the
same commit"). The small case that becomes green:
`(let ((tally (forecast-wordcount--tally (list "cat" "dog" "cat"))))
(gethash "cat" tally))` answers `2` — words have to clear
`forecast-wordcount--interesting-p`'s own `(< 1 (length word))` filter
(`forecast-wordcount.el:34`) to reach the table at all, which single
letters like `"a"`/`"b"` do not; the measured `void-function` failure
above is unaffected by that filter (`make-hash-table` is called
unconditionally, before any word is looked at), but a real acceptance
case has to use words the function actually keeps.

## kg's own future uses

The plan names three explicitly: Lisp keymaps/configuration tables,
package-local caches, structured state. Examined against what kg
actually does in C today.

### Keymaps and configuration tables

`src/keymap.c` is entirely fixed C state today, name-addressed, never a
first-class Lisp value: `keymap_max_maps = 18`, `keymap_max_entries =
256` total across every map (`src/keymap.c:22-23`), and `define-key` /
`local-set-key` (`src/lisp_cmd.c:729-758`) write into that table by
looking up a map *name string*, not by taking or returning a keymap
*object*. There is no `(make-sparse-keymap)`, no keymap value a Lisp
program can inspect, compose, or pass around — Emacs' `where-is-internal`
and friends have nothing to operate on in kg because there is no Lisp
value to operate on.

If kg ever grows a first-class Lisp keymap (the plan's phrasing implies
this is plausible, not that it exists), the honest sizing question is
answered by the C table that already exists: **256 entries is the
editor-wide ceiling today, across 18 maps** — call it low tens of
bindings per map in the realistic case. `src/syntax.c`'s 25 language
tables (`C_HL_extensions[]` etc., `grep -c '_HL_extensions\[\]'
src/syntax.c` → 25) are the same shape for "configuration table": each
one holds 2-8 file extensions. Emacs' own `auto-mode-alist` — the
closest real precedent for exactly this kind of table — is a plain
alist of `(REGEXP . MODE)`, not a hash table or vector, and Emacs runs
it over hundreds of registered modes without complaint.

**Verdict: an alist is honestly sufficient here, and Emacs' own design
choice for the closest analogous table agrees.** Vectors would only
matter if kg wanted to reproduce Emacs' internal single-byte dispatch
optimization (a 128-ish-entry vector per keymap for O(1) char lookup),
which is a C-level performance trick kg's `src/keymap.c` already gets
for free in C — duplicating it in a *Lisp-visible* structure buys
nothing a Lisp program would notice at these sizes.

### Package-local caches

This is the one place alist-is-fine breaks down, and it is the same
finding as Entry 2 above from the other direction: a cache keyed by
something that scales with buffer content — an LSP symbol table, a
syntax/highlight memo, a dedup set over lines — is exactly the shape
`dash.el`'s own `-uniq`/`-distinct` reach for a hash table to avoid
O(n²). kg's own bench corpus already goes to 1M lines
(`utils/bench.py`'s `--big` corpus, referenced in `CLAUDE.md`), so a
per-buffer cache keyed by symbol name or line content is not a
hypothetical scaling case for kg the way it might be for a toy editor.

**Verdict: an alist is not honestly sufficient here past a few hundred
entries**, which is the concrete form of "hash tables matter to kg" this
report can state without needing a synthetic benchmark — 21.1/21.2's
counters are what turn this into a number.

### Structured state

kg already has genuinely record-shaped C data it does not expose to
Lisp as a distinguishable type: `struct dap_breakpoint_info`
(`src/dap_breakpoint.h:120-135`) carries twelve fields (`line`,
`requested_line`, `verified`, `has_id`, `id`, `temporary`, `enabled`,
`anchored`, `condition`, `hit_condition`, `log_message`, `message`,
...). Today anything Lisp-visible built from data like this comes back
as a plist or alist (`interactive-form`/`internal--command-documentation`
already answer this way per `utils/forecast/AUDIT.md`'s covered-name
list).

**Verdict: an alist/plist is sufficient for kg's current sizes** — a
dozen or so named fields per instance is nowhere near where alist lookup
cost matters. A `record`/`cl-defstruct`-shaped type would buy ergonomics
(named accessors, a `type-of` answer, static field-shape checking) more
than performance, which is exactly why the plan's "Recommended execution
summary" leaves records to "compete openly" in Phase 28 rather than
bundling them with vectors — the weakest-motivated of the three named
future uses, honestly reported as such.

## Examined and rejected

- **`dash.el`** (20260221.1346, `d3a84021dbe48dba63b52ef7665651e0cf02e915`,
  github.com/magnars/dash.el, GPLv3+, no deps). Real vector literals at
  dash.el:2124/2745-2746 and real hash-table use at dash.el:2982-3103 —
  genuinely on-topic. Rejected as a *shortlist entry* (kept as
  corroborating evidence inline above) because its own first blocker is
  `eval-when-compile` (`dash.el:46`, `void-function`) — a
  special-form-shaped gap unrelated to this plan's three families — so
  using it as "the" vector or hash-table exhibit would misattribute
  which family stops its load. `s.el` and kg's own sketch give a cleaner
  single-family attribution each.
- **`f.el`** (20241003.1131, requires `s`+`dash`). No vector or
  hash-table use at all (`grep -n "hash-table\|gethash\|puthash\|
  make-hash-table\|\[" f.el`: no hits) — not evidence for this phase.
  Notable only as the box where the form-feed finding below repeated.
- **Completion-stack packages** (`orderless`, `vertico`, `corfu`,
  `marginalia`) and **`magit`**, **`yasnippet`**: every one of these
  fails immediately on an un-vendored `require` (`compat`, `cl-lib`,
  `magit-core`) before reaching any Fe-language evidence at all —
  exactly the trap the plan's Evidence policy warns about ("Counts also
  depend more on which packages were selected than on what kg users
  want to do"). Vendoring their dependency graphs to get past that wall
  is vendoring, which is declined by this plan's own terms; these are
  not usable as clean single-blocker evidence without it.
- **Records/`#s(...)`/`cl-defstruct`**: actively looked for a small,
  licence-clear package whose *own first blocker* is a record literal
  or `cl-defstruct` use, the way `s.el` cleanly exhibits vectors. None
  of the packages surveyed qualify — `cl-defstruct` calls are a macro
  expansion, not a reader literal, so they are gated behind whichever
  `cl-lib`/`eval-when-compile` blocker a `cl-defstruct`-using package
  hits first (as with `dash.el`, `yasnippet.el`). A tree-wide
  `grep -rl '#s(' /root/.emacs.d/elpa/*/*.el` finds exactly one hit in
  the whole ELPA tree — `lsp-mode-20260716.755/lsp-rust.el:945`,
  `(defcustom lsp-rust-analyzer-cargo-extra-env #s(hash-table) ...)`, an
  empty hash table's own printed form used as a default value — inside a
  1935-line file of a package that itself requires seven others
  (`dash`, `f`, `ht`, `spinner`, `markdown-mode`, `lv`, `eldoc`); not a
  small, single-family exhibit the way `s.el` is for vectors.
  `fe/compat/cases/reader-record-literal.json` already tracks the
  reader form (`expr: "#s(a 1)"`, under the combined
  `reader-hash-syntax-unsupported` entry, status `divergent`) with zero
  real measured pressure behind it, matching the "Recommended execution
  summary"'s own treatment of records as "compete openly" rather than
  pre-funded. Not promoted to the shortlist for lack of a measured
  target, exactly as the Evidence policy requires.
- **The form-feed (`\f`, 0x0C) reader-whitespace gap**: found
  incidentally (both `s.el:770` and `f.el:39` hit it), and real —
  `fe/fe.c:2447`'s whitespace-skip set is `strchr(" \n\t\r", chr)`,
  which omits `\f`; a page-break character used as a conventional
  Elisp section separator becomes a symbol-constituent byte, producing
  `void-variable \` rather than being skipped. Confirmed in isolation:
  a bare `\f` byte, or `nil\f\nnil`, both answer `void-variable`
  instead of `nil`. This is real and reproducible but **out of this
  phase's scope**: it is a reader-whitespace classification gap, not a
  vector/hash-table/record type question, and fixing it is unrelated to
  Phase 22's arena/storage work. Recorded here rather than silently
  dropped, and left as a candidate for its own small, separately-scoped
  fix — not folded into this shortlist's family accounting, and not a
  reason to inflate any package's blocker count.
- **The original `utils/forecast/wild/` popularity census**: not run,
  per the plan's Declined section. No aggregate reference count over an
  arbitrarily-selected package set appears anywhere in this document;
  every number above is attached to one named file, one named line, and
  one named error string.

## What this document is not

- **Not vendored test data.** Nothing from `/root/.emacs.d/elpa/` was
  copied into this repository. Every command above reads a package file
  in place from `/root/.emacs.d/elpa/`; the scratch copies used to probe
  past one blocker at a time (a one-line `autoload` shim prepended to a
  temp file, a `load-path` pointed at an in-place `dash.el` directory)
  lived under `/tmp` and were never committed. `utils/forecast/wild/`
  remains empty and untouched.
- **Not an implementation commitment.** No C, Lisp, or build-rule change
  accompanies this document. The acceptance cases named above
  (`fe/compat/cases/reader-vector-literal.json`,
  `fe/compat/cases/unsupported-hash-table-p.json`, and their kg-side
  siblings) already exist as tracked, `unsupported`/`divergent`
  entries with real Emacs oracle snapshots; this document points at
  them rather than adding anything new. Nothing here schedules Phase 22
  through 28 or asserts an order among them beyond what the plan's own
  "Recommended execution summary" already states.
- **Not a popularity census.** No aggregate reference count across an
  arbitrarily chosen package set is used to argue relative importance
  anywhere above. Each entry's evidence is one specific file, one
  specific line, one specific measured error string, and — separately —
  kg's own already-checked-in forecast/compat evidence. Fifty
  hypothetical calls to an optional helper are not weighed against one
  vector literal that blocks a whole file; per the plan's own Evidence
  policy, they should not be.

## Verified

```
$ git -C /work/.parallel-elisp/p21cap log -1 --format=%H
059dd8e8b9741f45f632d40868995b40dbac743c
$ git -C /work/.parallel-elisp/p21cap/fe rev-parse HEAD
dd35a2b934c5d35db3d5491ed9cc3a7da753812d
$ make test/kgbatch                     # plain gcc -Os, no sanitizer
$ ./test/kgbatch -a /dev/null
/dev/null: nil
census: total=56147 free=50188 peak-live=6819 collections=1 failures=0
$ ./test/kgbatch -a /root/.emacs.d/elpa/s-20260522.135/s.el
s.el: eval:34: void-function autoload
$ ./test/kgbatch -a /root/.emacs.d/elpa/ht-20230703.558/ht.el
ht.el: eval:32: Cannot open load file: No such file or directory, dash
$ ./test/kgbatch -a utils/forecast/forecast-wordcount.el
forecast-wordcount.el: forecast-wordcount
```

No file under `src/`, `fe/`, `lisp/`, `utils/`, or `test/` changed as
part of this document. `git status --porcelain` shows only this new
file.
