# 02A — Pin `setq`, `set` and numeric `=` before implementing them

Parent: [Phase 2](../2026-08-03-elisp-subset-and-fe-evaluator.md#6-phase-2--hard-cut-assignment-and-numeric-),
Fe compatibility-corpus work followed by a kg gitlink update.

**Prerequisites:** none beyond the completed first set.  This is Phase 2's
first slice.  It writes JSON and Lisp case strings, but no C or header.

## Outcome

Land the Emacs 31 answers that 02B and 02C must implement.  The new cases
belong in Fe's corpus because all three constructs will be Fe-core constructs:

- `fe/compat/features.json` — three planned feature rows;
- `fe/compat/cases/*.json` — one small observable per case;
- `fe/compat/oracle/*.json` — version-stamped Emacs answers, generated only
  by the existing runner.

Do **not** duplicate these cases under `test/lisp-compat/`.  kg's existing
`prelude-setq` row describes the temporary kg macro, not the future core form,
and remains unchanged until 02D deletes that macro.  Ownership is the reason
the two corpora exist.

After the Fe data commit passes, move kg's gitlink in a separate commit and
run kg's structural checker.  Rule 10 still applies to data-only submodule
changes.

## Oracle answers already resolved for the implementation contract

The non-obvious answers were checked with the pinned GNU Emacs 31.0.90 using
lexical evaluation (`eval`'s `LEXICAL` argument):

- `setq` updates the innermost lexical value binding when one exists; without
  one, it writes the symbol's global value cell.
- `set` always writes the symbol's global value cell.  A lexical binding of
  the same spelling is neither read nor changed.  In a lexical `x = 1`,
  `(set 'x 2)` returns `2`, the lexical `x` remains `1`, and the global `x`
  becomes `2`.
- `setq` processes complete pairs before diagnosing a dangling final symbol;
  those earlier assignments stand.  By contrast, `set`'s fixed arity is
  rejected before any supplied argument form is evaluated.
- For an arity-correct ordinary call, `set` and numeric `=` evaluate all
  argument forms left to right before validating their types.  A type error
  in an early value therefore does not erase side effects from later argument
  forms.
- `(=)` signals `wrong-number-of-arguments`; `(= 1)` returns `t`.  Two or
  more arguments are compared as one chain.
- Numeric `=` evaluates every argument left to right even after the result is
  already known to be false, because it is an ordinary function in Emacs.

02A's checked-in snapshots remain the authority.  These prose answers are
included so a junior implementing 02B does not have to rediscover which
binding cell `set` means.

## Use the machinery as it exists

The relevant paths and commands are:

- `fe/compat/{features.json,cases/,oracle/}`;
- `fe/utils/run-emacs-oracle.py` and `fe/utils/run-fe-compat.py`;
- `make -C fe compat-oracle` to generate/verify every Fe snapshot;
- `make -C fe compat` to replay Fe against checked-in snapshots;
- `utils/check_lisp_compat.py`, run by kg's `make lisp-compat-check`, to
  verify both manifests and their source-name coverage.

The case schema is deliberately small: `id`, `setup`, `expr`, and `note`.
Each runner uses a fresh process per case.  Do not write a case that depends
on globals left by a preceding case, and do not add a runner, schema field,
Make target, or source linter.

## Manifest edits and `source_name` ownership

Add Fe-owned, `comparison: "emacs"`, `status: "planned"` rows for core
`setq`, `set`, and numeric `=`.  Name Phase 2 in each rationale, as
`utils/check_lisp_compat.py` requires for every planned row.
Use the stable ids `primitive-setq`, `primitive-set`, and
`primitive-numeric-eq`; name their case files `<feature>-<property>.json`
from the property labels below so later status transitions do not rename ids.

Use these `source_name` transitions; they prevent the inventory checker from
reporting either a duplicate or a source construct that does not exist yet:

| Feature | 02A `source_name` | Later transition |
|---|---|---|
| new core `setq` | `null` | `"setq"` in 02B, when `primitive_names[]` gains it |
| new core `set` | `null` | `"set"` in 02B |
| numeric `=` | `null` | `"="` in 02C |
| existing `primitive-assign-eq` | remains `"="` | delete the row, its case, and its snapshot in 02C |
| kg `prelude-setq` | remains `"setq"` in kg's manifest | delete the row, case, and snapshot in 02D |

Do not give both meanings of `=` the same `source_name`, and do not delete
the existing divergence before the assignment primitive is actually gone.

## Case matrix

Use separate, narrowly named case files so a failure says which rule moved.
Several case ids may be listed in one feature row; Fe's oracle target walks
all files, not only the first case.

### Core `setq`

Record at least these observables:

| Property | Representative expression | Oracle result |
|---|---|---|
| zero pairs | `(setq)` | `nil` |
| left-to-right and final value | `(setq a 1 b a)` | `1` |
| lexical update, global untouched | `((lambda () (setq x 9) (list ((lambda (x) (setq x 2) x) 1) x)))` | `(2 9)` |
| new global | `(setq fresh 7)` | `7` |
| odd form count | `(setq a 1 b)` | `wrong-number-of-arguments`; `a` was assigned before the error |
| target must be a symbol | `(setq 1 2)` | `wrong-type-argument` |
| value error propagates | `(setq a missing-value)` | `void-variable` |

The lexical case proves both halves of the rule in one isolated process.
Do not use kg's prelude in these cases; standalone Fe is the implementation
under test after 02B.

### `set`

Record:

| Property | Representative expression | Oracle result |
|---|---|---|
| symbol argument is evaluated and value returned | `(set 'fresh 7)` | `7` |
| ignores lexical binding and writes global | `((lambda () (setq x 9) (list ((lambda (x) (list (set 'x 2) x)) 1) x)))` | `((2 1) 2)` |
| first value must be a symbol | `(set 1 2)` | `wrong-type-argument` |
| exact arity | `(set 'x)` and `(set 'x 1 2)` | `wrong-number-of-arguments` |

The lexical case is the regression that must fail if a later cleanup aliases
`set` to `setq` or calls `GetBound(..., env)` instead of the global setter.

### Numeric `=` over Fe's existing doubles

Record:

| Property | Representative expression | Oracle result |
|---|---|---|
| minimum arity | `(=)` / `(= 1)` | `wrong-number-of-arguments` / `t` |
| chain, true and false | `(= 1 1 1)` / `(= 1 1 2)` | `t` / `nil` |
| all operands are evaluated | the expression below | `(nil 3)` |
| signed zero | `(= 0.0 -0.0)` | `t` |
| NaN | `(= (sqrt -1) (sqrt -1))` | `nil` |
| wrong type | `(= 1 "1")` and `(= 1 nil)` | `wrong-type-argument` |
| documents the hard cut | `(= never-bound 3)` | `void-variable` |

Use `(sqrt -1)` for NaN.  Fe's reader does not accept Emacs' printed
`0.0e+NaN` token, while both runtimes already provide `sqrt`; testing the
reader here would conflate Phase 2 with a separate reader divergence.

```lisp
((lambda ()
   (setq seen 0)
   (list (= 1
            ((lambda () (setq seen 2) 2))
            ((lambda () (setq seen 3) 3)))
         seen)))
```

The evaluation-order expression must use only names standalone Fe has by
02B (`lambda`, `setq`, `list`, `=`); do not use kg's `progn` prelude macro.

Phase 5 will add integer/mixed-number cases beside these.  Do not add a case
whose expected answer Phase 5 intentionally changes.

## Error records before the condition system

Phase 2 needs the message names `wrong-number-of-arguments` and
`wrong-type-argument`.  `arith-error` belongs to Phase 5 overflow work, not
numeric equality.  Structured conditions arrive only in Phase 6.

The Emacs snapshot therefore has `condition_source: "structured"`, while Fe
will eventually report `condition_source: "message"`; the comparator requires
the oracle's condition name to appear in Fe's `FeHandleError()` text.  This is
an implementation requirement for 02B/02C: existing generic messages such as
`too few arguments` and `expected double` are not sufficient for these new
supported features.

Do not assert a catchable condition object, exact Emacs error data, or a
condition hierarchy in this slice.

## Focused procedure and gates

1. From an idle tree, record both complexity gates in both repositories.
   They must not move because no scanned C changes.
2. Add the Fe manifest rows and case files with `status: "planned"`.
3. Run `make -C fe compat-oracle`; review the generated records, especially
   lexical `set`, unary `=`, and NaN.  Do not use
   `--allow-version-change` unless the pinned oracle was intentionally moved.
4. Run `make -C fe compat`.  Every new case must be reported as a known gap,
   not a failure; all previously supported cases still pass.
5. Run `make -C fe check`, then land the Fe data commit.
6. Move kg's gitlink and run `make lisp-compat-check` and `make check`.
7. Re-run both complexity gates in both trees and record unchanged measured
   values in the commit message.

No native unit or PTY test is added here: no runtime behavior changes.  The
oracle snapshots are the deliverable, and running interactive editor tests
would not test anything this slice owns.

## What this does not do

- It does not implement any of the three constructs.
- It does not alter or delete assignment `=`.
- It does not modify kg's prelude-setq case or rename a file.
- It does not add integers, reader syntax for NaN, structured conditions, or
  migration infrastructure.

## Status

**Complete, 2026-08-04.**  Fe commit `ecb1110` on `analyzers-etc` adds the
three planned rows (`primitive-setq`, `primitive-set`,
`primitive-numeric-eq`, all `comparison: emacs`, `owner: fe-core`,
`source_name: null`) and **22 cases** with version-stamped snapshots from the
pinned GNU Emacs 31.0.90; kg's gitlink moved in `bbdb608`.  No C or header
changed in either tree, and neither complexity gate moved: fe scc 214/220
(`fe.c` 106/112), pmccabe 198 symbols; kg scc 5443/5500, pmccabe 1246
symbols; 0 new/gone/improved in both.

**Every one of the 22 oracle answers matched this document's predicted
table** — including the four most easily got wrong by hand: `setq` updates
the innermost lexical binding and leaves the global cell alone (`(2 9)`),
`set` writes the global cell straight through a same-named lexical binding
(`((2 1) 2)`), `0.0 = -0.0` is `t`, and NaN is not `=` to itself.  Nothing
had to be argued after the fact, which is the whole reason this slice came
first.

Two properties the single-expression protocol structurally cannot record are
written into the case notes rather than left implied: that `a` survives the
error in `(setq a 1 b)`, and that a `set` arity error evaluates no argument.
Both are handed to 02B's native test matrix, which already lists them.

`make -C fe compat` reports all 22 as known gaps and 0 failures (66 cases, 31
passed, 35 known gaps); `make lisp-compat-check` sees 183 features across both
manifests; `make check` is 32 native / 405 PTY.

**One defect found and fixed on the way through** (fe `cf36951`, kg pin
`61363f9`): `make -C fe compat-oracle`'s full run could not finish, and had
not been able to since the corpus outgrew 00B's five proof-of-mechanism
cases.  The runner enumerated by globbing `cases/*.json` rather than reading
`features.json`, so it ran Emacs over the six `comparison: kg-policy` cases
that have no oracle answer by design, and aborted on `primitive-print` —
whose expression writes to stdout, breaking the shim's one-record-per-line
protocol.  02A itself worked around it with `--case`; the fix makes the
documented target usable for 02B–02D.  kg's `make lisp-compat-oracle` drives
the same runner and had the same failure latent in it.  The full runs now
complete with **no snapshot changed** — fe 60 unchanged / 6 not compared, kg
53 unchanged / 83 not compared — which re-verifies every checked-in snapshot
in both corpora, 02A's included, against the pinned oracle.  The `oracle/`
directories already held exactly the 60 and 53 files the manifest rule
selects, so the data was right and only the enumeration was wrong.
