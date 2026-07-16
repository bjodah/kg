# Plan: Lisp exponentiation (`expt`) and `C-x C-e`

## 1. Problem Description
The PTY test `lisp-exponentiation.yaml` fails.  It evaluates `(expt 2 8)` from
the buffer via `C-e M-1 C-x C-e` and expects the integer result `256` to be
appended to the line:

```
initial:  (expt 2 8)
keys:     C-e, M-1, C-x, C-e
expected: (expt 2 8)256
```

Two independent gaps cause the failure:

1. **`expt` is undefined in the Lisp environment.**  `kg` compiles only
   `fe/fe.c` (see `doc/fe-upstream.md`); the `fex*` math extensions — which
   provide `pow` — are deliberately excluded.  Core Fe has only `+ - * /` and
   the comparison predicates, so `(expt 2 8)` raises "unknown function expt".
   The Emacs name for exponentiation is `expt`; Fe's `pow` returns a `double`
   but Fe writes whole-valued doubles without a decimal point, so `pow(2,8)`
   → `256.0` prints as `256`.

2. **`C-x C-e` is not bound.**  Emacs binds `eval-last-sexp` to `C-x C-e`.
   The `eval-last-sexp` and `eval-print-last-sexp` named commands already
   exist in `src/cmd.c` (`do_eval_last_sexp`), and `C-j` already routes to
   `eval-print-last-sexp` in Lisp Interaction mode.  But the `C-x` prefix
   switch in `src/kbd.c` has no `case CTRL_E`.

3. **Prefix argument does not survive the `C-x` prefix.**  The numeric prefix
   (`M-1`) is consumed in `editor_process_keypress` at the moment `C-x` itself
   is typed, and the resulting `n` is discarded by `case CTRL_X`.  By the time
   the second key (`C-e`) arrives, no prefix is available.  Emacs uses the
   prefix to select the "print into buffer" variant: with any prefix arg,
   `eval-last-sexp` inserts the value; without one it shows it in the echo
   area.

## 2. Design

### 2.1 `expt` native function (in `src/lisp.c`)
Add an editor-owned native next to the other `kg-*` natives so the Fe
submodule stays untouched and unmodified (no submodule bump, no `fex_math`
link).  `pow()` is already available because the build links `-lm`
(`Makefile` adds `LDLIBS += -lm`).

```c
#include <math.h>   /* add to the KG_USE_LISP includes */

static FeObject *native_expt(FeContext *context, FeObject *arguments)
{
	FeDouble base = FeToDouble(context, FeGetNextArgument(context, &arguments));
	FeDouble exp  = FeToDouble(context, FeGetNextArgument(context, &arguments));
	FeRequireNoArguments(context, arguments);
	return FeMakeDouble(context, pow(base, exp));
}
```
Register it alongside the other natives:
```c
FeDefineNative(context, "expt", native_expt);
```
Fe prints `256.0` as `256` (`FeWrite` uses `%" PRId64` when `floor(d)==d`),
which satisfies the expected saved text.  Negative/zero/fractional exponents
work by the usual `pow` semantics; this matches Emacs `expt` for the numeric
cases kg exercises.

(Alternative considered and rejected: adding `expt` to `fe/fe.c` core.  That
would require bumping the pinned submodule, updating `doc/fe-upstream.md`,
and running Fe's own CI.  Per `doc/fe-upstream.md`, kg embeds Fe core only and
adds editor natives in `src/lisp.c`; keeping `expt` as an editor native matches
that contract and needs no submodule change.)

### 2.2 Carry the prefix argument through `C-x`
Add one field to `editor_config` in `src/def.h`:
```c
int cx_prefix_arg; /* 0 = no prefix supplied; >0 = prefix carried into C-x */
```
In `editor_process_keypress` (`src/kbd.c`), the prefix is currently captured
into local `n` at the "consume the pending C-u count" block.  Capture the
*raw* value too so `C-x` can forward it:

```c
int prefix = editor.prefix_arg;   /* 0 when none was supplied */
if (prefix > 0) {
	editor.prefix_arg = 0;
	editor_set_status_message("");
	n = prefix;
} else {
	n = 1;
}
```
Then in `case CTRL_X:` store it before turning on the prefix:
```c
case CTRL_X: /* C-x prefix */
	editor.cx_prefix = 1;
	editor.cx_prefix_arg = prefix;   /* 0 = no prefix for the subcommand */
	editor_set_status_message("C-x-");
	return;
```
Because `CTRL_X` always assigns `cx_prefix_arg`, no per-buffer reset is
needed (the subcommand reads it on the very next keystroke).  Initialise it to
`0` in `src/main.c` next to the other prefix fields for cleanliness.

### 2.3 Bind `C-x C-e`
In the `C-x` prefix switch (`src/kbd.c`), add:
```c
case CTRL_E: /* C-x C-e: eval-last-sexp (insert result when prefixed) */
	do_eval_last_sexp(editor.cx_prefix_arg > 0);
	break;
```
`do_eval_last_sexp` already takes a `print_to_buffer` flag and handles undo,
insertion, and read-only rejection.  `M-1` supplies `prefix == 1 > 0`, so the
result `256` is inserted; with no prefix it shows in the status area — the
Emacs behaviour.  `do_eval_last_sexp` is currently `static` in `src/cmd.c`;
move its declaration into `src/lisp.h` (it already lives conceptually with
the eval commands) or expose a small `cmd_eval_last_sexp(int print_to_buffer)`
helper that `kbd.c` can call.  Prefer the minimal route: add a non-static
wrapper `void cmd_eval_last_sexp(int print_to_buffer)` in `cmd.c` that calls
`do_eval_last_sexp(print_to_buffer)`, and prototype it in `lisp.h` (which
`kbd.c` already includes).

### 2.4 Documentation
- `doc/kg.1`: under the `C-x` bindings table, add `C-x C-e` → "eval-last-sexp;
  with a prefix arg, insert the result into the buffer".
- `src/help.c`: the built-in help table does not list `C-x C-e`; keep parity
  with the man page (add a one-line entry alongside the existing `C-x` rows if
  space allows; otherwise at least ensure the man page is authoritative).
  Note: the help cheat-sheet rows are fixed-width; only add if it fits.

## 3. Verification
1. `make` builds cleanly with the default Lisp configuration.
2. `python3 utils/pty_accept.py --kg src/kg \
   test/pty/lisp-exponentiation.yaml` → PASS (saved file is
   `(expt 2 8)256`).
3. `make WITH_LISP=0` still builds (the new code is under `#ifdef KG_USE_LISP`
   where appropriate; the `C-x C-e` binding in `kbd.c` must still compile in
   the no-Lisp build — guard the call so a no-Lisp build reports "Lisp not
   available" or simply does nothing, matching how `C-j` is guarded with
   `kg_lisp_active()`).
4. Existing Lisp eval PTY cases (`lisp-eval-c-j*.yaml`) stay green.
5. `make check` overall is green.

## 4. Risks / Notes
- `pow` on very large integer exponents yields a `double` and may lose
  precision beyond 2^53; that is acceptable for kg's Lisp scratch use and
  matches Emacs `expt` only for small results.  Do not attempt to implement
  arbitrary-precision integer exponentiation (Fe's TODO mentions GMP).
- Keep `expt` as an editor native, not in `fe/fe.c`, to avoid a submodule bump.
- The `C-x C-e` guard for the no-Lisp build: mirror the `C-j` guard
  (`if (kg_lisp_active())`) so the disabled-Lisp CI stage
  (`.ci/ci-08-with-lisp-0.sh`) stays green.