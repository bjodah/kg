# Code deduplication and developer ergonomics review

Scope: kg, Fe, and tiny-regex-c as checked out on 2026-07-30. This is a
read-only review; no product code was changed. The recommendations are bounded
to ten items: six local refactors and four larger boundary changes.

## Executive triage

| Overall rank | Recommendation | Scope | Payoff | Regression risk |
|---:|---|---|---|---|
| 1 | Give tiny-regex-c's compiler one emitter cursor | Small | Fixes a confirmed bookkeeping defect and removes repeated capacity arithmetic | Low–medium |
| 2 | Make kg buffer state owned, rather than copied through ambient globals | Architectural | Removes the strongest accidental coupling in kg and makes buffer-local extension state practical | High |
| 3 | Make kg command policy one descriptor, then let keys/help consume it | Small first, larger later | Prevents command, Lisp, read-only, key, and help metadata from drifting | Medium |
| 4 | Centralize kg local-variable value decoding and application | Small | Removes three semantic implementations of the same two variables | Medium |
| 5 | Add Fe registered user-type descriptors | Architectural | Replaces hard-coded Fex slots and switch edits with an actual extension boundary | Medium–high |
| 6 | Add an error-unwound external-resource stack to Fe | Architectural | Makes native extensions safe and much easier to write in the presence of `longjmp` errors | Medium–high |
| 7 | Introduce kg change primitives and, later, edit transactions | Architectural | Centralizes undo/update/dirty invariants spread across ten modules | High |
| 8 | Make tiny-regex-c match accounting per-execution | Small | Restores reentrancy/thread-safety and removes hidden process-global state | Low |
| 9 | Make test doubles composable; remove tiny-regex-c's copied private layout | Small | Reduces four kg stub variants and deletes knowingly stale white-box tests | Low |
| 10 | Table-drive Fe's numeric native adapters with exact arity | Small | Removes 21 wrappers/registrations and fixes inconsistent excess-argument handling | Low–medium |

The order is deliberate. Rank 1 is both a refactor and a demonstrated defect.
Rank 2 is the largest leverage point for kg, but should not be mixed into an
unrelated feature. Ranks 3 and 4 are good preparatory work because they create
single sources of truth without first changing the editor's storage model.

## Small and local refactors

### 1. Give `re_compile_to()` one emitter cursor

The compiler currently represents its output position twice:

- `re_compiled` is the physical node pointer
  (`fe/tiny-regex-c/re.c:587-593`).
- `j` is intended to be the logical node index
  (`fe/tiny-regex-c/re.c:587-589`).

Most source characters emit one node, so the shared increment at
`fe/tiny-regex-c/re.c:827-829` keeps them aligned. The invalid `\x` fallback is
the exception: it advances `re_compiled` and emits three or four `CHAR` nodes,
with four repeated `RE_HAS_ROOM`/initialize sequences
(`fe/tiny-regex-c/re.c:742-760` and `fe/tiny-regex-c/re.c:767-789`), but `j`
still advances only once. A later `\)` searches backwards using `j`
(`fe/tiny-regex-c/re.c:693-712`), and interval quantifiability also consults
that index (`fe/tiny-regex-c/re.c:570-579`).

This is observable under the fallback semantics the code explicitly
implements: `\xZ` matches the literal bytes `\xZ`, and `\(\xZ\)` works, but
`\xZ\(a\)` failed to match literal `\xZa` in a direct run of the existing
`tests/test_rand` driver. The group is physically after three emitted nodes
but logically after one, so its closing scan never visits the opener.

Refactor to a small internal compiler cursor:

```c
struct re_emitter {
	unsigned char *base;
	unsigned char *end;
	regex_t *next;
	unsigned nodes;
};
```

`emit_node()` should do the room check, initialize the node, advance `next`,
and increment `nodes`; `emit_char()` should layer on it. The main compiler,
invalid-hex fallback, group back-scan, and `quantifiable()` then consume the
same count. Do not merely add `j += 2`/`j += 3`: that preserves the duplicated
invariant and invites the next multi-node escape to repeat the bug.

Tests:

- Add observable compile-and-span cases to
  `fe/tiny-regex-c/tests/test_api.c:45-97` for invalid `\x` before, inside, and
  after a group.
- Include nested groups and an interval after the fallback, since both depend
  on node indices.
- Run tiny-regex-c's normal, ASan/UBSan, MSan, fuzz, and differential gates.

### 2. Make kg command policy one descriptor

kg has a useful command table already: 54 static commands with function and
mutation flag in `src/cmd.c:751-818`. However, the Lisp bridge restates an
11-command subset and restates whether each mutates
(`src/lisp.c:1224-1241`). Read-only policy is consequently enforced once in
the dispatcher (`src/cmd.c:835-854`) and again in the Lisp bridge
(`src/lisp.c:1255-1297`).

The local first step is to add a `CMD_LISP_CALLABLE` flag to `named_cmd` and
expose a policy-aware dispatcher, for example
`cmd_execute_from_lisp(name, fd, prefix)`. That deletes `allowed_commands`,
its linear search, and the second read-only check. It also makes adding a Lisp
callable command a one-line table change instead of two coordinated edits.
Preserve a narrow allowlist; this recommendation is not to make all M-x
commands callable from Lisp by accident.

The subsequent, larger step is to let a command descriptor optionally carry a
default key sequence, mode predicate, and short help label. Today those facts
live in:

- the named-command table (`src/cmd.c:757-818`);
- direct C-x and top-level switch dispatch
  (`src/kbd.c:367-461` and `src/kbd.c:625-1053`);
- small mode maps such as dired's table (`src/kbd.c:463-498`);
- the hand-laid help text (`src/help.c:18-96`).

Do this incrementally. First unify executable policy; then convert one bounded
map such as dired; only then consider generated help. A single mega X-macro
covering every key is likely to make prefix-state handling harder to read.

Tests:

- Keep the existing Lisp allow/deny and read-only cases in
  `test/test_lisp.c`; add a table test that every Lisp-callable descriptor is
  unique and executable.
- For later key metadata, assert lookup results directly and retain PTY cases
  for prefix timing and mode-specific shadowing.
- A generated help view needs a width/ordering golden test; otherwise a
  metadata cleanup could silently degrade the 79-column display.

### 3. Centralize local-variable value decoding and application

The three supported local-variable syntaxes need different envelope scanners,
but they do not need three implementations of value semantics.

String escape decoding is duplicated almost statement-for-statement in
`parse_quoted_string()` (`src/localvars.c:35-92`) and `dlr_read_str()`
(`src/localvars.c:528-590`). More importantly, recognition and application of
`compile-command` and `buffer-read-only` appear three times:

- modeline: `src/localvars.c:272-313`;
- `.dir-locals.el`: `src/localvars.c:763-828`;
- footer: `src/localvars.c:1248-1368`.

Each path independently lowercases booleans, chooses malformed versus ignored
counters, bounds the compile command, and copies into `local_settings`.
Adding one more safe variable currently means implementing its policy three
times.

Keep the envelope parsers separate. Extract:

1. a bounded escape decoder over a `[begin, end)` slice, returning a named
   result enum rather than `-1/-2/-3`;
2. a `parse_local_bool()` helper;
3. one `local_settings_apply(name, typed_value, out)` policy function.

The scanners should produce a small typed value (`STRING`, `SYMBOL`,
`UNSUPPORTED`, `MALFORMED`) and the common apply function should be the only
code that knows the two accepted variable names. Footer continuation remains
footer-specific; S-expression skipping remains dir-locals-specific.

Regression risk is medium because the three syntaxes intentionally accept
different envelopes. Extend `test/test_localvars.c` with a matrix containing
the same string escapes, case variants of `t`/`nil`, oversize values, unknown
variables, and malformed values in all three formats. The result fields and
both counters must agree where the value semantics agree.

### 4. Move tiny-regex-c's work/depth counters into `re_ctx`

`re_match_steps` and `re_match_depth` are file-scope globals
(`fe/tiny-regex-c/re.c:294-298`). Every execution resets them
(`fe/tiny-regex-c/re.c:1656-1667`), every recursive match mutates them
(`fe/tiny-regex-c/re.c:1618-1636`), and `re_exec()` reads the global step
counter to decide between no-match and too-complex
(`fe/tiny-regex-c/re.c:436-451`).

That makes otherwise independent `re_exec()` calls race. One thread can reset
another's budget or make a simple match report `RE_STATUS_TOO_COMPLEX`.
`re_ctx` already carries all other execution-local state
(`fe/tiny-regex-c/re.c:1290-1296`), so add `steps` and `depth` there and make
the status flow return through that context. This is a small, API-neutral
change.

Test with two concurrent loops over the same compiled expression: one forces
heavy bounded backtracking and the other repeatedly performs a trivial match.
ThreadSanitizer is the most reliable verification if added to this submodule;
a normal stress test alone cannot prove absence of a race.

### 5. Make test doubles composable and stop copying private regex layout

kg maintains four versions of the same base process/editor globals:

- `test/stubs.c:10-30`;
- `test/stubs_noyank.c:7-27`;
- `test/stubs_buffer.c:7-23`;
- `test/fuzz_stubs.c:10-26`.

The first two also duplicate the observable status/command fake
(`test/stubs.c:32-58` and `test/stubs_noyank.c:29-60`) and differ mainly
because one links the real yank module. `stubs_noyank.c` then uses weak
definitions for optional facilities (`test/stubs_noyank.c:61-121`), which can
make a newly introduced dependency link successfully while doing nothing.

Split the scaffolding by owned interface instead of by test binary:
`stub_editor_globals.o`, `fake_status.o`, `stub_minibuffer.o`,
`stub_special_buffers.o`, and `stub_killring.o`. Make each test's `EXTRA_*`
list select the needed pieces. Prefer a link failure for an unselected
dependency over a broad weak no-op. The fuzz harness can share the base
globals while retaining its input-pipe-specific stubs.

tiny-regex-c has the same coupling in a different form:
`fe/tiny-regex-c/tests/test_compile.c:15-29` copies the private `regex_t`
layout. The test itself admits that the copy predates the current compact
layout and marks three resulting checks xfail
(`fe/tiny-regex-c/tests/test_compile.c:95-119`). Delete those representation
checks. Express nested-group expectations through `re_exec()` spans, as
`fe/tiny-regex-c/tests/test_api.c:45-97` already does. If compiler inspection
is genuinely required, add a test-only dump in `re.c` under a build define so
the implementation remains the sole owner of its layout.

The kg risk is mostly linker plumbing; run every unit binary, not only a
representative one. The regex risk is low because the replacement tests are
black-box and stronger.

### 6. Table-drive Fe's numeric native adapters with exact arity

`FexInstallMath()` manually registers 21 functions
(`fe/fex_math.c:16-40`), and the file then defines 21 wrappers with repeated
`FeGetNextArgument`/conversion/result construction
(`fe/fex_math.c:43-158`). Unlike Fe's core numeric natives, which call
`FeRequireNoArguments` (`fe/fe.c:1501-1505`, representative), the Fex wrappers
silently ignore excess arguments. I/O natives have the same manual argument
pattern (`fe/fex_io.c:34-90`).

Use a bounded descriptor/adaptor layer for the genuinely uniform cases:

- unary double to double;
- unary double to boolean;
- binary double to double.

Each adapter must consume the declared arity and call
`FeRequireNoArguments`. Keep bespoke functions bespoke; do not force file,
process, or regex operations through a union-heavy generic schema merely to
save lines. A small `FeDefineNatives()` helper can also replace repeated
registration calls across Fex modules, matching the table approach kg already
uses at `src/lisp.c:1581-1642`.

Add one correct-arity and one excess-argument test per adapter class, plus
domain-edge values (`NaN`, infinities, negative square root) to ensure the
table does not accidentally normalize libc behavior.

## Larger boundary changes

### 7. Make buffer state owned instead of copied through `editor`

This is kg's most consequential accidental coupling. `editor_config`
interleaves terminal state, key-prefix state, current buffer data, local
settings, marks, and view coordinates (`src/def.h:258-324`).
`editor_buffer` duplicates roughly thirty of those fields
(`src/def.h:390-424`). Switching buffers manually copies the duplicated set in
both directions (`src/bufmgr.c:45-140`).

There is also a second, partial copy protocol for background special-buffer
updates. `editor_buffer_swap` carries only rows, dirty state, syntax, filename,
read-only fields, and undo (`src/bufmgr.c:1682-1693`);
`buf_temp_swap_in/out()` hand-copies those fields four ways
(`src/bufmgr.c:1695-1749`). Three callers must balance that protocol
(`src/bufmgr.c:1826-1928`, `src/bufmgr.c:1938-1947`, and
`src/bufmgr.c:1960-1972`). The dired code documents a real consequence:
nested special-buffer setup can overwrite a half-reset outgoing slot
(`src/dired.c:321-325`).

The target model should separate:

- process/editor state: terminal dimensions, prefix parser, paste timing,
  status area;
- buffer-owned state: rows, filename, syntax/mode, dirty/undo, local
  variables, marks, disk snapshot;
- window-owned view state: cursor and scroll for a displayed buffer.

Make `buflist[buf_current]` the live buffer object and pass/access it directly;
do not mirror it into `editor`. Background compilation can then mutate its
target buffer without temporarily impersonating it as the current one. This
also gives Lisp buffer-local variables and modes an obvious ownership home.

Stage it:

1. Move only content and settings into an explicit `buffer_state`, leaving
   cursor/view behavior unchanged.
2. Convert helpers to accept `buffer_state *`; eliminate the partial special
   swap.
3. Move view coordinates to windows and remove the full save/restore copy.

Do not combine the first stage with a new feature. The regression surface is
all multi-buffer behavior: per-buffer undo/marks/read-only/local variables,
split views, async compilation into a non-current buffer, dired refresh, auto
revert, kill/select, and save-all. Existing PTY coverage is useful, but add a
focused native invariant test that changes every buffer-owned field in two
slots and proves switching/background append cannot cross-contaminate them.

### 8. Give Fe a registered user-type descriptor API

Fe's extension type boundary is explicitly hard-coded. `FeType` reserves three
Fex slots and tells contributors to edit the enum, every relevant switch, the
global name table, and installation code (`fe/fe.h:32-67`). Those slots appear
in core marking, writing, and evaluator switches
(`fe/fe.c:388-418`, `fe/fe.c:652-735`, and `fe/fe.c:1294-1333`).
Fex aliases two slots (`fe/fex.h:11-12`), mutates Fe's global `type_names`
array, installs one global GC callback (`fe/fex.c:12-40`), and must enumerate
even the unused third slot.

Replace numbered public slots with per-context registered descriptors:

```c
struct FeUserTypeDescriptor {
	const char *name;
	void (*mark)(FeContext *, void *);
	void (*finalize)(FeContext *, void *);
	void (*write)(FeContext *, void *, FeWriteFn *, void *);
};
```

`FeRegisterUserType()` returns a small handle stored in a pointer object;
`FeMakeUserObject()` associates payload and handle. Core code treats all such
objects uniformly and dispatches descriptor callbacks. Registration must be
bounded by the object tag's representable range and must not allocate outside
the caller-provided arena unless explicitly documented.

This is a compatibility-sensitive change, so retain `FeMakePtr` as the
unmanaged/default pointer type and offer a transition for Fex. Test two
simultaneous host-defined types, distinct names, marking through payload
references, exactly-once finalization, printing, invalid handles, and context
teardown. Update `fe/doc/c-api.md` and `fe/doc/implementation.md`.

### 9. Give Fe an error-unwound external-resource stack

Fe's arena objects are protected by its GC stack, but native extensions have
no equivalent for temporary heap/file resources. Errors do not return:
`FeHandleError()` resets interpreter state and invokes the host error function
(`fe/fe.c:248-281`), which normally `longjmp`s. Compiler cleanup attributes
therefore cannot help.

Current code handles this ad hoc:

- `FexExecute()` repeats `FreeArguments()` before two error paths
  (`fe/fex_process.c:19-45`);
- regex compilation threads one previous allocation through
  `Allocate(..., cleanup)` (`fe/fex_re.c:22-29` and
  `fe/fex_re.c:101-133`);
- `FexReadFile()` holds `getdelim()` memory while constructing a Fe string
  (`fe/fex_io.c:51-65`);
- `FexWriteFile()` holds a 4 MiB allocation while converting and constructing
  a result (`fe/fex_io.c:75-90`).

If an intervening Fe allocation raises out-of-memory, the C allocation cannot
be freed after the call. The pattern gets harder as richer kg natives acquire
buffers, regex programs, subprocess state, and roots.

Add a small per-context cleanup stack with checkpoints:

- `FeSaveCleanups(ctx)`;
- `FeDeferCleanup(ctx, fn, payload)`;
- `FeCancelCleanup(ctx, token)` or restore-without-running on ownership
  transfer;
- unwind-and-run from `FeHandleError()` before calling the host error
  function.

Callbacks must be non-allocating, non-raising, and safe in reverse
registration order. This complements, rather than replaces, `FeSaveGC` /
`FeRestoreGC` (`fe/fe.c:364-377`). A convenience GC-frame helper may reduce
the repeated restore-and-repush sequences at `fe/fe.c:915-931`,
`fe/fe.c:1081-1088`, and `fe/fe.c:1287-1337`, but it does not solve external
resource unwinding.

Test by forcing tiny arenas and injected allocation failures at every native
boundary, with cleanup counters plus ASan/Valgrind. Also test a normal
ownership transfer so registered resources are not freed twice.

### 10. Centralize kg mutations as change primitives, then transactions

Undo is pushed directly from 45 call sites across ten source files
(`src/buffer.c`, `src/cmd.c`, `src/fileio.c`, `src/kbd.c`, `src/lisp.c`,
`src/rect.c`, `src/search.c`, `src/shell.c`, `src/word.c`, and `src/yank.c`;
see representative pairs `src/lisp.c:409` with its raw insertion,
`src/search.c:703`, and `src/word.c:1449-1457`). Direct row edits separately
remember to call `editor_update_row()` and change `editor.dirty`, for example
`src/word.c:702-707`, `src/rect.c:111-112`, and `src/yank.c:385-418`.

This makes every new command responsible for four invariants:

1. snapshot enough original text;
2. choose the correct undo opcode and coordinates;
3. mutate rows without recursively adding undo;
4. refresh syntax/render data and dirty state.

The small first move is a bounded
`editor_replace_range_with_undo(start, old_len, replacement, replacement_len)`
used by the existing replace-like commands. It should fail atomically if the
undo record or replacement storage cannot be allocated.

The later design is an edit transaction containing ordered primitive changes
and one undo group. Rectangle, reflow, query-replace, Lisp insertion, shell
replacement, and multi-row transforms can use the same commit path. Keep
cursor motion and user messages outside the transaction. Avoid a callback
transaction that relies on stack unwinding; explicit begin/commit/abort is
clearer in this C codebase.

This is high risk because current undo granularity is user-visible (for
example trailing whitespace intentionally records per line at
`src/cmd.c:116-139`). Before conversion, characterize one-step versus
multi-step undo for each migrated command with PTY cases, plus allocation
failure tests proving a failed transaction leaves rows, syntax, dirty state,
and the undo stack unchanged.

## What I would not deduplicate yet

- The capture snapshots in tiny-regex-c are repeated at four backtracking
  choice points (`fe/tiny-regex-c/re.c:1404-1422`,
  `fe/tiny-regex-c/re.c:1430-1460`, `fe/tiny-regex-c/re.c:1479-1498`, and
  `fe/tiny-regex-c/re.c:1542-1559`), but the existing `save_spans()` /
  `restore_spans()` helpers make the invariant explicit. A generic
  “try-with-rollback” callback would obscure continuation flow for little
  gain.
- I would not merge the modeline, footer, and `.dir-locals.el` envelope
  parsers. They have materially different grammars and safety limits. Share
  value semantics only.
- I would not generate all kg keyboard dispatch from one macro in the first
  pass. Prefix state, mode precedence, paste detection, and direct editing
  commands are different concerns. Consolidate command policy and one small
  mode map before deciding how much key metadata is truly uniform.
- I would not replace Fe's fixed arena with general heap allocation as an
  ergonomic shortcut. Registered type descriptors and cleanup checkpoints can
  improve extension authoring while preserving the arena contract.

## Recommended execution sequence

1. Fix the regex emitter/counter defect and add black-box regressions.
2. Move regex match budgets into `re_ctx`; clean the stale private-layout test.
3. Add command flags/dispatcher policy and centralize local-variable value
   application in kg.
4. Split kg's test support objects so the later refactors have sharper tests.
5. Design Fe user-type descriptors and cleanup checkpoints together, but land
   them as separate changes.
6. Start kg buffer ownership with content/settings only; remove the special
   background swap before moving view state.
7. Introduce one replace primitive and migrate commands in small families,
   preserving documented undo granularity.

That sequence takes the low-risk correctness wins first, then improves the
test and metadata seams needed to make the two large architectural changes
reviewable.
