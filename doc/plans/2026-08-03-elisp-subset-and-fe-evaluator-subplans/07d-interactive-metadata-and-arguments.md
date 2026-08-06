# 07D — Interactive metadata and non-prompting arguments

Parent: [Phase 7](../2026-08-03-elisp-subset-and-fe-evaluator.md#11-phase-7--strict-arity-and-interactive-command-arguments),
kg-only, no pin move. `(interactive …)` becomes real command metadata and
drives argument construction for the no-argument spec, `p`, `P`, `r`, and
`(interactive FORM)`. Prompting codes are 07E.

**Prerequisite:** [07C](07c-the-strict-pin.md). Strictness must already be
live so argument delivery is tested against the real call contract.

## Outcome

This is a valid kg command and receives one or five, rather than relying on
lax nil binding:

```lisp
(defun step (n)
  (interactive "p")
  (goto-char (+ (point) n)))
```

Plain `M-x step` advances one codepoint and `C-u 5 M-x step` advances five.
`current-prefix-arg` is a value binding with Emacs' raw forms, while
`prefix-numeric-value` is a function. A command whose spec supplies too few
arguments raises the ordinary strict arity condition; there is no nil
padding. `(command-execute 'step)` uses the same metadata and evaluator.

## Declaration and metadata

1. **Recognise the declaration in the Emacs position.** In the `defun`
   macro, split an optional leading docstring, then inspect only the next
   body form. It is a declaration only when it is `(interactive …)` there.
   Remove that one declaration from the function body. A later
   `(interactive …)` remains an ordinary, nil-valued body form and does not
   register a command (07A I10). If a docstring plus declaration leaves no
   executable body, append nil: `(defun f () "Doc" (interactive))` returns
   nil when called, not the docstring exposed by Fe's docstring-only rule.
2. **Preserve FORM's lexical environment.** The descriptor is the first
   argument of the declaration, or nil when omitted. Nil and string specs
   are stored directly. For FORM, the macro creates a zero-argument thunk
   `(lambda () FORM)` in the same lexical environment as the command
   closure; creating the thunk does not evaluate FORM. Store an explicit
   descriptor kind so a function-valued thunk is never confused with a
   string. This matches 07A I15; evaluating detached raw syntax globally
   would not. Additional `interactive` MODES are deliberately ignored and
   documented as a divergence.
3. **Make redefinition truthful and single-evaluation.** Evaluate the new
   closure once. For an interactive definition, create/validate all command
   roots from that object first, then write the function cell through the
   already-validated, non-allocating `defalias` path; a root failure leaves
   both the old command and old function cell intact. Interactive →
   interactive replaces function/spec/doc together; interactive →
   noninteractive writes the new function then unregisters via a path that
   cannot fail; noninteractive → interactive registers it. Keep public
   `(remove-command NAME)`'s missing-name error; use an internal
   no-op-if-absent synchronisation path for `defun` rather than weakening
   that API. The command root and function cell must hold the same closure,
   never two evaluations that merely behave alike.
4. **Root everything that outlives evaluation.** `struct lisp_command`
   contains the bounded name plus separate roots for the function,
   interactive string/thunk, and optional docstring, plus the descriptor
   kind. Create all replacement
   roots before releasing old ones; `remove-command`, shutdown, failed init,
   and redefinition release each exactly once. The 32-command bound and
   runtime command identity rules stay unchanged.
5. **Extend the kg-owned registration API explicitly.** `(define-command
   NAME FUNCTION &optional INTERACTIVE-SPEC DOCUMENTATION)` remains usable
   with two arguments (empty spec, no documentation). Its optional spec is
   nil, a string, or a zero-argument function that returns the argument
   list; this invented API cannot accept detached raw FORM syntax and guess
   its lexical environment. Validate documentation as nil/string. `defun`
   supplies the thunk automatically.

The docstring is stored because the parent contract requires optional
documentation, but this slice adds no `documentation`/`interactive-form`
reflection API. Raw FORM syntax is intentionally not stored beside the
thunk, so honest `interactive-form` reflection needs a later metadata
decision. Existing `describe-command` may continue to say “Defined in Lisp”;
exposing full Lisp docstrings is a separately evidenced UI change.

## Prefix representation and scope

`struct command_prefix` stays free of Fe types. Extend it with a small raw
kind while retaining the effective bounded integer used by built-ins:

- no prefix: raw nil, effective 1;
- Meta/C-u digits: raw integer, including negative integers;
- one or more bare `C-u`: raw `(4)`, `(16)`, … with the existing effective
  integer and 1000 cap; and
- bare `M--`: raw symbol `-`, effective −1.

Move the collapse of bare `C-u` out of capture and into consumers. Add `M--`
and `M-- DIGITS` to `handle_universal_arg`; preserve every existing built-in
consumer by continuing to expose `supplied` plus the effective `value`.
Prefix-map dispatch must copy the complete struct, not only the numeric
part.

`kg_lisp_run_command` accepts the command prefix from `cmd_invoke`. Before
building arguments it binds the global value cell `current-prefix-arg` to a
fresh Fe representation of the raw form, then restores the previously
rooted value on success, error, quit, and budget exits. Define the variable
to nil at Lisp initialisation so it is never accidentally void. This is one
bounded command-boundary binding, not a general dynamic-binding feature.
Reuse that same rooted object for `P` (so it is `eq` to
`current-prefix-arg` during the body); never mutate it, and build a fresh
universal list for the next command.
Because Fe has no special variables, a lexical binding named
`current-prefix-arg` shadows this global command-boundary value. Document
that difference from Emacs rather than implying general dynamic scope.
Read the prior global through Fe's existing `symbol-value` callable and root
it; do not assume it is nil, because user Lisp may assign the global.
Top-level restoration belongs in both recovery branches. Nested
`command-execute` registers a `FeProtectWithCleanup` record backed by stable
state (never a soon-to-vanish C stack object), so a completion that jumps
past the native still restores the value and releases its root; its cleanup
performs no Fe allocation.

`prefix-numeric-value` is a strict one-argument native implementing the I1
table: nil→1, integer→itself, `(N)`→N, and `-`→−1; malformed raw forms raise
`wrong-type-argument`. The command-boundary representation and this native
share one conversion helper so `p` cannot disagree with Lisp code.

## Argument construction

Build arguments immediately before the call, under the command's existing
GC checkpoint:

1. Nil or `""` supplies zero arguments. Any other string is split on
   newlines; each clause must be nonempty, starts with one ASCII code byte,
   and uses the remainder as prompt text. An interior/trailing empty clause
   is an invalid spec, not silently skipped. This slice recognises `p` (one converted prefix),
   `P` (one raw prefix), and `r` (two sorted 1-based Lisp positions). `r`
   permits an empty region and reuses the region native's mark-not-set error.
2. A FORM descriptor is a rooted zero-argument thunk. Call it at invocation,
   after `current-prefix-arg` is bound and before the body; require a proper
   list (`wrong-type-argument listp VALUE` otherwise) and copy its elements
   into the call vector without evaluating them again. Its ordinary error,
   quit, or budget completion propagates unchanged. Improper and
   cyclic/overlong lists fail clearly.
3. Use a documented `LISP_INTERACTIVE_MAX_ARGS` bound of 16 for both string
   and FORM specs. `r` consumes two slots. Root every constructed value and
   every list element until `FeCallWithOptions` has taken the vector; restore
   the checkpoint on all exits. The bound is a recorded kg divergence, not a
   silent truncation.
4. A valid-but-deferred Emacs code/modifier reports “unsupported interactive
   code CODE”; a byte outside 07A's measured set reports “invalid interactive
   code CODE”. In both cases the command body does not run. Prompt text for
   `p`/`P`/`r` is parsed but unused, as in Emacs.
5. Pass the exact constructed argc to `FeCallWithOptions`. Never pad missing
   parameters with nil; strict arity owns that diagnostic.

## Invocation and nested commands

The shared Lisp command descriptor gains `CMD_LISP_CALLABLE` and retains
`CMD_EDITS_BUFFER`; it does **not** gain `CMD_REPEATS`. `cmd_invoke` remains
the only policy/identity route.

There are two execution paths behind `kg_lisp_run_command`:

- from a key or M-x, establish the existing top-level recovery frame, bind
  the raw prefix, build arguments, and call; and
- from `(command-execute …)` while Lisp is already active, build and call
  inside that evaluator instead of returning the current “Lisp is busy”
  pseudo-success. Errors and quit propagate to the surrounding
  `condition-case`; the Fe native-reentry limit bounds recursion.

`cmd_invoke` publishes a scoped active command context for this purpose.
`native_command` inherits that context's prefix when one exists and uses
none otherwise, matching `command-execute`'s default prefix inheritance.
It must not read `editor.current_prefix` unconditionally: outside dispatch
that field may still contain a stale keystroke value, which is the bug the
current explicit-empty code prevents. Save/restore of the active context,
`editor.current_prefix`, command identity, and the Lisp value binding is
tested under nested success and failure.

## Tests owned by this slice

- `test/test_lisp.c`: declaration placement after/no docstring, including a
  docstring+declaration+empty body returning nil; later-body
  non-declarations; all three redefinition directions; single closure
  identity and failure-atomic registration; registration root
  replacement/removal/GC; nil/empty/multiline specs; `p`/`P`/`r`; every I12
  raw form, `P` identity with `current-prefix-arg`, and per-command list
  freshness; FORM evaluation once, lexical capture, proper-list validation
  and the 16-argument
  boundary; no-padding arity; restoration of `current-prefix-arg` after
  normal/error/quit/budget exits; nested `command-execute` success and a
  catchable failure.
- PTY (prefix and region need the real editor): plain and `C-u 5 M-x step`;
  `C-u` versus `C-u 4` observed through `P`; `C-u C-u`, bare `M--`, and
  `M-- 5`; an `r` command over active and empty regions; no-mark refusal;
  redefinition from interactive to noninteractive disappearing from M-x.
- kg compat manifest: retire the `prelude-interactive` placeholder with a
  rationale naming its replacement; flip 07A's planned
  declaration-placement, redefinition, `p`/`P`/`r`/FORM,
  `current-prefix-arg`, and nested `command-execute` rows to `supported`.
  Regenerate no 07A snapshot.

## Documentation

Rewrite the interactive sections in `README.md` and `doc/lisp-api.md`: the
old strip story becomes the declaration/argument story, with the supported,
deferred, and invalid code distinction; document the 16-argument bound,
temporary `current-prefix-arg` binding, prefix forms, and kg-owned
`define-command` extension. `doc/kg.1` gains prefix delivery to Lisp.
`doc/TODO.md` carries only the specific 07E prompting work and recorded
deferred codes/modifiers/reflection; no generic “interactive unsupported”
entry remains.

## Gates

Run both complexity commands at slice start and end. Then `make check` from
an idle tree, `make WITH_LISP=0 clean all check`, `header-check`,
`docs-check`, `lisp-compat-check`, `lisp-prelude-check`, oracle
verify-by-running, and the full parallel runner. This is the larger kg slice,
priced **+80..120 scc** against 07A's funded 5660 cap; record the actual and
split helpers before raising the per-file/new-function gates.

## What this does not do

- No prompting codes (`n N s f F b B`) or minibuffer calls — 07E.
- No valid deferred codes/modifiers, prompt `%` interpolation,
  `interactive` MODES filtering, keyboard-macro strings, `CMD_REPEATS`, or
  reflection API.
- No change to `after-change-functions`' four-argument shape.
- No new Fe API or implementation. If the builder appears to require one,
  stop and review the design before making an unplanned submodule commit.
