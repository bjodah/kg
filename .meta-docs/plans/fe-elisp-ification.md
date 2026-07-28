# Plan: making kg's Lisp feel like Emacs Lisp

Investigation date: 2026-07-28.  Every behavioural claim below was executed
against a scratch clone of the `fe/` submodule (`8873622`, `analyzers-etc`)
driven by a standalone C harness that reproduces kg's embedding (1 MiB arena,
`FeEvaluateStringWithOptions`, step budget, `longjmp` error handler).  The
appendix records the snippets and their real output.

## 1. Executive summary

The headline result is that **almost all of the elisp surface can be bought in
the prelude for zero divergence from upstream fe.**  fe's primitives are
ordinary global bindings holding `FeTPrimitive` objects, so they can be
aliased, shadowed and even *renamed out from under themselves* by plain
assignment.  That single fact answers both of the owner's questions and most of
the wider gap.

Recommendation, in priority order:

1. **Do it in the prelude.**  Grow `lisp_prelude[]` in `src/lisp.c` from ~25
   lines to ~135 lines of Fe.  That buys `lambda`, `defun`, `defmacro`,
   `setq`, `progn`, `defvar`, `defconst`, `let`/`let*` with elisp binding
   lists, elisp `if` semantics, `&optional`/`&rest`, `(interactive)`
   auto-registering a command, `null`/`eq`/`equal`, quasiquote *semantics*,
   and the list library (`length` `nth` `nthcdr` `last` `reverse` `append`
   `mapcar` `assoc` `member` `dolist` `dotimes` `push`).  Measured cost:
   ~40 KiB of the 1 MiB arena (4 %) and ~6–7 k of the 1 048 576-step budget
   (0.7 %).  Submodule cost: **zero**.
2. **Add a handful of natives in `src/lisp.c`** (not fe): `type-of` and the
   predicates built on it (`stringp`, `symbolp`, `numberp`, `consp`,
   `functionp`).  `FeGetType` and `type_names` are both public in `fe.h`, so
   this is kg-side code that the submodule pin never sees.
3. **Take exactly one fe.c change: the backquote reader** (24 inserted lines
   in `Read()`, plus two characters in the delimiter string).  Quasiquote
   *semantics* are prelude-only, but the *sugar* (`` ` ``, `,`, `,@`) is not
   reachable from the prelude, and writing macros without it is the single
   biggest remaining papercut.  This is the one place where the syntax, not
   the bindings, has to move.
4. **Optionally** take two more one-to-eleven-line fe.c changes: a
   `void-function NAME` error message, and `GcStackSize` 512 → 4096.  Both are
   quality-of-life; see §2 for what they buy.

What I would **not** do:

- **Do not rename `fn` to `lambda` by editing fe.c.**  `(= lambda fn)` in the
  prelude is exact, costs one line, and keeps `fn` working for anything that
  already emits it.  The only observable difference from a real rename is that
  `FeWrite` prints closures as `(fn ...)`.  If that matters, it is a one-word
  fe.c change — but it breaks fe's own `tests/*.out` golden files, so it is
  the worst value-per-divergence item on the list.  Recommend: leave it, or
  fix it in kg's own printer if kg ever gets one.
- **Do not implement `defun` in C.**  It is a nine-line prelude macro and it
  works, including docstrings, `&optional`, `&rest` and `(interactive)`.
- **Do not chase real elisp dynamic binding, `unwind-protect`,
  `condition-case`, keyword args, vectors, hash tables, buffer-local
  variables, or a byte compiler.**  Each needs deep surgery in `Evaluate`
  and none of them is what makes an `init.fe` *feel* like elisp.
- **Do not add a `?a` character-literal reader or `#'` function quote.**  Both
  currently read as ordinary symbols and therefore evaluate to `nil`
  *silently* (verified).  That is a genuine footgun, but the cheap fix is
  documentation plus, if desired, prelude bindings for the handful of
  characters people actually type — not a reader fork.  If the `#'` silence is
  judged unacceptable, prefer the `void-function` error change (item 4), which
  turns `(#'foo)` into a named error instead of a silent `nil`.
- **Do not make `=` mean numeric equality in the same release as everything
  else.**  It is *possible* — `(setq = (lambda (a b) (is a b)))` works, tested
  — but it is a semantic trapdoor: after it, a stray `(= x 1)` silently
  computes a boolean instead of assigning, with no error.  Ship it as its own
  phase, last, once every internal user of `=` has been converted to `setq`.

### The trap that shapes everything

fe evaluates a macro call by **overwriting the caller's cons cell with the
expansion** (`*obj = *DoList(...)` in `Evaluate`).  Two consequences, both
measured:

- **Macros expand exactly once per call site**, and the expansion is cached
  *in the source*.  A macro whose expansion depends on runtime state is
  silently wrong.  Verified: a macro with a side-effecting body called three
  times incremented its counter once, and printing the enclosing function
  showed the expansion had replaced the call.  Every macro proposed here is a
  pure function of its unevaluated argument forms.
- **A macro that expands to bare `nil` produces a broken nil.**  The copy is
  nil-*typed* but not pointer-equal to `&nil`, and fe's nil test is pointer
  equality.  Measured: such a value prints as `nil`, but `(is x nil)` → `nil`,
  `(not x)` → `nil`, and `(if x "T" "f")` → `"T"`.  Every macro must emit
  `(list 'quote nil)` instead — the reason kg's `cond` already does.  Atoms of
  every other type survive the copy intact (symbols keep working because the
  copy shares the symbol's value cell; numbers and strings compare equal).

## 2. Feature table

Effort is "one engineer, including tests".  "Prelude" means `lisp_prelude[]`
in `src/lisp.c`, zero submodule impact.

| elisp feature | What it takes | Lands in | Effort | Risk |
| --- | --- | --- | --- | --- |
| `lambda` | `(= lambda fn)` — aliases the `FeTPrimitive` object | Prelude | 1 line | none |
| `defun` | 9-line macro over `fn`, incl. docstring, `&optional`, `&rest`, `(interactive)` | Prelude | 1 h | low |
| `defmacro` | same shape over `macro` | Prelude | 15 min | low |
| `setq` | `(= setq =)` **plus** a macro for the multi-pair form (fe's `=` silently drops extra pairs — verified) | Prelude | 30 min | low |
| `progn` | `(= progn do)` | Prelude | 1 line | none |
| `defvar` / `defconst` | macros over `=`; "unbound" is indistinguishable from `nil` in fe, so `defvar` will re-initialise a variable whose value is `nil` | Prelude | 20 min | low (documented divergence) |
| `let` (elisp binding list, **parallel**) | macro → immediate lambda application `((fn (a b) body) 1 2)`; verified parallel | Prelude | 1 h | low |
| `let*` | macro → `(do (fe-let a 1) (fe-let b 2) body…)`; needs fe's `let` kept under an internal alias | Prelude | 30 min | low |
| elisp `if` (ELSE is an implicit progn) | **required**: fe's `if` is an elif chain, so `(if c a b1 b2)` silently runs `b1` as a *condition* and never runs `b2` (verified) | Prelude | 30 min | medium — a real semantic conflict, must ship with the rest |
| `&optional` | strip the marker from the arglist; missing args already bind to `nil` in `ArgsToEnv` | Prelude | 20 min | none |
| `&rest` | rewrite `(a &rest r)` → dotted `(a . r)` | Prelude | 20 min | none |
| docstrings | already inert (evaluated and discarded); an alist registry + `documentation` is ~6 more lines | Prelude | 30 min | none |
| `(interactive)` | `defun` scans the body, strips it, and emits `(define-command 'name name)`.  kg's `define-command` already accepts a **symbol** (`copy_fe_string` → `GetStringObject` handles `FeTSymbol`) — verified | Prelude | 45 min | low |
| quasiquote **semantics** | `(quasiquote (a (unquote b) (unquote-splicing c)))` as a macro + iterative `append` | Prelude | 45 min | low |
| quasiquote **sugar** (`` ` `` `,` `,@`) | new `case '\`'` / `case ','` in `Read()`; 24 inserted lines, + 2 chars in the delimiter string | **fe.c** | 2 h incl. upstream PR | low-medium (see §5) |
| `null` | `(= null not)` | Prelude | 1 line | none |
| `eq` / `equal` | `(= eq is)` / `(= equal is)`; fe's `is` is pointer-eq for symbols/pairs, near-eq for doubles, content-eq for strings — i.e. `equal` for strings, `eql` for numbers | Prelude | 2 lines | low (documented divergence: `(equal '(1) '(1))` → `nil`) |
| `=` as numeric comparison | `(setq = (lambda (a b) (is a b)))` — works, but permanently retires `=`-as-assignment | Prelude | 15 min | **high** — silent misbehaviour for any stale `(= x v)`; own phase |
| `car`/`cdr` on `nil` | already elisp-correct (`GetPairMember` returns `nil` for `nil`) | — | none | none |
| `length` `nth` `nthcdr` `last` `reverse` `append` `mapcar` `assoc` `member` | `while`-loop implementations.  **Must not be recursive**: fe's GC stack is 512 slots and a self-call costs ~6, so recursion dies at ~70 frames (measured: depth 70 OK, 80 → "GC stack overflow") | Prelude | 2 h | low |
| `dolist` `dotimes` `push` | macros over the above | Prelude | 30 min | low |
| `stringp` `symbolp` `numberp` `consp` `functionp` `type-of` | natives over the public `FeGetType` + `type_names`; **not** expressible in the prelude (fe's `atom` only splits pair/non-pair) | `src/lisp.c` natives | 1 h | none |
| better errors (`void-function NAME`) | 11 lines in `Evaluate`'s non-callable branch | **fe.c** | 30 min | low |
| deeper recursion | `GcStackSize` 512 → 4096; measured depth 70 → ~550.  Costs 28 KiB of `FeContext`, i.e. `FeMinimumArenaSize` grows by 28 KiB (kg allocates 1 MiB) | **fe.c** | 15 min | low |
| print closures as `(lambda …)` | one word in `FeWrite` — but it breaks fe's own golden test outputs | fe.c | 15 min | **not worth it** |
| `?a` char literals, `#'` function quote | reader cases; today both read as symbols and evaluate to `nil` silently | fe.c | — | **not worth it** |
| `unwind-protect`, `condition-case` | needs a real non-local-exit mechanism; fe only has `FeHandleError` → `longjmp` out of the whole evaluation | deep surgery | — | **out of scope** |
| dynamic binding, buffer-local vars, keyword args, vectors, hash tables | new object types and `Evaluate` changes | deep surgery | — | **out of scope** |

## 3. Phased implementation plan

The prelude is a C string literal, so each phase is a diff to
`lisp_prelude[]` in `src/lisp.c` plus tests.  **Ordering inside the prelude is
load-bearing**: every alias of a primitive must be taken *before* that
primitive's name is shadowed, and no macro defined after a rename may emit the
old name.  Keep the phase comments in the literal so the ordering constraint
is visible at the edit site.

### Phase 0 — housekeeping (do first, independent)

`doc/fe-upstream.md` pins `0dc79f2a9db6f4646c11200c65d7f315e3ce27e0`, but the
checked-out submodule is `88736222c0f14666056f828170e8556f1d939335`, which
already carries the `sin`/`cos`/`expt`/`sqrt`/… natives directly in `fe.c`.
The doc is stale.  Fix the pin line before any of this work starts, otherwise
step 3 of the update procedure ("confirm `FE_API_VERSION`") is being run
against the wrong baseline.

*Test*: none; documentation only.

### Phase 1 — aliases and elisp `if`

Prepend to `lisp_prelude[]`:

```
(= setq =) (= progn do) (= lambda fn) (= null not)
(= fe-let let) (= fe-if if)
(= if (macro (test then . else) (list 'fe-if test then (cons 'progn else))))
```

Then convert the *existing* prelude bodies (`cond`, `when`, `unless`,
`dolist`, `string-empty-p`, `thing-at-point`) to emit `fe-if`/`progn`/`fe-let`
rather than `if`/`do`/`let`, because Phases 3–4 shadow those names.

*Test*: extend `test/test_lisp.c` with an `if`-arity case that would fail
under fe's elif semantics — `(if nil 1 (setq probe 2) (setq probe 3))` must
leave `probe` at 3.  Add `test/pty/lisp-elisp-if.yaml` evaluating the same
form with `C-x C-e`.

### Phase 2 — list library and predicates

Add the `while`-based `reverse`, `internal--append2`, `append`, `length`,
`nthcdr`, `nth`, `last`, `mapcar`, `assoc`, `member` to the prelude.  Add
`type-of` and the `-p` predicates to `native_bindings[]` in `src/lisp.c`,
implemented over `FeGetType`/`type_names` (both public in `fe.h`; no
submodule change).  Register them next to the existing string natives and keep
the "Emacs name where Emacs has one" comment true.

*Test*: `test/test_lisp.c` cases for each list function including the empty
list and the miss cases (`(nth 9 '(a))` → `nil`, `(assoc 'z …)` → `nil`), and
a case asserting `(length …)` over a 500-element list does **not** blow the GC
stack — that is the regression guard against someone rewriting these
recursively.  One PTY case exercising `mapcar` + `insert` is enough at that
layer.

### Phase 3 — binding forms

Add `internal--bind-name`, `internal--bind-value`, elisp `let` (parallel, via
immediate lambda application), `let*` (sequential, via `progn` of `fe-let`).
`let` must be defined *after* `mapcar`.

*Test*: `test/test_lisp.c` — `(let ((y 1) (z y)) z)` with an outer `y` of 100
must yield 100 (parallel), and the `let*` twin must yield 1; nested `let`
shadowing; `let` body containing `while` and `setq` of an outer variable;
bare-symbol and value-less bindings (`(let (a (b))…)` → both `nil`).

### Phase 4 — definition forms

Add `internal--arglist` (strips `&optional`, folds `&rest` into a dotted
tail), `internal--interactive-p`, `internal--has-interactive`,
`internal--strip-interactive`, then `defun`, `defmacro`, `defvar`,
`defconst`, and `(= interactive (macro args (list 'quote nil)))` so a stray
top-level `(interactive)` is inert.  `defun` returns `(quote name)` so
`C-x C-e` echoes the symbol the way Emacs does.

*Test*: `test/test_lisp.c` for arglist rewriting in isolation
(`(internal--arglist '(a &optional b &rest r))` → `(a b . r)`), docstring
returned when it is the only body form, `defmacro` with `&rest`.
`test/pty/lisp-defun-interactive.yaml`: an `init.fe` planted via
`config_files:` that defines an interactive `defun`, then `M-x` it and assert
the saved buffer.  This is the case that proves `define-command` accepts the
symbol.

### Phase 5 — quasiquote semantics (still zero fe changes)

Add `internal--qq` and `(= quasiquote (macro (form) (internal--qq form)))`.
At this point macros can be written as
`(quasiquote (if (unquote test) (progn (unquote-splicing body))))` — verbose,
but complete.  Ship this phase even if Phase 6 is deferred: it is the
prerequisite, and it makes Phase 6 a pure reader change.

*Test*: `test/test_lisp.c` for `(quasiquote (a (unquote b) c))`,
`(unquote-splicing …)`, `(quasiquote ())` → real `nil` (guards the nil trap),
and a user macro written with it.

### Phase 6 — the backquote reader (the only fe change)

In `fe/fe.c`, in `Read()`, immediately after the existing `case '\''`:

```c
case '`': {
  FeObject* v = FeRead(ctx, fn, udata);
  if (v == NULL) { FeHandleError(ctx, "stray '`'"); }
  return FeCons(ctx, FeMakeSymbol(ctx, "quasiquote"), FeCons(ctx, v, &nil));
}

case ',': {
  const char* name = "unquote";
  const char next = fn(ctx, udata);
  if (next == '@') { name = "unquote-splicing"; } else { ctx->nextchr = next; }
  FeObject* v = FeRead(ctx, fn, udata);
  if (v == NULL) { FeHandleError(ctx, "stray ','"); }
  return FeCons(ctx, FeMakeSymbol(ctx, name), FeCons(ctx, v, &nil));
}
```

and extend the symbol delimiter from `" \n\t\r();"` to `" \n\t\r();\`,"`.

Risk notes, all checked:

- **Rooting**: identical to the existing `'` handler.  `v` is already on the
  GC stack when `FeMakeSymbol` runs (every `Read` return path pushes), so the
  `FeCons` pair cannot be collected mid-construction.
- **`FeHandleError` longjmp**: the two new error sites are the same shape as
  `"stray '''"`; `ctx->nextchr` is reset by `FeHandleError` itself.
- **EOF after a comma**: `next == '\0'` stores `'\0'` in `nextchr`, which
  means "nothing pending", so the follow-up `FeRead` returns `NULL` and the
  `stray ','` error fires.  Verified.
- **Strings and comments**: `"` and `;` are matched earlier in the same
  switch, so commas and backticks inside them stay inert.  Verified.
- **Delimiter change**: only affects symbols that *contain* a backtick or
  comma, which nothing in kg or fe does.  Verified against fe's own
  `scripts/*.fe`: `macros.fe`, `reverse.fe`, `concatenate.fe`, `fib.fe`,
  `life.fe`, `mandelbrot.fe` produce byte-identical output before and after.
- **Step budget / macro copy semantics**: untouched — the change is purely in
  the reader.

This is a submodule bump.  Follow `doc/fe-upstream.md`: land it upstream on
`bjodah/fe` first (the remote is the owner's own fork, so this is a PR to
oneself, not a real fork of the language), bump the pin, re-review the whole
submodule diff, and rerun `.ci/run-ci-steps.sh` including
`.ci/ci-08-with-lisp-0.sh`.

*Test*: `test/test_lisp.c` reader cases for `` `(a ,b) ``, `` `(a ,@l) ``,
`` `() `` → real `nil`, `` '`(a ,b) `` → `(quasiquote (a (unquote b)))`, and a
string containing `` ` `` and `,@`.  One PTY case typing a backquoted macro
into a `.fe` buffer and evaluating it.

### Phase 7 — optional fe.c quality changes

Bundle with the Phase 6 submodule bump so there is only one pin move:

- `void-function NAME`: replace the non-callable `FeHandleError` in
  `Evaluate` with an 11-line block that formats the callee symbol's name via
  `FeToString` when `FeGetType(CAR(obj)) == FeTSymbol`.  Verified; the only
  behaviour change in fe's own script suite is the improved message.
- `GcStackSize` 512 → 4096: raises usable recursion depth from ~70 to ~550.
  Verified.  Note this raises `FeMinimumArenaSize` by 28 KiB; kg's 1 MiB
  default is unaffected but `KG_LISP_ARENA_SIZE` overrides below ~64 KiB
  would start failing `FeOpenContext`.  Add an assertion or a comment at
  `lisp_arena_size`.

### Phase 8 — `=` as numeric comparison (separate release)

Only after every internal `(= x v)` is `setq`.  Append, as the very last line
of the prelude, `(setq = (lambda (a b) (is a b)))`.  Nothing may use `=` for
assignment after that point — including any macro *expansion*, which is why
Phases 1–7 must emit `'setq`, never `'=`.

*Test*: `test/test_lisp.c` asserting `(= 2 2)` → `t`, `(= 2 3)` → `nil`, and
that `setq` still assigns.  Grep gate in CI is not worth building; a comment
at the top of the prelude is.

### Documentation that must move with the work

- `README.md` — the Lisp section's examples (currently `(= f (fn …))`).
- `doc/kg.1` — same examples plus any `define-command` / `global-set-key`
  prose; the man page is authoritative per `CLAUDE.md`.
- `src/help.c` — only if a keybinding changes; none of this changes bindings.
- `doc/fe-upstream.md` — Phase 0 pin correction, and the Phase 6 bump.
- `doc/TODO.md` — retire whatever "Lisp ergonomics" entries this closes.
- A new "kg Lisp vs Emacs Lisp" divergence list.  The honest ones are:
  `equal` is not structural on lists; `defvar` cannot tell unbound from `nil`;
  no `unwind-protect`/`condition-case`; recursion depth is ~70 (or ~550 with
  Phase 7); `t` is assignable; `?a` and `#'x` read as ordinary symbols and
  evaluate to `nil`; numbers are all doubles.

## 4. Backward compatibility and migration

The owner does not care about external users, so this is purely internal
churn.  Sized against the current tree:

| Surface | Size | What changes |
| --- | --- | --- |
| `src/lisp.c` `lisp_prelude[]` | 25 → ~135 lines | rewritten; `cond`/`when`/`unless`/`dolist`/`thing-at-point` bodies must emit `fe-if`/`progn`/`fe-let` |
| `src/lisp.c` `native_bindings[]` | +6 entries | `type-of` and predicates |
| `test/test_lisp.c` | 1091 lines, 37 lines containing Lisp snippets | mechanical `(= x (fn …))` → `(defun x …)`; plus ~15 new cases |
| `test/pty/*.yaml` | 8 of 29 Lisp cases embed Lisp source | mechanical rewrite of the embedded source and of `expected_saved` where a printed value changes |
| `README.md`, `doc/kg.1` | 11 lines mention Lisp forms | rewrite examples |
| `fe/` submodule | 24 inserted lines (+13 more with Phase 7) | Phase 6 only |

Two hazards during migration, both of which produce **silent** wrongness
rather than an error, so convert wholesale rather than incrementally:

1. Any surviving `(if c a b1 b2)` written against fe's elif semantics changes
   meaning the moment Phase 1 lands.  Grep the tree for multi-armed `if`
   before merging Phase 1.
2. Any surviving `(= x v)` changes meaning the moment Phase 8 lands.  Phase 8
   is deliberately last and separate for exactly this reason.

Estimated total: Phases 1–5 and 7's kg-side work is 1–2 days including tests;
Phase 6 adds a submodule round-trip; Phase 8 is an afternoon plus the grep.

## 5. Weighing the divergence

The submodule pin exists so that kg's Lisp core stays reviewable and
upstream-trackable.  A prelude change costs nothing on that axis — it is kg
source, in kg's repo, reviewed with kg's diff.  That is why §1 pushes so hard
toward the prelude even where a C change would be shorter.

The backquote reader is the one place the trade is worth taking, and it is
worth taking *cheaply*: 24 lines, one function, no new object types, no GC or
budget interaction, and the semantics live in kg's prelude where they can be
changed without another bump.  Crucially, `fe/`'s remote is the owner's own
fork, so "forking the language" here means opening a PR against a repo the
owner controls — the pin can move forward rather than sideways.  If that
upstream round-trip is unattractive, Phase 5 alone still delivers working
quasiquote; only the punctuation is missing.

## 6. Appendix — what was actually run

Harness: `scratchpad/harness.c`, ~120 lines, links only `fe.c`; 1 MiB
`aligned_alloc` arena, `FeSetErrorFn` → `longjmp`, `FeEvaluateStringWithOptions`
with `step_limit = 1<<20`, `poll_interval = 256`; `FE_ARENA` / `FE_STEPS`
environment overrides for the measurements.  Three builds: `harness-base`
(pristine `fe.c`), `harness-qq` (reader change only), `harness-all` (reader +
`void-function` + `GcStackSize`).

**Q1 — `lambda`.**

```
(= lambda fn)
(print (lambda (x) (* x 2)))      ; => (fn (x) (* x 2))
(print ((lambda (x) (* x 2)) 21)) ; => 42
(= adder (lambda (n) (lambda (x) (+ x n))))
(print ((adder 10) 5))            ; => 15
(print ((lambda (a . rest) rest) 1 2 3)) ; => (2 3)
(print fn lambda)                 ; => [primitive] [primitive]
```

Closures, dotted rest args and printing all behave identically.  The only
visible seam is `FeWrite` hardcoding `"fn"`.

**Q2 — `defun`.**

```
(= defun (macro (name params . body)
  (list '= name (cons 'fn (cons params body)))))
(defun square (x) (* x x))          ; => (square 7) = 49
(defun fact (n) (if (<= n 1) 1 (* n (fact (- n 1)))))  ; => (fact 10) = 3628800
(defun documented (x) "doc." (+ x 1))                  ; => 2
(defun onlydoc (x) "just a doc")                       ; => "just a doc"
```

Three lines.  Docstrings are inert when followed by a body and returned when
they are the body — the same as Emacs.

**The macro copy trap.**

```
(= badmac  (macro () nil))
(= goodmac (macro () (list 'quote nil)))
badmac  -> prints nil, (is x nil)=nil, (not x)=nil, (if x "T" "f")="T"
goodmac -> prints nil, (is x nil)=t,   (not x)=t,   (if x "T" "f")="f"
```

Symbols, numbers and strings survive the copy (`(is (symmac) t)` → `t`,
because the copied symbol shares the interned symbol's value cell).  Only
`nil` is poisoned.

**Call sites are rewritten in place.**

```
(= counter 0)
(= noisy (macro (x) (= counter (+ counter 1)) (list '+ x 1)))
(= f (fn (y) (noisy y)))
(print (f 1) (f 2) (f 3))   ; => 2 3 4
(print counter)             ; => 1
(print f)                   ; => (fn (y) (+ y 1))
```

The surprise here is how *useful* this is — expansion is free after the first
call — and how dangerous: any macro that reads runtime state expands once and
keeps the stale answer forever.

**Recursion depth.**  `(deep n)` = `(if (<= n 0) 0 (+ 1 (deep (- n 1))))`:

| depth | 512-slot GC stack | 4096-slot GC stack |
| --- | --- | --- |
| 50 / 60 / 70 | OK | OK |
| 80 / 100 / 150 | `GC stack overflow` | OK |
| 300 / 500 | — | OK |
| 600 / 700 | — | `GC stack overflow` |

This was the biggest surprise of the investigation.  ~70 frames is far lower
than the C stack would suggest (`ulimit -s` was 8192 KiB), and it is why every
list function in the plan is a `while` loop.  Expression nesting is *not*
affected: a `cond` with 200 clauses, falling through all of them, works fine
on the stock 512-slot build.

**`if` is an elif chain, not elisp's `if`.**

```
(if nil (print "then") (print "else1") (print "else2"))
```
prints only `else1` — it is evaluated as a *condition*, returns `nil`, and
`else2` is then consumed as the next condition with no consequent.  The macro
`(= if (macro (test then . else) (list 'fe-if test then (cons 'progn else))))`
restores elisp semantics; retested, both `else1` and `else2` print, and
`(if nil 1)` returns a real `nil` (`(is … nil)` → `t`).

**Argument binding is already elisp-friendly.**

```
((fn (a b c) (list a b c)) 1)        ; => (1 nil nil)   -- missing args are nil
((fn (a b c) (list a b c)) 1 2 3 4 5) ; => (1 2 3)      -- extras dropped, no error
```

so `&optional` needs no runtime support at all, only arglist rewriting:
`(internal--arglist '(a &optional b &rest r))` → `(a b . r)`, verified, and
`(is '&rest '&rest)` → `t` (interned symbols compare by pointer).

**`let`, both flavours.**  With an outer `y` of 100,
`(let ((y 1) (z y)) z)` → `100` (parallel, via `((fn (y z) …) 1 y)`) and
`(let* ((y 1) (z y)) z)` → `1`.  Nested shadowing, `while`+`setq` inside a
`let` body, and closures created inside a `let` all behave.  A naive
`do`+`fe-let` expansion of `let` gives `let*` semantics — that was the first
version I wrote, and it silently disagreed with Emacs.

**Renaming primitives, including `=`.**

```
(= setq =) (= progn do) (= null not)
(setq p 1 q 2)  ; => p=1 q=nil   -- fe's `=` silently ignores extra pairs
(setq = (lambda (a b) (is a b)))
(= 2 2) ; => t     (= 2 3) ; => nil     (setq r 5) ; => still assigns
```

The multi-pair `setq` result is the reason `setq` cannot be a bare alias.

**Quasiquote without touching fe.**

```
(quasiquote (a (unquote b) c))          ; b=42       => (a 42 c)
(quasiquote (a (unquote-splicing lst) c)) ; lst=(7 8) => (a 7 8 c)
(quasiquote ())                         ; => real nil
```
and a `when` macro written with it fires and short-circuits correctly.

**Quasiquote with the reader change** (`harness-qq`): `` `(a ,b c) `` →
`(a 42 c)`, `` `(a ,@lst c) `` → `(a 7 8 c)`, `` `(nested (deep ,b)) `` →
`(nested (deep 42))`, `` '`(a ,b) `` → `(quasiquote (a (unquote b)))`,
`(print "a, b \`c\` ,@d")` unchanged.  `(= defun (macro (name params . body)
`(= ,name (fn ,params ,@body))))` then works verbatim.

**Reader footguns on stock fe.**  `#'car` → symbol `#'car` → `nil`, silently.
`?a` → symbol `?a` → `nil`, silently.  `` `(a b) `` → two forms, symbol `` ` ``
then `(a b)`, so `(print `(a b))` dies with "tried to call non-callable
value".  `,x` → symbol `,x` → `nil`, silently.  `'(a . b)`, `"\\"`, `"\""`,
`-1.5e3` and case-sensitive symbols all behave.

**`(interactive)` and `define-command`.**  A `defun` macro that scans for
`(interactive)`, strips it, and emits `(define-command 'name fn)` registered
the command and the body ran.  A harness native mirroring kg's
`copy_command_name` confirms symbols are accepted:

```
(name-of 'my-command) -> [my-command] len=10 type=symbol
(name-of "my-command") -> [my-command] len=10 type=string
```

so `(defun my-cmd () (interactive) …)` and `(global-set-key "C-c d" 'my-cmd)`
both work with **no C change** to kg.

**Type introspection.**  `type_names[FeGetType(o)]` over the public API gives
`symbol string double pair nil primitive fn macro` for
`'a "s" 1 '(1) nil car (lambda (x) x) cond` — enough for every predicate
elisp users reach for, none of which the prelude can express on its own.

**Docstring registry.**  A `defun` variant that conses `(name . doc)` onto a
`docstrings` alist and a `documentation` lookup returned `"Bar doc."`.
Prelude-only.

**`t` is assignable.**  `(setq t nil)` succeeds and `(fe-if t "TRUE" "FALSE")`
then returns `"FALSE"`.  Pre-existing, not caused by anything here, but it
belongs in the divergence list.

**Cost measurements** (135-line candidate prelude plus a realistic
elisp-shaped `init.fe` using `defvar`, two `defun`s with docstrings /
`&optional` / `(interactive)`, a `defmacro` with `` ` ``/`,`/`,@`, `let`,
`while`, `dolist`, `cond` and `global-set-key`):

| Measurement | Result | kg's setting |
| --- | --- | --- |
| min arena, stock `GcStackSize` | ~40 KiB (32 KiB fails) | 1 MiB |
| min arena, `GcStackSize=4096` | ~64 KiB | 1 MiB |
| min arena, *current* kg prelude | ~12 KiB | 1 MiB |
| steps to evaluate the prelude | 100–200 | 1 048 576 (own budget) |
| steps for prelude + full init | ~6–7 k (5 000 fails, 8 000 OK) | 1 048 576 |

**Regression check of the patched fe** against fe's own `scripts/` with
`scripts/assert.fe` preloaded: `macros`, `fib`, `reverse`, `concatenate`,
`life`, `mandelbrot` byte-identical to the pristine build; `func` and `math`
differ only in the improved error text (`void-function %` /
`void-function abs` instead of `tried to call non-callable value`) — those two
scripts depend on `fex` extensions kg does not compile.

**Not tested.**  Everything above ran against the harness, not against kg's
binary — another agent held `/work`, so no `make`, no `make check`, and no PTY
case was executed.  Specifically unverified end-to-end: that `C-x C-e` echoes
`defun`'s symbol return value the way this plan assumes; that the enlarged
prelude does not trip `.ci/ci-08-with-lisp-0.sh` (it should not — the prelude
is inside `KG_USE_LISP`); and the actual edit-and-rerun churn numbers in §4,
which are grep counts rather than completed conversions.
