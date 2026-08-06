# 07E — Prompting codes and the phase close

Parent: [Phase 7](../2026-08-03-elisp-subset-and-fe-evaluator.md#11-phase-7--strict-arity-and-interactive-command-arguments),
kg-only, no pin move; closes the phase. This is the first Lisp→minibuffer
seam and adds `n`/`N`, `s`, `f`/`F`, and `b`/`B` to 07D's argument builder.

**Prerequisite:** [07D](07d-interactive-metadata-and-arguments.md); its
metadata, prefix model, bounded vector, nested invocation, and non-prompting
codes must be green.

## Outcome

This command prompts and receives the selected path without opening it as a
side effect:

```lisp
(defun insert-file-name (file)
  (interactive "FFile: ")
  (insert file))
```

Cancelling any prompt with `C-g` aborts before the command body and produces
a quit completion (`Quit` at the top-level command boundary). Numeric input
accepts Fe integers or floats without evaluating Lisp, rejects trailing
junk, and re-prompts. Buffer/file argument reads do not switch buffers or
visit files while constructing arguments.

## One prompt seam and one result contract

The argument builder owns one helper that takes the code, literal prompt,
fd, prefix, and already-built arguments, and returns one of: Lisp value,
cancelled, overflow, or validation failure. It delegates UI to existing
minibuffer/picker code but owns the Lisp conversion and completion policy.

All prompt implementations use `enum minibuf_result` end-to-end:

- `MINIBUF_ACCEPTED` is the only success;
- `MINIBUF_CANCELLED` raises `FeCompletionQuit` with the existing
  cancellation prose, so a nested `command-execute` can be caught by a
  surrounding `(quit …)` handler and a top-level invocation reports `Quit`;
  and
- `MINIBUF_OVERFLOW` raises an ordinary error naming the prompt/code. A
  truncated prefix is never delivered as if it were the user's answer.

`editor_read_line_path` currently returns `int` and loses its `overflow`
counter on both Enter paths. Change its declaration/definition to return
`enum minibuf_result`, return `MINIBUF_OVERFLOW` when appropriate, and update
every existing caller to act only on `MINIBUF_ACCEPTED`. Add regression tests
for the old silently truncated acceptance; this is part of making the seam's
existing documented contract true, not optional cleanup.

## Code contracts

1. **`s` — literal string.** Start with an empty prefill and return exactly
   the accepted bytes, including an empty string. No history ring and no
   input-method distinction (kg has no input methods). Prompt text is the
   clause tail verbatim.
2. **`n` — always prompt.** Ignore any prefix. Use a small non-allocating
   decimal token classifier mirroring `fe.c`'s documented sign/digits/
   fraction/exponent grammar, then `strtoimax`/`strtod` and
   `FeMakeInteger`/`FeMakeDouble`; do not call `FeReadString` and then try to
   recover from its non-local syntax error. Require one token plus only
   trailing ASCII whitespace, and re-prompt on empty, a non-number, or junk.
   Integer overflow becomes a double as in Fe. The bignum difference is
   already a language divergence, and allocation failure after valid input
   remains a command error rather than a re-prompt.
3. **`N` — prefix or number.** When a prefix was supplied, return
   `prefix-numeric-value` without prompting; otherwise run exactly the `n`
   path. Pin both halves so `n`/`N` cannot be reversed later.
4. **`f` / `F` — path without visit.** Seed `editor_read_line_path` with
   `editor_prompt_prefill_dir`, accept its completion/literal policy, and
   expand `~` exactly once. `f` requires the resulting path to name an
   existing filesystem entry and re-prompts when it does not; `F` permits
   absence. Neither opens, creates,
   selects, or canonicalises with `realpath` (which cannot represent an
   absent `F` target). Relative names and `M-RET` retain kg's existing file
   picker policy and are recorded as such rather than called Emacs parity.
5. **`b` / `B` — name without selection.** Extract the buffer picker read
   from `buf_select_interactive` into a non-mutating helper: `b` accepts only
   an existing display name (re-prompting on a miss) and blank means the
   current buffer; `B` accepts a new typed name and blank defaults to the
   first “other buffer” candidate (current buffer when no other exists).
   For `B`, RET accepts typed text when there is no match and `M-RET` accepts
   it literally even when a completion exists, mirroring the path picker's
   explicit-literal escape. Only the eventual command body may switch/create
   a buffer.

The buffer-picker helper belongs to `bufmgr.c`; introduce a self-contained
`src/bufmgr.h` for its declaration instead of adding another module-owned
prototype to `def.h`. The existing `buf_select_interactive` becomes a thin
read-then-select caller of the same helper, preserving its picker ordering,
UTF-8 input, and cancellation behaviour.

Prompts are literal in this phase. Emacs passes a prompt containing `%`
through `format` with earlier interactive arguments; kg records prompt
interpolation as a divergence instead of treating status text as a format
string accidentally. Untrusted prompt/answer bytes continue through the
ordinary safe display path.

## Prompt context and re-entry

Prompting is allowed only while executing a command from a real key/M-x
command context with a live fd. A Lisp command reached by nested
`command-execute` during that command may prompt once the outer M-x/key
prompt has closed. Calls from init loading, eval-expression without a command
fd, hooks, process filters/sentinels, or while another minibuffer read is
active fail before reading input with a clear “interactive prompt is not
available here” error. Track this as explicit execution context, not by
guessing from `state.frame_active` (which is true in all those cases).

Balance `kg_event_prompt_enter`/`kg_event_prompt_leave` exactly once on every
accepted/cancelled/overflow/validation path. A prompt failure leaves the
already-built Fe arguments rooted until the builder's common unwind restores
its checkpoint, and the command body never runs.

## Tests owned by this slice

- PTY, primarily tmux-backed: each code end-to-end through a command that
  inserts its received value; `n` integer/float and junk re-prompt; `n`
  ignoring a prefix; both `N` branches; `f` refusing a missing entry; `F`
  accepting one; `b` blank/current and existing selection; `B` blank/other
  and a new typed name; cancellation for scalar, path, and buffer pickers;
  command body not run on cancel/overflow/error; editor remains usable.
- Native tests for the pure seams: numeric integer/float/exponent/overflow
  classification, full-input rejection, and no evaluation;
  `editor_read_line_path` overflow propagation and all updated
  callers rejecting it; the buffer picker returning a name without changing
  `buf_current`; execution-context refusal from init/hook/process callback;
  balanced prompt enter/leave on every result.
- kg compat manifest: add `n`/`N`, `s`, `f`/`F`, and `b`/`B` value rows with
  fresh snapshots where the batch oracle can compare the delivered value;
  use `kg-policy` rows for path-picker details, overflow, prompt literals,
  context refusal, and the no-other-buffer fallback. Never regenerate a 07A
  snapshot.

## Documentation and fixed scope

Finish the interactive-code table in `README.md`, `doc/lisp-api.md`, and
`doc/kg.1`; distinguish supported, valid-but-deferred, invalid, and kg-policy
behaviour. Document cancellation/overflow, the no-side-effect argument read,
numeric grammar, path resolution policy, literal prompts, and where prompting
is refused.

`read-string`, `read-number`, `read-file-name`, and `read-buffer` do **not**
land opportunistically. The internal seam does not by itself settle those
public functions' optional arguments, defaults, history, or non-command
re-entry semantics. Record them for Phase 8 corpus evidence.

## The phase close

Run both complexity commands at slice start and end and record 07D/07E
actuals against 07A's funded rows. Then Rule 9 in full: `make check` from an
idle tree, `make WITH_LISP=0 clean all check`, `header-check`, `docs-check`,
`lisp-compat-check`, `lisp-prelude-check`, oracle verify-by-running, and
`JOBS=8 .ci/run-ci-steps.sh --parallel` after regenerating
`compile_commands.json`. This slice is priced **+30..50 scc**; split helpers
before changing any funded cap.

The Status entry records per-slice actuals, what the plan got wrong, and the
carried work: deferred valid codes/modifiers and prompt interpolation,
general read functions, `interactive` MODES/reflection, the 16-argument
bound, `after-change-functions`, and review findings. Remove the five
sub-plan documents only after reviewer acceptance.

## What this does not do

- No completion framework or history rings beyond reusing the existing
  pickers; no `completing-read`.
- No `read-*`, `y-or-n-p`, or `yes-or-no-p` public Lisp functions.
- No async or recursive minibuffer. Unsupported contexts fail before input.
- No new Fe surface and no edits in the Fe submodule.
