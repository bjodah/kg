# 08C — An honest reader: reject, then implement, then say where

Parent: [Phase 8](../2026-08-03-elisp-subset-and-fe-evaluator.md#12-phase-8--core-init-file-compatibility-roadmap),
fe-only, second and last fe slice of the set. No kg edits; the pin moves
in 08D.

**Prerequisite:** [08B](08b-constants-and-keywords-in-fe.md) (keywords must
already read and evaluate; the escape work below feeds `?\s`-class forms
that produce integers, which Table R pins).

## Outcome

Three lies stop:

1. **Nothing is silently misread.** Today `?x`, `#xff`, `[1 2 3]`,
   `#:sym`, and `"\x41"` all parse as innocent-looking wrong values
   (symbols, or strings with the backslash stripped). After this slice a
   form the reader does not support is a *read error naming the syntax*,
   per §12's own rule.
2. **The measured subset works.** `?a` → 97, `?é` → 233, `?\n`/`?\e`/
   `?\C-a`-class modifiers per 08A Table R; `#x`/`#o`/`#b` radix
   integers; the shared string-escape table (`\xHH`, `\NNN` octal, `\e`,
   `\d`, `\s`, `\a`, `\b`, `\v`, `\f`, and the measured rest of Table R).
   One escape table, two call sites (string bodies and `?` literals).
3. **Errors say where.** `(load "init.el")` failures report `init.el:LINE`
   with a real line number (the reader counts newlines), and a *runtime*
   error in a loaded file still carries the position of the form being
   evaluated — today `EvaluateInput` clears `error_has_offset` before
   `FeEvaluate`, so a bad init line reports no position at all.

## Mechanics

1. **Reject arms first, one commit**: `?` (until implemented within this
   slice), `#` followed by anything but `'` or the radix letters, `[`,
   `]`, `#:`, and any string escape outside the table → `FeHandleError`
   with "unsupported read syntax: …". This commit alone converts the
   silent-misread class into diagnostics and is separately revertible.
2. **The escape table**: a single static table mapping escape spellings to
   codepoints, consumed by the string reader and the `?` reader. Measure
   Emacs for every entry before pinning (08A Table R rows). Unknown
   escapes error (Emacs's own behavior for e.g. `"\q"` is `\q` → `q`?
   — measure; if Emacs strips silently, kg still errors and records the
   divergence: an init-file dialect earns strictness).
3. **`?` literals**: after `?`, either an escape-table entry, a modifier
   chain (`\C-`, `\M-`, `\s-`? — pin only what Table R measured; reject
   the rest), or one UTF-8 sequence decoded to its codepoint. The decoder
   is a fixed-count loop on the lead byte; no pushback needed. Result is
   a plain `FeTInteger`.
4. **Radix integers**: `#x`/`#o`/`#b` with optional sign per Emacs;
   overflow follows the reader's existing integer-overflow policy
   (measure what Emacs does at 63 bits before deciding to mirror or
   diverge-and-record).
5. **Line counting**: the read path tracks the line (newline count + 1)
   of the form being read; `ReadEvaluatedFile` stores it where it today
   stores the byte offset, and the error formatter prints `file:LINE`.
   Runtime: `EvaluateInput` records the line of each top-level form
   before evaluating it, so a runtime raise inherits the form's line.
   Sub-form precision is out of scope (recorded); top-level-form line is
   the contract.
6. **Symbol-length**: while in the reader, name the 63-byte symbol cap in
   the error ("symbol too long" names the cap) — the audit hit it
   blind. No cap change.
7. **Version**: `FeVersion` "7.0"→"8.0" here (the phase's last fe slice),
   with `FE_LANGUAGE_VERSION` already 7 from 08B (reader changes ride the
   same language version — decide in 08A whether the reject arms are
   language 7 or need their own bump; one bump for the phase is the
   default).

## Tests owned by this slice

- `test_api.c`: every Table R row, both accepted values and rejected
  spellings (assert the error names the syntax); line numbers for read
  errors at line 1, line 2, and after a multi-line form; runtime-error
  line for a top-level form on a known line; UTF-8 `?` literals for 2-,
  3-, and 4-byte sequences; radix bounds.
- Script suite: reader cases runnable by `fe` directly.
- Compat: Table R rows become runnable cases with runner-produced
  snapshots; `reader-char-literal-unsupported` retires with a rationale
  naming this slice; new divergence rows for anything deliberately
  stricter than Emacs.
- Fuzz: the reader fuzzer's dictionary gains `?`, `#x`, and escape
  spellings; confirm the new arms are reachable (counts in the commit
  body).

## Documentation

`doc/language.md` reader section rewritten: supported syntax, rejected
syntax (as a table), escape table, line-number contract.
`doc/fe-upstream.md` is kg's document — 08D updates it at the pin.

## Gates

fe's Rule 9 in full, as 08B. Priced **+60..110 scc** — the biggest fe
slice of the set; `fe.c` measures 100 against its 520 per-file cap at
the Phase 7 close, the roomiest file in the program. Split helpers
before touching any cap; new functions ≤ pmccabe 15.

## What this does not do

- No vectors, no `#s(...)`, no symbol escapes (`a\ b` stays rejected,
  recorded), no `#N=` circular syntax, no bignums (radix overflow follows
  existing policy).
- No printer changes: `?a` prints as 97; char literals are read syntax
  only, exactly as in Emacs.
- No kg edits, no pin move, no prelude changes (the backquote fixes are
  08D's — they are prelude macros, not reader syntax).
