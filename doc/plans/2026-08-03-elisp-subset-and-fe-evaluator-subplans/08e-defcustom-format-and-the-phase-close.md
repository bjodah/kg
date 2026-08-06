# 08E — `defcustom`, `format`, loader diagnostics, and the phase close

Parent: [Phase 8](../2026-08-03-elisp-subset-and-fe-evaluator.md#12-phase-8--core-init-file-compatibility-roadmap),
kg-only, no pin move; closes the set.

**Prerequisite:** [08D](08d-the-pin-and-the-free-library.md) (keywords are
live in kg, the library batch is in, the init-file case passes its 08D
subset).

## Outcome

The three remaining init-file staples work, and a failing init file tells
the user where it failed:

```lisp
(defcustom my-fill-column 72
  "Column beyond which to break lines."
  :type 'integer :group 'editing)          ; => my-fill-column, inert keywords
(custom-set-variables '(tab-width 4))      ; => applied, kg's documented subset
(message "at %-8s col %d %c" name col ?»)  ; => widths, %c
```

and `kg` started with a broken `init.el` reports
`init.el:LINE: <the condition>` in the status line (08C gave fe the line;
this slice makes kg's surfaces show it).

## Mechanics

1. **`defcustom`** exactly per parent §12's contract, as a prelude macro
   over `defvar` (08A pinned its oracle cases): initializes only when
   unbound; standard form not evaluated when bound; docstring recorded
   (the 08D `documentation` table serves variables too — decide the
   variable-doc spelling there consistently); keyword tail must be pairs,
   unknown keywords error; presentation keywords (`:type` `:options`
   `:group` `:tag` `:link` `:version` `:package-version`) accepted and
   inert — documented, tested divergence; semantics-bearing keywords
   (`:initialize` `:set` `:get` `:require` `:set-after` `:risky` `:safe`
   `:local`) rejected clearly. Returns the symbol.
2. **`custom-set-variables`** as the minimal measured subset: each
   argument a `(SYMBOL VALUE)` (quoted) form; sets the global; extra
   Custom metadata in an entry is rejected, not ignored. Record the
   divergence set explicitly (no Custom state, no `custom-file`).
3. **`format` grows the measured directives**: `%c` (codepoint → UTF-8,
   byte policy per 08A Table D), `%x` `%X` `%o`, and field width/flags
   (`-`, `0`, precision) for the numeric/string directives, matching the
   Table D oracle rows. Implementation stays in `src/lisp_io.c`; the
   audit measured the current directive set at six with no widths.
   `%%` policy re-checked against Emacs.
4. **Loader diagnostics kg-side**: the init-load error path surfaces
   fe's `file:LINE` (08C) verbatim in the status message and `*Messages*`
   equivalent; `load`'s own error for an unreadable/missing file names
   the resolved path tried. `describe-command` may now read the stored
   docstring for Lisp commands (07D stored it write-only — decide here
   whether to surface it or record why not; the review called it
   write-only as a finding).
5. **`declare`** in defun/defmacro docstring position becomes an
   accepted no-op returning nil (measured Emacs treats unknown declare
   specs as ignorable) — one line in the `defun` macro, kills a whole
   class of init-file breakage. Record which position is recognised.

## The init file, closed

Extend 08D's PTY init-file case with the 08E forms until it is 08A's full
30-line corpus verbatim, loading with zero errors and observable effects
(a set variable, a bound key, a defined command). That case passing is
the phase's acceptance.

## Tests owned by this slice

- `test/test_lisp.c`: defcustom all branches (unbound/bound/re-eval/
  malformed tail/rejected keyword — each an 08A-pinned oracle case);
  custom-set-variables subset + rejections; format directive matrix
  against Table D including boundary widths and `%c` of ASCII, Latin-1,
  and 4-byte codepoints; declare no-op in both positions.
- PTY: the full init-file case; a broken-init case asserting the
  `file:LINE` status surface (tmux `expected_screen_contains`).
- Manifests: defcustom's `planned` row flips with its runnable case;
  format rows; declare row; diagnostics as kg-policy rows.

## Documentation

`README.md`/`doc/lisp-api.md`: defcustom/custom-set-variables subset and
divergences, format directive table, declare, the `file:LINE` contract.
`doc/kg.1`: init-file diagnostics paragraph. `doc/TODO.md`: strike the
landed items; carry the recorded Phase 8 second-set candidates
(`string-match` surface, `symbol-name`/`intern`, vectors, `load-path`
variable, sub-form source precision, `read-*` functions — carried from
07E).

## The phase close

Both complexity commands at start and end with actuals recorded against
08A's funding; then Rule 9 in full: `make check` idle, `WITH_LISP=0
clean all check`, `header-check`, `docs-check`, `lisp-compat-check`,
`lisp-prelude-check`, oracle verify-by-running, and `JOBS=8
.ci/run-ci-steps.sh --parallel` after regenerating
`compile_commands.json`.

The Status entry records per-slice actuals (including Part 2's ≈+0 scc
claim, honestly re-measured), what the plan got wrong, arena peaks, and
the carried second-set candidates. The five 08 documents are deleted only
after reviewer acceptance.

## What this does not do

- No Customize UI, `defgroup`, `setopt`, `custom-file`, or Custom state.
- No `%S` print-depth work, no `format-message` curly quotes.
- No general `message` ring or `*Messages*` buffer.
- No new fe surface and no edits in the fe submodule. If a builder
  appears to need one, stop and review the design first.
