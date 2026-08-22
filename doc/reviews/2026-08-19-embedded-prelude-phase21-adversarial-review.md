# Adversarial review: embedded prelude and Phase 21 instrumentation

Date: 2026-08-19

## Scope and verdict

This review covers the complete changes introduced by these two merge-base
ranges:

| Tree | Baseline | Reviewed head |
| --- | --- | --- |
| kg | `stricter-emacs-adherence` (`25eb391aad13`) | `23e7772f750e` |
| fe | `analyzers-etc` (`3eedbf36419e`) | `dd35a2b934c5` |

The review concentrated on executable behavior, generated-prelude boundaries,
GC reachability, native/evaluator re-entry, counting/non-counting build
composition, and whether the new performance evidence measures what its labels
claim. The large design documents were used as assertions to falsify against
the code and experiments, rather than treated as evidence by themselves.

**Verdict: do not merge in the current state.** There is one high-severity
semantic regression, four medium-severity correctness or measurement defects,
and two low-severity test/tool defects. The normal suites are green, but the
focused probes below exercise transitions those suites do not cover.

| Severity | Count | Findings |
| --- | ---: | --- |
| High | 1 | A captured deferred stub can overwrite a later user definition |
| Medium | 4 | Native primitive re-entry bypasses handlers; two advertised counting targets do not link; benchmark answers are checked in the wrong process; `context-open-close` excludes close |
| Low | 2 | `--names FILE` is unusable; the public-GC test does not isolate its root |

## Findings

### 1. High: calling a captured deferred stub overwrites a later definition

Affected code: `lisp/prelude.el:1306-1332`,
`src/lisp_prelude.c:247-328`, and
`utils/prelude_deferred_names.txt:78`.

The stub factory returns this closure:

```elisp
(lambda (&rest args)
  (internal--force-deferred name)
  (apply (symbol-function name) args))
```

It unconditionally evaluates the saved `defalias` form and then looks up the
symbol's current function cell. That is not equivalent to capturing the eager
function value, despite the compatibility claim at `lisp/prelude.el:1306-1310`.
If code captures the stub and then redefines the symbol, calling the captured
value installs the old deferred definition over the new one.

Reproduction (`mapcar` is in the deferred policy list):

```elisp
(let ((saved (symbol-function 'mapcar)))
  (defalias 'mapcar (lambda (function sequence) 'replacement))
  (list (funcall saved '1+ '(1 2))
        (mapcar '1+ '(1 2))))
```

Observed:

```text
$ ./test/kgbatch -p /tmp/deferred-redefine.el
/tmp/deferred-redefine.el: ((2 3) (2 3))

$ emacs -Q --batch --eval '<the same expression, printed with prin1>'
((2 3) replacement)
```

The first value should use the function that was captured; the second should
use the later definition. kg instead silently destroys the later definition.
The baseline prelude eagerly installs `mapcar`, so `symbol-function` returns
the real closure there rather than a mutable forwarding stub.

There is a second violation of captured-function semantics: a retained stub
consults `(symbol-function name)` on every call. After its first call forces
the old definition, a later redefinition changes what that already-captured
value invokes. An eagerly captured closure does not behave that way.

This can break init files and packages that save a function value before
advising or replacing the symbol. It is particularly dangerous because the
failure mutates global function state and does not raise an error.

Recommendation:

- Give a stub stable access to the real closure it represents.
- When forcing, do not overwrite a function cell that no longer contains that
  exact stub. If evaluating the saved `defalias` is retained as the loader,
  capture the resulting real closure and restore an intervening replacement.
- Add a regression that captures a deferred function, redefines the symbol
  before first call, calls the captured value twice across another
  redefinition, and checks both the captured result and current function cell.
  The existing first-call PTY case covers only the uncomplicated path.

### 2. Medium: nested primitive evaluation escapes the enclosing `condition-case`

Affected code: `src/lisp_cmd.c:845-874` and `src/lisp_cmd.c:1027-1046`.

`lisp_call_primitive()` invokes `put` and `setcdr` by constructing a form and
calling plain `FeEvaluateWithOptions()` from inside a native function. The
repository already documents at `src/lisp_core.c:734-744` that this exact
nested-evaluation shape transfers a completion past lexically enclosing Lisp
handlers to kg's outer host barrier. That code uses `FeTryCallWithOptions()`
and `FeResignal()` specifically to avoid the defect; the new helper does not.

A direct differential probe demonstrates the change from the old Lisp helper:

```elisp
;; New native helper.
(condition-case e
    (internal--variable-doc-put 1 "x")
  (error (list 'caught (car e))))
```

```text
$ ./test/kgbatch -p /tmp/native-put.el
/tmp/native-put.el: eval:1: expected symbol, got integer
$ echo $?
1
```

Defining the baseline body under another name and running the same call gives:

```elisp
(defalias 'direct-variable-doc-put
  (lambda (name doc)
    (if doc (put name 'variable-documentation doc))
    doc))
(condition-case e
    (direct-variable-doc-put 1 "x")
  (error (list 'caught (car e))))
```

```text
/tmp/direct-put.el: (caught wrong-type-argument)
exit 0
```

The defect also reaches an ordinary `defvar` path if `put` has been replaced:

```elisp
(defalias 'put (lambda (&rest args) (error "replacement put failed")))
(condition-case e
    (defvar review-probe-value 2 "doc")
  (error (list 'caught (car e))))
```

kg exits 1 with the raw `replacement put failed` error instead of returning
the handler's value. Before this change, `internal--variable-doc-put` called
`put` directly in the enclosing evaluator run, so the handler caught it.

Recommendation: use the same protected-call-and-resignal pattern already
centralized for `raise_signal_form()`, or add a safe Fe API for invoking a
primitive without starting an unprotected nested evaluator run. Add tests for
an error raised by both native callers while inside `condition-case`.

### 3. Medium: both new counting prelude probe targets fail to link

Affected code: `Makefile:1061-1074` and `Makefile:1083-1096`.

The new targets compile kg objects with `FE_PERF_COUNTERS=1`, so
`test/perfobj/lisp_core.o` references `FePerfWriteJson()`. Their prerequisites
then link ordinary `$(FE_OBJ)` rather than `$(PERF_FE_OBJ)`. The ordinary list
intentionally omits `fe_perf.o`; the counting list at `Makefile:65-76` includes
it.

Both advertised targets fail deterministically:

```text
$ make test/perfobj/prelude_read_eval_split
/usr/bin/ld: test/perfobj/lisp_core.o: in function `kg_lisp_perf_dump_fe_json':
lisp_core.c:(.text+0x1d01): undefined reference to `FePerfWriteJson'
collect2: error: ld returned 1 exit status
make: *** [Makefile:1074: test/perfobj/prelude_read_eval_split] Error 1

$ make test/perfobj/prelude_gc_probe
/usr/bin/ld: test/perfobj/lisp_core.o: in function `kg_lisp_perf_dump_fe_json':
lisp_core.c:(.text+0x1d01): undefined reference to `FePerfWriteJson'
collect2: error: ld returned 1 exit status
make: *** [Makefile:1096: test/perfobj/prelude_gc_probe] Error 1
```

`make check` does not build either target, which is why the full suite remains
green.

Recommendation: use `$(PERF_FE_OBJ)` in both link prerequisite lists and add a
cheap link/run gate for the targets whose measurements are cited by the plan.

### 4. Medium: benchmark answer validation is disconnected from the measured run

Affected code: `utils/bench.py:484-486`, `utils/bench.py:720-747`, and
`utils/bench.py:973-1042`.

`bench_case()` first drives the counting kg in a PTY. After all measured runs
have exited, it starts a separate `test/kgbatch` process and evaluates the
source there. Therefore an answer of `3` proves only that kgbatch can evaluate
`(+ 1 2)`; it does not prove that the measured PTY executed the expression.

The `lisp-command-latency` case makes this a complete false positive. Its only
counter assertion is `lisp_arena_total_slots > 0`, which is already true after
prelude startup. Calling `bench_case()` with an exit-only key script, but the
real case's answer oracle, succeeds:

```python
from utils import bench

r = bench.bench_case(
    "test/perfobj/kg", "review-no-eval", None,
    ["\x18\x03"], 1, 24, 80, 10,
    assert_gt={"lisp_arena_total_slots": 0},
    kgbatch="test/kgbatch", answer=("(+ 1 2)", "3"))
print("answer=", r["answer"])
print("total_slots=", r["counters"]["lisp_arena_total_slots"])
```

Observed:

```text
answer= 3
total_slots= 56147
```

No `M-:` command or arithmetic expression ran in the measured process, yet
both validations passed. As a result, a broken key sequence or command path
can turn this case into a startup benchmark while retaining a plausible name
and answer.

Recommendation: obtain the answer from the same process whose time and
counters are recorded. A structured side channel is preferable to matching
terminal bytes, since the expression itself is echoed. At minimum, add a
counter that proves the minibuffer evaluation completed and have a self-test
replace the key sequence with exit-only keys and require rejection.

### 5. Medium: `context-open-close` measures open but excludes close's full GC

Affected code: `fe/perf_workloads.c:252-295` and
`fe/perf_workloads.c:983-1031`.

The new workload is named and documented as a bare context open/close. In
`RunOne()`, however, the timer is stopped at line 1017 and counters/stats are
snapshotted at lines 1018-1022. `FeCloseContext()` is not called until line
1029. This is not an inconsequential destructor: `fe/fe.c:3628-3642` clears
the roots and runs `CollectGarbage()`, sweeping the arena.

The workload itself proves that close is outside the counter region by
asserting `FePerfGcCollection == 0` at `fe/perf_workloads.c:269`; a real close
increments that counter. The observed report likewise says:

```text
context-open-close ... gcs 0 ... 0.000121 seconds
```

Thus both the wall time and deterministic counters describe context open
only. The `opened` answer and `FeIsFBound` checks also occur after timing but
before close, so they do not validate the claimed close half either.

Recommendation: either rename the workload and documentation to
`context-open`, or deliberately include `FeCloseContext()` before stopping
the timer and taking the counter snapshot. If open-time validation would
pollute that region, validate a separate context or split open and close into
two named workloads.

### 6. Low: the documented `embed_lisp_split.py --names FILE` option is unusable

Affected code: `utils/embed_lisp_split.py:206-215`.

The parser removes arguments beginning with `--`, but not the following option
value. Supplying the documented option leaves four positional entries, so the
length check prints usage and returns 2. Omitting the value instead indexes
past `argv` and raises `IndexError`.

Observed:

```text
$ python3 utils/embed_lisp_split.py lisp/prelude.el /tmp/eager.inc \
    /tmp/deferred.inc --names utils/prelude_deferred_names.txt
usage: utils/embed_lisp_split.py <lisp/prelude.el> <eager.inc> \
       <deferred.inc> [--names FILE]
$ echo $?
2
```

The Makefile's default invocation does not expose this because it relies on
the default names path.

Recommendation: parse the option and its operand as a pair, preferably with
`argparse`, and add success, missing-operand, and unexpected-option tests.

### 7. Low: the public collection test does not prove root-list reachability

Affected code: `fe/test_api.c:9726-9773`.

The test says the kept pair is reachable *only* through `root`, but it takes
its GC checkpoint after constructing the integer and pair:

```c
FeObject* const kept =
    FeCons(context, FeMakeInteger(context, 7), FeNil(context));
FeRoot* const root = FeCreateRoot(context, kept);
/* ... */
const size_t gc = FeSaveGC(context);
```

Every `MakeObject()` pushes its result on Fe's GC stack. `FeCreateRoot()` saves
and restores the already-increased stack index; it does not remove the pair or
integer that preceded it. Consequently the pair survives even if its root is
released before collection.

A small host probe constructed the same value, recorded the GC stack before
construction and after root creation, released the root, discarded 64 garbage
pairs, forced collection, and read the kept pair:

```text
gc_stack_delta=2 value_after_released_root=7
```

The current test still proves that `FeCollectGarbage()` runs and reclaims the
64 garbage pairs. It does not prove its stated root-list property and would
miss a regression that stopped marking `ctx->root_list`.

Recommendation: save the GC stack before constructing `kept`, create the
root, then restore to that earlier checkpoint before forcing collection. Add
a complementary release-and-collect case that demonstrates the value becomes
reclaimable once neither root list nor GC stack retains it.

## Verification performed

The following broad gates passed at the reviewed heads:

- kg `make complexity-check pmccabe-check`: exact ratchets passed (`scc`
  total 10643, max file 519; pmccabe baseline 2834/2834 symbols).
- fe `make complexity-check pmccabe-check`: exact ratchets passed (`scc`
  total 867, max file 520; pmccabe total 1285/408 symbols).
- fe `make check`: API tests, GC-stress variants, example host, debug and
  release script corpora all passed.
- fe `make perf-check`: all 19 workloads, counting API tests, example host,
  and counting interpreter script corpus passed.
- kg `make check`: 59/59 native test binaries passed; Lisp oracle reported
  263 matches and the same 20 recorded divergences; PTY acceptance reported
  582 passes, 5 expected skips, and no failures/errors across 587 cases.
- kg generated-prelude, prelude census, forecast, header, documentation,
  Lisp GC-stress, and compatibility-manifest checks passed as part of
  `make check`.
- `git diff --check` passed in both trees.

The full multi-lane `.ci/run-ci-steps.sh` matrix, coverage, fuzzing, and the
expensive valgrind lane were not run for this review. The findings above do not
depend on those omitted lanes: each is established by source-level control
flow plus a deterministic focused reproduction.

## Areas examined without an additional finding

- The ordinary Fe build compiles the performance macros out, while its
  dedicated `perfobj/` rules keep counting core objects separate.
- Allocation-by-final-type correction for string cells is internally
  consistent with the constructors audited, and `make perf-check` exercises
  its stated sum invariant.
- The post-prelude forced collection has explicit roots for the surviving kg
  objects exercised by the GC-stress and census probes.
- The generated eager/deferred includes match `lisp/prelude.el` according to
  the checked generator path; the material semantic issue is the stub's
  runtime identity, not byte drift in the generated arrays.
- The new public `FeCollectGarbage()` wrapper calls the same collector as the
  allocation paths. Finding 7 concerns the adequacy of its root-specific
  regression test, not the wrapper's basic dispatch.
