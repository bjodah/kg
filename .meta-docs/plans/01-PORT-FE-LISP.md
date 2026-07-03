# Plan: embed Fe Lisp in kg

## Objective

Embed Fe as kg's optional extension language without weakening kg's stability,
terminal safety, or dependency-free distribution model. Lisp support is a
compile-time feature, enabled by default, that packagers and users can switch
off; a lisp-less build must behave exactly like today's kg. The first useful
release should provide safe expression evaluation, a startup file, explicit
package loading via `(kg-load ...)`, and a small editor bridge. Lisp-defined
interactive commands and key bindings follow only after the execution and
rooting APIs are proven.

This is an integration plan, not an instruction to start wiring Fe in
immediately. Complete the blocking items in Fe's
`.meta-docs/plans/01-PRE-KG-EMBEDDING-API.md` first, then pin the resulting Fe
commit in the `fe/` submodule.

## Decisions

- Move kg from C99 to strict C23 before importing Fe. Do not use `gnu11` as an
  intermediate compatibility mode.
- Consume Fe through the existing `fe/` git submodule pinned to an exact
  commit of `bjodah/fe`; do not copy a `vendor/fe/` snapshot. Release tarballs
  must embed the pinned Fe sources so a tarball build needs neither git nor
  network access.
- Lisp support is compile-time optional and enabled by default: `make
  WITH_LISP=0` must produce today's editor with no Fe objects linked. Guard
  integration points with one macro (`KG_USE_LISP`) and keep conditional code
  confined to `src/lisp.c`, command registration, and `main`.
- Extension packages load explicitly: `init.fe` is the only automatically
  evaluated file, and it pulls in packages with `(kg-load "name")` resolved
  against `$XDG_CONFIG_HOME/kg/lisp/`. No autoload directory scanning in the
  first release.
- Do not compile Fe's I/O, process, regex, math, or time extensions initially.
  In particular, do not accidentally expose filesystem or process execution
  through Fe's standard extensions.
- Treat init files and interactively evaluated expressions as trusted code, not
  as a sandbox. Resource bounds still protect the editor from accidental
  infinite loops and runaway allocation.
- Keep interpreter ownership behind `src/lisp.c`; do not spread `FeContext*`,
  `jmp_buf`, or GC-stack management through editor modules.
- Use kg's existing command and buffer operations rather than duplicating
  editing logic in Lisp wrappers.

## Non-goals for the first release

- A complete Emacs Lisp compatibility layer
- Async or multithreaded evaluation
- Loading arbitrary native modules
- Exposing raw `struct editor_config`, rows, windows, or buffer arrays to Lisp
- Making every C command automatically callable from Lisp
- Automatic package discovery, dependency resolution, or `require`/`provide`
  semantics; `(kg-load ...)` from `init.fe` is the entire loading story
- Persisting or serializing Fe heap state across kg processes

## Milestone 0: migrate kg to C23

Change every language-mode flag in `Makefile` (normal, fuzz, and coverage
builds) to `-std=c23`, or temporarily to the compiler's current spelling
(`-std=c2x`), while retaining the POSIX feature-test defines. Pin minimum GCC
and Clang versions in CI and document the supported compiler floor. Do this as
an isolated change before adding Fe.

Required verification:

- GCC and Clang warning-clean builds with warnings as errors
- `make check`, including all unit and PTY tests
- ASan/UBSan, MSan, GCC analyzer, Valgrind, IWYU, cppcheck, and clang-tidy stages
- `make fuzz-keypress-smoke`

Do not mix unrelated C23 modernization into this milestone. Its purpose is to
prove that the existing editor behaves identically under the new language
mode.

### C23 hardening opportunities after the mode switch

Treat these as small, reviewed follow-ups rather than prerequisites for linking
Fe. Prefer changes that let the compiler prove an invariant or remove custom
overflow logic; do not adopt new syntax merely because it is available.

- Use `<stdckdint.h>` checked arithmetic (`ckd_add`, `ckd_mul`) for sizes built
  before `malloc`, `calloc`, and `realloc`. Audit buffer growth in `buffer.c`,
  `display.c`, `fileio.c`, `shell.c`, `word.c`, and `yank.c` first. A failed
  size calculation must follow the same recoverable OOM path as allocation
  failure, never wrap to a smaller allocation.
- Replace suitable preprocessor constants with typed `constexpr` objects and
  use `static_assert` for relationships such as table counts, fixed capacities,
  key-code ranges, and assumptions shared by arrays and enums. Keep `#define`
  where conditional compilation genuinely requires it.
- Add standard attributes selectively: `[[nodiscard]]` on helpers whose error,
  allocation, or lookup result must be checked; `[[fallthrough]]` on deliberate
  switch fallthrough; `[[maybe_unused]]` in configuration-dependent code; and
  `[[noreturn]]` on fatal exits. Attributes should clarify an existing contract,
  not suppress a warning.
- Consider fixed-underlying-type enums for key codes, syntax classes, and undo
  tags where storage width or signedness is an actual invariant. Add range
  assertions at input boundaries; do not rely on a narrower enum as runtime
  validation.
- Use C23 `bool`, `true`, and `false` directly and `nullptr` for pointer nulls
  when the supported compiler floor implements them consistently. This makes
  intent and variadic/generic type checking clearer, but should be a mechanical
  change with no mixed-style churn.
- Use `typeof_unqual` and `_Generic` only for small, type-safe utility macros
  where they eliminate double evaluation or enforce an API contract. Prefer an
  ordinary inline function whenever it expresses the operation cleanly.

Do not depend initially on unevenly implemented C23 facilities such as
`#embed`, `_BitInt`, or the new library additions merely to reduce a few lines
of code. Add a compile probe and CI coverage before using any C23 feature whose
availability depends on the compiler or C library rather than the language
mode alone.

New code written for this plan is exempt from the "follow-up" framing: write
`src/lisp.c`, the bridge, and the package loader in C23 idiom from the start —
`bool`/`nullptr`, `[[nodiscard]]` on the eval and load entry points,
`constexpr` for the arena size, budget defaults, and load-depth limit, and
`<stdckdint.h>` for arena and buffer size math. Only retrofitting existing
editor code is deferred.

## Milestone 1: pin the Fe submodule and wire the optional build

Fe is already present as the `fe/` submodule; make it a reliable, pinned,
optional dependency:

- Fix the submodule URL. `.gitmodules` currently says `github.com:bjodah/fe`,
  which resolves only for users with a matching SSH host alias. Use
  `https://github.com/bjodah/fe.git`; keep SSH pushing as a local
  `url.<base>.insteadOf` remap.
- Pin the exact Fe commit that completes the blocking phases of Fe's embedding
  plan. Record commit, tracked branch, license/attribution, and the update
  procedure (bump submodule, review diff, rerun kg CI) in `doc/fe-upstream.md`
  or a README section.
- Compile only `fe/fe.c` — not `fex*.c`, `auto.c`, or `main.c` — from kg's
  Makefile with dedicated `FE_CFLAGS`, placing the object with kg's own. Do
  not glob the submodule. Keep `fe.h` out of `src/def.h`; only `src/lisp.c`
  and a narrow test module include it.
- Introduce `WITH_LISP` (default `1`). With `WITH_LISP=0`: no Fe objects, no
  `KG_USE_LISP` define, and a binary behaviorally identical to pre-Fe kg.
- With `WITH_LISP=1` and `fe/fe.c` absent (submodule not initialized), fail
  with an actionable message naming `git submodule update --init` and
  `WITH_LISP=0`, not a cryptic missing-file error.
- Teach `dist` to embed the pinned Fe sources and license so tarball builds
  work offline in both configurations; `clean`/`distclean` must not touch
  submodule-tracked files.
- Make `kg -V` report the feature (for example `+lisp`/`-lisp`) so tests,
  packagers, and bug reports can tell configurations apart.

Acceptance criteria:

- both configurations build warning-clean under GCC and Clang
- with Fe linked but not initialized, kg behaves identically
- a fresh clone without submodule init builds with `WITH_LISP=0` and produces
  the actionable error with `WITH_LISP=1`
- a dist tarball builds offline in both configurations
- license review is complete (MIT; rxi, Chris Palmer, and the fork)
- a submodule bump produces a reviewable diff

## Milestone 2: isolate interpreter lifecycle

Add `src/lisp.c` and a small `src/lisp.h` interface. The header should expose
kg-level operations, not Fe internals. A reasonable initial surface is:

```c
int kg_lisp_init(void);
void kg_lisp_shutdown(void);
int kg_lisp_eval_string(const char *source, size_t length,
    char *result, size_t result_size);
int kg_lisp_load_file(const char *path);
```

Decide the disabled-build shape here: with `WITH_LISP=0`, either compile a
stub `lisp.c` whose entry points report "not compiled in", or exclude the file
and guard its few call sites. Prefer whichever keeps `KG_USE_LISP` out of
editor modules; `main` and the command table should not accumulate scattered
conditionals. The `result` buffer of `kg_lisp_eval_string` is display-oriented
(status line): truncating the printed representation for display is
acceptable, but truncation must never affect evaluation itself or data passed
back into editor operations.

Keep a private state object in `lisp.c` containing the arena, context, host
userdata, active evaluation frame, cancellation state, and last error text.
Allocate the arena with a named, configurable default rather than an unexplained
2 MiB literal. Check allocation and `FeOpenContext` failures.

Initialize the interpreter explicitly from `main`, after base editor state is
valid. Shut it down on every normal exit path. If an `atexit` fallback is kept,
make shutdown idempotent; do not put Lisp ownership in `editor_at_exit`, whose
responsibility is terminal restoration.

Acceptance criteria:

- repeated init/shutdown is either supported or rejected deterministically
- startup allocation failure reports an editor error instead of exiting inside
  Fe
- Valgrind reports no interpreter lifecycle leaks
- noninteractive `kg -h` and `kg -V` do not initialize Fe

## Milestone 3: safe evaluation boundary

Use Fe's context-userdata and handler setter APIs. Do not use a process-global
`jmp_buf`. Each kg evaluation creates a private frame containing:

- saved Fe GC-stack index
- `jmp_buf`
- pointer to the previous frame, if nested evaluation is supported
- source label and error destination
- evaluation budget/cancellation data

The Fe error callback retrieves kg's Lisp state from the context, copies the
error message without allocating Fe objects, and jumps only to the active
frame. Cleanup after `setjmp` must restore the GC stack and close host resources
such as files. Define and test whether nested evaluation from a native callback
is supported; otherwise reject it explicitly.

Use Fe's length-aware multi-form evaluation helper for strings and files. Do
not create temporary files or maintain a second ad hoc reader in kg. Return the
last value as display text for interactive evaluation, but avoid showing every
value while loading configuration.

Every top-level evaluation must have a finite step budget and an interrupt
hook. Startup configuration gets a conservative budget. Interactive evaluation
should also allow `C-g` cancellation without waiting for the expression to
finish. A Lisp `(while t)` must not hang the editor.

Acceptance criteria:

- syntax, type, OOM, and budget errors return control to kg
- a second valid expression succeeds after each error class
- repeated multi-form evaluation does not grow the GC protection stack
- embedded NUL and truncated input behavior is defined and tested
- evaluation cannot leave raw mode or terminal state corrupted

## Milestone 4: define a narrow editor bridge

Start with value-oriented operations that have clear ownership and undo
semantics:

- `(kg-message string)`
- `(kg-insert string)`
- `(kg-buffer-name)`
- `(kg-point)` and `(kg-goto row column)`
- `(kg-command string)` for an explicit allow-list of named commands

Use Fe's native binding helper, exact-arity checks, and length-aware string
extraction. Never silently truncate a Lisp string into a fixed local buffer.
Return Fe's public nil accessor rather than referencing a global `nil` object.

Before implementing `kg-command`, refactor `src/cmd.c` so M-x and Lisp share a
single command lookup/execution API. Preserve the existing picker ordering and
keep command descriptors private to the command module. Commands that prompt,
run a shell, save files, or exit require explicit policy; do not expose the
entire static table by default.

All mutating wrappers must enforce read-only mode and use existing editor
operations so cursor normalization, dirty flags, syntax refresh, and undo are
preserved. Decide whether one Lisp call or one top-level expression is one undo
unit and add tests for that decision.

Acceptance criteria:

- wrong type, missing argument, and extra argument errors are deterministic
- large inserted strings are complete or rejected, never truncated
- read-only buffers cannot be modified through Lisp
- one bridge failure does not poison later evaluation
- bridge functions do not expose pointers into rows that reallocations can
  invalidate

## Milestone 5: user-facing evaluation commands

Add `eval-expression` to the existing named-command table. Reuse the minibuffer
input machinery, but select a deliberate expression-size limit and report
overflow rather than evaluating a truncated expression. Display the last
result in the status area and preserve a useful, source-labelled error.

After that path is stable, add `eval-buffer` or `eval-region`. Region extraction
must use the established region API and preserve embedded-byte policy. Do not
make a direct key binding mandatory in the first release; M-x is sufficient.

Add PTY tests for:

- evaluating an insertion and saving the resulting buffer
- displaying a scalar result
- syntax and type errors followed by successful editing
- cancellation or budget exhaustion
- read-only rejection

## Milestone 6: startup configuration and package loading

Resolve the init file in this order:

1. an explicit command-line override, if added
2. `$XDG_CONFIG_HOME/kg/init.fe`
3. `$HOME/.config/kg/init.fe`

Missing files are normal. Add `-Q`/`--no-init-file` before enabling automatic
loading so users can recover from a broken configuration. Reject unsafe path
construction when environment variables are unset or too long.

Load the file only after a current buffer and the bridge are available. Do not
overwrite a load error immediately with the generic "Press Ctrl-h for help"
message. Define partial-application behavior: forms evaluated before an error
remain applied unless a real transactional mechanism is implemented.

### Package loading

Add a `(kg-load NAME)` native as the explicit extension-package mechanism:

- A bare name resolves to `<config>/lisp/NAME.fe` using the same XDG-then-HOME
  fallback as the init file; a name containing `/` is treated as a literal
  path. Apply the same unsafe-path rejection rules as for the init file.
- Loading re-enters the evaluator. Nested evaluation inherits the ambient
  budget and cancellation state (Fe's evaluation-control semantics), so a
  package cannot reset the limits its caller established.
- Errors must name the failing file through Fe's source labels; "error in
  init.fe" is not acceptable when the failure is three loads deep.
- Guard against load cycles with an explicit depth limit and a clear error.
  Packages loading packages is supported; unbounded recursion is not.
- No `require`/`provide` in the first release: loading a file twice evaluates
  it twice. Document this; users structure `init.fe` to load each package
  once.
- Because `init.fe` is the only automatic entry point, `-Q` also disables all
  package loading. `kg-load` remains callable from `eval-expression` for
  interactive testing.

Tests must run with a temporary HOME/XDG directory and cover missing, valid,
invalid, over-budget, and bypassed init files, plus package loading: a package
defining state that `init.fe` then uses, a missing package whose error names
the package, and the load-cycle guard. The PTY runner must never read a
developer's real configuration.

## Milestone 7: Lisp-defined commands and bindings

This milestone delivers actual editor customization rather than only an eval
console.

Refactor the command registry to support both static C commands and dynamic
Lisp commands through one lookup/list/execute interface. A Lisp command must be
rooted for its registration lifetime and released when redefined or removed.
Use Fe's call/root APIs rather than constructing or retaining internal pairs in
kg.

Candidate Lisp API:

- `(kg-define-command "name" function)`
- `(kg-remove-command "name")`
- `(kg-bind-key "C-c x" "name")`
- `(kg-unbind-key "C-c x")`

Do not implement key parsing twice. Introduce one canonical key-sequence parser
used by configuration, help display, and dispatch. Dynamic commands must appear
in M-x completion and must obey the same evaluation budget and error recovery
as `eval-expression`.

Acceptance criteria:

- redefine/remove operations release old roots
- M-x lists and executes Lisp commands
- a bound key invokes the same command object as M-x
- errors in a Lisp command return to the editor event loop
- invalid or conflicting key sequences produce clear errors
- a package loaded with `(kg-load ...)` from `init.fe` can define and bind a
  command that then works identically from M-x and from its key — this is the
  end-to-end proof of the extension-package story

## Milestone 8: test, fuzz, and documentation closure

Add a native `test/test_lisp` with editor stubs for lifecycle, bridge, error,
budget, rooting, and repeated-evaluation tests. Use PTY cases for minibuffer,
screen message, key binding, init-file, and saved-buffer behavior.

Extend fuzzing in two layers:

- replay Fe's raw reader and bounded evaluator fuzz targets against the pinned
  submodule commit
- extend kg's keypress harness only after eval prompts and dynamic commands can
  be exercised without filesystem or shell side effects

Every gate must cover both build configurations. Keep the default pipeline on
`WITH_LISP=1`, add at least one full build plus `make check` with
`WITH_LISP=0`, and teach the PTY harness to skip Lisp cases when the binary
reports `-lisp` (via `kg -V`) instead of failing them. Sanitizer and fuzz
stages target the default configuration only.

Update `README.md`, the man page, built-in help, `AGENTS.md`, and packaging
metadata. Document that Lisp is trusted code, list resource limits, explain
`-Q`, the `WITH_LISP` build option, `(kg-load ...)`, and the
`$XDG_CONFIG_HOME/kg/lisp/` package directory, and provide a minimal recovery
procedure.

Final release gate:

- `make check`
- Fe parser/evaluator fuzz smoke
- kg keypress fuzz smoke
- complete `.ci/run-ci-steps.sh`
- a clean install/dist build
- manual smoke test over a real PTY

## Risks to track

- **Editor hangs:** mitigated by mandatory evaluator budgets and interrupt
  polling.
- **Stale Fe pointers:** mitigated by root handles and no raw object storage in
  editor structures.
- **Longjmp leaks:** mitigated by evaluation frames and host cleanup outside the
  jump region.
- **Undo fragmentation:** requires an explicit transaction policy before broad
  mutation APIs.
- **Broken startup:** mitigated by `-Q`, isolated config paths in tests, and
  nonfatal load errors.
- **Upstream drift:** mitigated by a pinned submodule commit and a documented
  bump-review-CI refresh procedure.
- **Uninitialized submodule:** mitigated by an actionable Makefile error, the
  `WITH_LISP=0` escape hatch, and dist tarballs that embed the Fe sources.
- **Configuration drift between builds:** mitigated by CI building and testing
  both `WITH_LISP` settings and by `kg -V` reporting the feature.
- **False sandbox expectations:** mitigated by excluding Fe's external-effect
  extensions initially and documenting that kg's Lisp remains trusted code.
