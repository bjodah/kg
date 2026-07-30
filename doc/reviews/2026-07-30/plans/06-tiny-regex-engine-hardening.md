# Plan 06 — tiny-regex-c compiler, matcher, and portability hardening

## Goal

Make tiny-regex-c's accepted language truthful, portable, reentrant, and
bounded before considering a new regex VM. Nothing here changes kg's `src/`;
wrapper status plumbing, offset contracts and replacement progress are Plan 03.

## Verified state of the tree

Re-checked against tiny-regex-c `de5f96a` (fe `0ca1229`, kg `906e48f`). Line
numbers are from that commit and will drift.

- `regex_t` (`re.c:135-158`): `unsigned short type` plus a 4-byte union;
  `sizeof == 6`, `_Alignof == 2`. Class data overlays the union and runs past
  the object (`RE_CCL_STR`/`RE_CCL_DAT`, `re.c:168-169`).
- Constants (`re.c:52-101`): `MAX_REGEXP_OBJECTS 30`, `RE_INTERVAL_MAX 65535`,
  `RE_REP_INF UINT_MAX`, `MAX_GROUP_REPEATS 256`, `MAX_MATCH_STEPS 2000000`,
  `MAX_MATCH_DEPTH 4096`. `MAX_CHAR_CLASS_LEN` and `MAX_REGEXP_LEN` are dead.
- Compiler: `re_compile_to()` (`re.c:582-846`) with `compile_charclass()`,
  `parse_count()`, `compile_interval()`, `quantifiable()`, `getsize()`,
  `getnext()`, `getindex()`. `getindex()` is **compile-time only** (`re.c:578`,
  `:697`); it is not on the match path.
- Matcher: `re_matchp_internal()` → `match_seq()` → `match_seq_body()` →
  `match_atom()`/`match_group()`/`match_anchor()`/`match_alt()`, closed by
  `match_cont()`/`match_rep()`/`match_group_iter()`; `group_end()`,
  `atom_end()`, `find_branch()`, `quant_bounds()`, `group_span()` are hot.
- `re_ctx` (`re.c:1289-1296`, a typedef with no tag) holds `text_start`,
  `prog_end`, `out`, `has_branch` — but not the budgets.

Confirmed defects, each with the probe that reproduces it:

1. **Alignment UB, reachable.** `re_compile_checked()` returns `(re_t)storage`
   (`re.c:398`); `re_compile_to()` casts the same way (`re.c:593`); the
   internal 8 KiB automatic `temp_buffer` is cast at `re.c:379`. Compiling into
   `raw + 1` and executing aborts under `clang -fsanitize=undefined
   -fno-sanitize-recover=all` — *"member access within misaligned address … for
   type 'struct regex_t', which requires 2 byte alignment"*, at `re.c:421` in
   `re_exec()`'s group-count scan. ~15 lines of driver.
2. **The two compiler cursors do desynchronize.** `re_compiled` (physical)
   advances up to three extra nodes in the invalid-`\x` fallback
   (`re.c:737-791`) while `j` (logical) advances once per iteration
   (`re.c:828`). `j` feeds `quantifiable()` and the `\)` backward scan
   (`re.c:693-713`), so a later *valid* group is misparsed:

   ```text
   \(a\)x\(b\)     on axb    -> match 3 0 3 0 1 2 3
   \(a\)\xZ\(b\)   on a\xZb  -> badpat        <-- desync
   ```

   The backward scan meets `CHAR 'Z'/'x'/'\\'` where group 1's `GROUPEND`
   should be, runs off the front and returns 0. `group_size` is written wrong
   too (`re.c:702`), harmless only because the matcher distrusts it.
3. **Malformed patterns accepted.** `\(` only scans for *some* later `\)`
   (`re.c:664-691`) and nothing checks the group stack is empty before the
   sentinel (`re.c:831-839`); `compile_charclass()` reaches NUL and still
   returns 1 (`re.c:470-505`); `*`, `+`, `?` test only `j > 0`
   (`re.c:619-636`) rather than `quantifiable()` (`re.c:573-580`). Executed:

   ```text
   pattern       subject   kg                    Emacs
   \(\(a\)       a         match 3 0 1 0 1 0 1   badpat
   [a            a         match 1 0 1           badpat
   []            x         nomatch               badpat
   *             *         badpat                match 1 0 1
   ^*            *         match 1 0 0           match 1 0 1
   a++           aa        nomatch               match 1 0 2
   ```

4. **Execution state is global.** `re_match_steps`/`re_match_depth`
   (`re.c:294-298`), reset at `re.c:1665-1666`, read back at `re.c:447-449`. A
   nested or concurrent `re_exec()` clobbers both. `re_compile()`'s buffer is
   one shared `static` (`re.c:848-858`).
5. **The 256 ceiling is a false no-match.** `match_group_iter()` returns plain
   `NULL` at `done >= MAX_GROUP_REPEATS` (`re.c:1512-1513`), reported as
   `RE_STATUS_NO_MATCH`. Boundary confirmed exactly: `\(a\)\{256\}` on 256 a's
   matches (`match 2 0 256 255 256`), `\(a\)\{257\}` on 257 a's does not,
   while the single atom `a\{300\}` on 300 a's matches.
6. **Depth, not steps, is the next wall.** Rebuilt with `MAX_GROUP_REPEATS`
   at 100000, `\(a\)\{n\}` matches to n = 2047 and returns `TOO_COMPLEX` from
   n = 2048 — exactly `MAX_MATCH_DEPTH / 2 - 1`, i.e. two `match_seq()` frames
   per repetition. Phase 5 alone buys honest status and ~2000 repeats; the
   advertised 65535 needs phase 6's work stack.

Corrected claims from the previous draft:

- `getindex()` never runs during matching. `group_end()` and `find_branch()`
  are the real hot rescans (phase 7).
- "Delete stale tests that copy private layout" collides with three *expected*
  failures; see below.

## Things this plan must not break

- **The three `xfail`s in `tests/test_compile.c` are pre-existing.** Its
  nested-group section declares a *local* `regex_t` (`:15-28`: `unsigned type`,
  `char ch`, `char* ccl`) that no longer matches `re.c`, so `[20]
  [0].group_num`, `[21] [1].group_num` and `[22] [4].group_start` read the
  wrong bytes. `make check` prints them as `(xfail)` and still reports `23/23
  tests succeeded`; an unexpected pass is counted as `unexpected` and fails. A
  layout change must keep them failing, or convert them to observable
  assertions *and* drop the xfail accounting in the same commit. Do not
  describe removing them as fixing three failures.
- **`make -C fe/tiny-regex-c verify` (CBMC) does not compile** and is not in
  `.ci/`: `verify_re_match()` reads `pattern[i].u.ccl` (`re.c:1731`, a union
  member that no longer exists) and passes `regex_t (*)[8]` to `re_match()`
  (`re.c:1739`). Repair it in the phase that stabilizes the node layout, or
  delete it; do not cite CBMC as live coverage meanwhile.
- `tests/test_end_anchor.c` and `tests/test_print.c` are not in the Makefile's
  `TEST_BINS`, so they are neither built nor run.
- `re_string()` is lossy — `re_string("a[0-9]+")` yields `"a[0-9]"`
  (`re.c:965-1019` emits no `STAR`/`PLUS`/`GROUP` text) — so it cannot serve as
  a compile round-trip oracle. A test dump must be new and explicit.

## Read first

- `re.c`, `re.h` (its header comment *is* the dialect spec), `README.md`
  §"Supported regex-operators" and §"Characters, not bytes"
- `tests/test1.c`, `tests/test2.c` (backtracking sweep, `RE_TEST2_MAX_BYTES`),
  `tests/test_compile.c`, `tests/test_api.c`, `tests/{ok,nok,xfail_ok}.lst`,
  `scripts/regex_test{,_neg}.py`, `fuzz/fuzz_regex.c`, `fuzz/seeds/regex`
- `.ci/run-ci-steps.sh` (globs `.ci/ci-[0-9][0-9]-*.sh`; two scripts share 06)
- kg `src/regex.h`, `src/regex.c` — downstream storage and status use only

## Submodule and pin policy

The chain is **kg → fe → tiny-regex-c**, and it is not what kg's commit
messages imply. `.gitmodules` pins `fe` on branch `analyzers-etc`;
`fe/.gitmodules` pins `tiny-regex-c` on branch `adapt-to-fe`. kg nonetheless
compiles the engine **directly**: `Makefile:182` (`$(OBJDIR)/tiny_regex.o` from
`fe/tiny-regex-c/re.c`), `:367` (regex fuzzer), `:371` (differential driver),
with `-Ife/tiny-regex-c` on both `CFLAGS` and `FE_CFLAGS` (`Makefile:18-19`).
kg commit `115a131` ("Move the fe pin to the next tiny-regex-c bump") is
therefore wrong to call such a bump "nothing kg compiles". `doc/fe-upstream.md`
documents fe only and never mentions the nested submodule; adding a
tiny-regex-c section there is worth doing (outside this plan's edit scope).

Landing order for every phase:

1. Commit on `adapt-to-fe` in `fe/tiny-regex-c`, its tests/README/`.ci/` green;
   push.
2. Update fe's `tiny-regex-c` gitlink on `analyzers-etc`, `make -C fe check`;
   push.
3. Update kg's `fe` gitlink; run kg `make check`,
   `make check-regex-differential`, `.ci/run-ci-steps.sh` and
   `.ci/ci-08-with-lisp-0.sh` (the engine is compiled in both `WITH_LISP`
   configurations). State in the kg commit message what engine behavior moved.

Preserve the Emacs-style escaped operators the header and README document.

## Phase 1 — Specify the public storage contract

Files: `re.h`, `re.c`, `tests/test_api.c`, `fuzz/fuzz_regex.c`, README.

**Preferred:** stop overlaying an arbitrary `unsigned char *` with `regex_t *`.
Keep the program as bytes and reach multi-byte fields through `memcpy` load /
store helpers. Byte order stays process-local; the program is never persisted.

**Transitional**, if the typed overlay stays:

- publish `#define RE_STORAGE_ALIGNMENT 2u` and assert it in `re.c` with
  `static_assert(_Alignof(regex_t) == RE_STORAGE_ALIGNMENT)`;
- reject `(uintptr_t)storage % RE_STORAGE_ALIGNMENT` in both
  `re_compile_checked()` and `re_compile_to()` (reuse
  `RE_STATUS_BAD_PATTERN`, or add `RE_STATUS_BAD_STORAGE` — see the ABI note
  in phase 3 before growing the enum);
- declare `temp_buffer` (`re.c:371`) and `re_compile()`'s static buffer
  `alignas(regex_t)`;
- keep the checked-add discipline in `RE_HAS_ROOM` (`re.c:595-599`), whose
  comment already explains why the subtraction form underflows;
- do not expose the private `regex_t` layout in `re.h`.

**ABI note for kg.** `struct kg_regex` (`src/regex.h:23-27`) puts
`unsigned char storage[RE_MAX_COMPILED_BYTES]` at offset 0 of a struct whose
alignment comes from `re_t regex`, so kg is *incidentally* correct today and
stays correct under either design; Plan 03 should still add `alignas` rather
than rely on that. Fe allocates with `Allocate()` (`fe/fex_re.c:107-130`),
`malloc`-backed and therefore suitably aligned. Rejecting misaligned storage
is source-compatible for both.

Tests in `tests/test_api.c`: compile+exec into `raw + 1` (rejected, not UB) and
`raw + 2`; size query with `NULL` storage and `*storage_size == 0`; compile
twice into caller storage. Run under `clang -fsanitize=undefined,address
-fno-sanitize-recover=all`, which `.ci/ci-04-clang-asan-ubsan.sh` already
supplies. Make `fuzz/fuzz_regex.c`'s `small_buf[64]` exercise both alignments.

## Phase 2 — Replace the duplicate compiler cursors with an emitter

Files: `re.c`, `tests/test_compile.c`, `tests/nok.lst`.

```c
struct re_emitter {
	unsigned char *base;    /* re_data */
	unsigned char *next;    /* the node being written */
	unsigned char *end;     /* re_data + bytes */
	unsigned nodes;         /* replaces `j` */
};
```

Helpers, each advancing `next` and `nodes` together: `emit_node(type)`,
`emit_char(cp)` (wrapping `set_char_cp()`), `emitter_node_at(index)`
(replacing the `getindex((regex_t *)re_data, k)` calls at `re.c:578` and
`:697`), and `emitter_room()` (the checked remaining-storage calculation now
spelled by `RE_HAS_ROOM`).

Migrate literals, escapes, the invalid-`\x` fallback, groups, classes and
POSIX classes, interval metadata, and the `UNUSED` sentinel. Delete `j` and the
bare `re_compiled = getnext(re_compiled)` at `re.c:829`; the invariant "`j`
counts what `re_compiled` skipped" then cannot be violated.

Decide invalid-`\x` semantics explicitly and write it into `re.h`'s header
comment, which today documents only the valid form (`re.h:29-31`): either keep
the literal `\`,`x`,byte fallback and make it correct (three `emit_char()`
calls), or reject it, as Emacs does where `\x` is simply `x`. Do not change it
as an unannounced side effect of the refactor.

Tests: the repro `\(a\)\xZ\(b\)` must behave like `\(a\)x\(b\)` under the
fallback reading or be rejected under the strict one, with `\(a\)x\(b\)`
unaffected either way; invalid `\x` before, inside and after a group, twice in
a row, and before an interval (`\xZ\{2\}` currently quantifies the wrong node);
nested groups after a fallback; exact storage boundary (fallback needing the
last node, and one node more than fits — must return 0, never write past
`end`); too-small `*size` reports the required byte count.

Rewrite the three `test_compile.c` nested-group checks as span assertions
(compile, `re_exec()`, compare `re_match_result`) instead of private-layout
reads, and drop the xfail accounting in the same commit.

## Phase 3 — Build a real parser state/stack

Files: `re.c`, `re.h`, README, `tests/nok.lst`, kg `utils/regex_differential.py`.

Add an explicit compile-time stack, sized `RE_MAX_SPANS`:

```c
struct parse_frame {
	unsigned group_start;   /* emitter node index of the GROUP */
	unsigned branch_start;  /* first node of the current alternative */
	unsigned capture_index; /* group_num */
};
```

Track bracket-expression state separately in `compile_charclass()`. Reject:

- any `\(` still open at the sentinel (replaces the forward scan at
  `re.c:664-691`, which is then deleted);
- a `\)` with an empty frame stack (replaces the backward scan at `:693-713`);
- an unterminated bracket expression — `compile_charclass()` must fail on NUL
  rather than succeed at `re.c:501-505` — and empty `[]`;
- unknown POSIX classes (already rejected at `re.c:478-479`; keep) and
  malformed intervals (already rejected by `compile_interval()`; keep);
- quantifiers with nothing to repeat: route `*`, `+`, `?` through
  `quantifiable()`, as `\{` already is.

Where Emacs is literal instead — a leading `*`, `+`, `?`, and `*` right after
`^` — emit the literal character rather than failing. The goal is an exact
documented subset, not blanket strictness: the six rows in the table above are
the acceptance contract to settle, each a *documented decision* rather than
automatically "match Emacs". Return structured compile errors internally (`{ code, byte offset }`) so tests
can assert on them. Public v1 may still collapse them to
`RE_STATUS_BAD_PATTERN`; growing `re_status` is an ABI change that must move
`fe/fex_re.c`, kg's `src/regex.c` and `test/regex_differential.c` (which
consumes `RE_STATUS_TOO_COMPLEX` directly at `:41-45`) in the same pin step.

Tests: `tests/nok.lst` gains every newly rejected pattern and `tests/ok.lst` the
literal-quantifier ones — but both are driven by `scripts/regex_test.py` /
`regex_test_neg.py` against Python's `re`, so a pattern whose Emacs and Python
readings differ belongs in a C test instead. On the kg side,
`utils/regex_differential.py:216` counts kg `badpat`/`toocomplex` **or** Emacs
`badpat` as incomparable — exactly what hides these six rows. Replace it with a
small explicit allowlist of documented dialect differences, compare
acceptance/rejection otherwise, and extend `rnd_pattern()` (`:125-137`) to emit
malformed patterns deliberately.

## Phase 4 — Move execution state into a per-call context

Files: `re.c`, `re.h` (only if options become public), `tests/test_api.c`.

Extend the existing `re_ctx` rather than inventing a second struct. Every
matcher helper already takes `re_ctx *ctx`, so this is field additions plus
`init_ctx()` changes:

```c
typedef struct {
	const char *text_start;
	regex_t *prog_end;
	re_match_result *out;
	int has_branch;
	unsigned long steps, max_steps;   /* was re_match_steps */
	unsigned long depth, max_depth;   /* was re_match_depth */
	int exhausted;                    /* set instead of poisoning steps */
	int (*cancel)(void *);            /* polled every N steps */
	void *cancel_data;
} re_ctx;

typedef struct {
	unsigned long max_steps;   /* 0 = engine default */
	unsigned long max_depth;
	int (*cancel)(void *);
	void *cancel_data;
} re_exec_options;

re_status re_exec_with_options(re_t, const char *text, int start_offset,
                               const re_exec_options *opts, re_match_result *);
```

Delete the globals (`re.c:294-298`) and the poisoning assignment at `re.c:1630`
(`re_match_steps = MAX_MATCH_STEPS + 1`); `re_exec()`'s decision at
`re.c:447-449` becomes `ctx.exhausted ? TOO_COMPLEX : NO_MATCH`. `re_exec()`
keeps its signature and forwards `NULL` options — no ABI break for kg or fe.
Make the program `const` on the exec path (`re_exec`, `matchone`, `group_end`,
`find_branch`); the only writer is the compile-time ICASE pass (`re.c:378-386`).

`re_compile()`'s static buffer is `MAX_REGEXP_OBJECTS * sizeof(regex_t)` = 180
bytes, so it silently returns `NULL` — indistinguishable from a bad pattern —
from **29 literal atoms** onward (measured: 29 ok, 30 `NULL`). Deprecate it in
favor of `re_compile_checked()`: kg already uses the checked entry
(`src/regex.c:101`) and fe uses it twice (`fe/fex_re.c:111`, `:119`); only
`re_match()` (`re.c:339-345`) and the submodule's own harnesses still call it.

Tests: TSan with two threads on one compiled program and on two distinct ones
(one catastrophic, one trivial); a cancellation callback that runs a nested
`re_exec()`, outer budget surviving; a tiny `max_steps` on one context not
affecting another; cancellation returning `TOO_COMPLEX` (or a new cancelled
status) with the context reusable; `re_compile()` at 29 and 30 atoms.

## Phase 5 — Make the group repetition ceiling truthful

**Immediate fix.** When `match_group_iter()` hits `MAX_GROUP_REPEATS`
(`re.c:1512-1513`), set `ctx->exhausted` before returning `NULL` so `re_exec()`
reports `RE_STATUS_TOO_COMPLEX`, never ordinary no-match.

**Watch the empty-body fallback.** `match_rep()`'s `done >= k->min || !grew`
branch (`re.c:1560-1566`) exists precisely so a `min` past the ceiling can
still be satisfied by an empty body, and Emacs agrees. Marking exhaustion
unconditionally would regress it; set the flag only where the repetition had to
consume, and keep a case for each of:

```text
\(a*\)\{300\}   ""    -> match 2 0 0 0 0
\(\)\{300\}     ""    -> match 2 0 0 0 0
\(a*\)\{300\}b  "b"   -> match 2 0 1 0 0   (Emacs oracle agrees)
```

Add boundaries at 255 / 256 / 257 / 300 for: consuming group, empty group,
nested group, capture-register contents, and the single-atom `a\{n\}`
comparison that already works.

**Correct fix.** Raising the constant is not enough (finding 6): each
repetition costs two `match_seq()` frames against `MAX_MATCH_DEPTH`, so the
wall just moves to 2047. Reaching `RE_INTERVAL_MAX` (65535) needs phase 6's
work stack, whose frames carry the program continuation (`p`, `stop`), subject
position, `min`/`max`/`done`, capture-snapshot reference and backtrack
alternative. Preserve greedy ordering, the `save_spans()`/`restore_spans()`
rollback, and `match_rep()`'s documented Emacs quirk (`re.c:1528-1541`) — an
empty body only stops the loop from repetition `min + 1` onward, which is what
keeps captures after `\{n\}` agreeing with Emacs. Verify with
`make check-regex-differential` at a raised case count before and after.
Accepted counts up to 65535 must execute or return `TOO_COMPLEX` — never a
false no-match.

## Phase 6 — Account for actual work and remove deep C recursion

Files: `re.c`, `re.h` (budget units), `tests/test2.c`, `fuzz/`, `.ci/`.

`MAX_MATCH_STEPS` counts `match_seq()` entries only (`re.c:1625`), so it misses
the glyph loop in `match_atom()` (`re.c:1440-1446`), the `group_end()` /
`find_branch()` walks (`re.c:1326-1361`), the `save_spans()`/`restore_spans()`
`memcpy`s, and the start-position sweep in `re_matchp_internal()`
(`re.c:1680-1691`). Count each, and document in `re.h` what one budget unit
means so 2,000,000 is an honest number.

Move the continuation chain (`re_cont`, `re.c:1279-1287`) off the C stack onto
an explicit bounded work stack sized from `re_exec_options` and caller storage.
That removes dependence on host thread stack size and makes `max_depth`
portable — today's 4096-frame comment ("comfortably under a megabyte",
`re.c:91-96`) is compiler- and ABI-dependent and unmeasured.

Tests: `-fstack-usage` on `match_seq`/`match_seq_body`/`match_atom` across the
GCC and Clang configurations `.ci/` uses, recorded in the commit message;
adversarial patterns on a pthread with a 256 KiB stack; `tests/test2.c`'s sweep
with `RE_TEST2_MAX_BYTES` restored to its full range once the stack no longer
grows with the subject; every work-budget exit asserted `TOO_COMPLEX`.

## Phase 7 — Resolve structure at compile time

Depends on phases 2 and 3, because it changes the node layout. The matcher
rescans structure on every step: `group_end()` walks to the matching
`GROUPEND`, `atom_end()` calls it per atom, and `find_branch()` calls
`atom_end()` per node — for every `match_seq_body()` entry. `group_size` exists
but is 8 bits (`re.c:145`) and deliberately distrusted (`re.c:1323-1325`).

Widen it and store resolved group end, branch alternatives, continuation after
a group, min/max repetition and capture slot at compile time. This layout
change moves `getsize()`'s payload table (`re.c:253-276`), `re_compare()`,
`re_string()`, the `CPROVER` block and the three `test_compile.c` layout reads
at once; land it as one commit. Then compile per-program metadata — nullable,
anchored, possible first codepoint / ASCII byte set, required literal prefix
when safely derivable — and use it to skip impossible unanchored start
positions in `re_matchp_internal()` while preserving UTF-8 glyph stepping and
the ASCII-only case folding (`fold_cp()`, `re.c:1046-1048`).

## Phase 8 — Benchmarks and stronger fuzz properties

Benchmarks, recorded as JSON or CSV in the submodule: compile time, nodes and
bytes for nested groups and alternations; per-match subject visits, steps,
backtracks, capture snapshots; matches and failures on 1 KiB – 10 MiB subjects;
pathological near-match patterns (`tests/test2.c`'s `.+nonexisting.+`); grouped
intervals around 256, 2047 and 65535. `fuzz/fuzz_regex.c` currently only checks
for crashes; add the properties that the compiler never writes past caller
storage, the required-size retry succeeds, spans are ordered and in bounds,
repeated execution is deterministic, no match begins before `start_offset`,
`TOO_COMPLEX` is distinct from `NO_MATCH`, iteration makes progress, and
concurrent executions share no writes.

**Seeds — two encodings, do not mix them.**

- Submodule `fuzz/seeds/regex`: first byte selects a 1..64 byte `small_buf`
  size; the rest splits at the first `\n` into pattern and text.
- kg root `test/fuzz-seeds/regex` (15 tracked): encoding at
  `test/fuzz_regex.c:8-13` — bit 0 of the first byte is `KG_REGEX_ICASE` and
  `1 + (data[0] >> 1) % (size - 1)` is the split, so a first byte of
  `2*strlen(pattern)` puts the split at the boundary with ICASE off and `+1`
  with it on; the text must be non-empty. `make fuzz-regex-seed` copies them
  into the gitignored corpus, `make fuzz-regex-seed-replay` replays each.

Seed every fixed defect into whichever corpus can express it, in that corpus's
encoding: the phase-2 repro `\(a\)\xZ\(b\)`, each phase-3 malformed pattern,
the phase-5 boundary counts. kg root CI gives the regex fuzzer 50 runs
(`Makefile:247-248` via `.ci/ci-09-fuzz-smoke.sh`) — that is seed replay, so
long campaigns run out of band.

## Conditional architecture decision: new VM

Only after phases 1–8 and measurement, write a separate design comparing the
current backtracker with an explicit stack and resolved jumps against a
Thompson/Pike NFA: capture and greedy-priority semantics under each,
backreferences and non-greedy repetition as future features, code-size and
memory budgets. Do not promise linear time while retaining unrestricted
backtracking, and do not replace the engine merely because worst-case
backtracking is theoretically possible — measure real workloads first.

## Documentation

`re.h`'s header comment is the dialect specification: accepted subset,
invalid-pattern behavior, the invalid-`\x` decision and budget units belong
there, with `README.md` §"Supported regex-operators" agreeing verbatim. Also
update `fuzz/fuzz_regex.c`'s input-format comment when the harness changes, and
`fe/fex_re.c`'s mapping after the fe pin moves if `re_status` grew. kg docs
only for user-visible status; the wrapper is Plan 03.

One commit per phase, in phase order. Each engine commit passes tiny-regex-c's
own CI before fe's gitlink moves, and fe's `make check` before kg's does.

## Acceptance

```sh
make -C fe/tiny-regex-c check          # 3 xfails expected until phase 2
make -C fe/tiny-regex-c fuzz-smoke
(cd fe/tiny-regex-c && .ci/run-ci-steps.sh)
make -C fe check
make check
REGEX_DIFF_CASES=200000 make check-regex-differential
make fuzz-regex-seed-replay
.ci/run-ci-steps.sh
.ci/ci-08-with-lisp-0.sh
```

`utils/regex_oracle.el` converts Emacs' character offsets to bytes
(`kg-regex-oracle-byte-offset`, `:23-25`); both sides of the differential
report BYTE offsets. Do not remove that conversion while widening the
generator.
