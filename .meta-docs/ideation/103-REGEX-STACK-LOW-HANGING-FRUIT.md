# Ideas: low-hanging improvements for the kg/fe/tiny-regex-c regex stack

## Context

The first coordinated regex pass is in place: `tiny-regex-c` owns the dialect and capture spans, Fe exposes compiled regex objects and matching, and kg has regexp isearch plus `query-replace-regexp` without depending on `WITH_LISP=1`.

This document lists natural follow-up work that should be small, testable, and useful without changing the overall architecture.

## Guiding Rules

1. Put dialect behavior in `tiny-regex-c`, not in kg or Fe adapters.
2. Test behavior at the lowest layer that owns it.
3. Keep kg editor regex independent of Lisp evaluation.
4. Prefer explicit documented limits over accidental compatibility claims.
5. Treat row-local matching as the default until multi-line regex is a deliberate project.

## Best Next Batch

1. Add edge-case PTY tests for `query-replace-regexp`.

   Coverage to add: zero-length matches, unmatched captures, unknown replacement escapes, tabbed rows where chars/render columns differ, invalid regex at prompt, and read-only buffers. This is the most direct way to protect the newly added editor behavior.

2. Add explicit Fe helpers `re-match` and `re-match?`.

   `compile-re` plus `match-re` is sufficient but verbose for scripts. A one-shot span-returning helper and a predicate helper match the roadmap API shape and should be small wrappers around the existing extension functions.

3. Tighten Fe regex GC-root safety.

   `fex_re.c` builds several Fe objects while allocating more objects. Audit `BuildError`, span-list construction, and regex-object creation for temporary objects that should be protected with `FeSaveGC` / `FeRestoreGC` or roots before another allocation can collect them.

4. Define and test interval edge semantics in `tiny-regex-c`.

   Current interval support is useful, but exact behavior for `\{0\}`, `\{0,n\}`, `\{n,n\}`, invalid ranges, and large counts should be specified in tests and docs. This is a contained parser/matcher cleanup.

5. Improve kg regex status reporting.

   Map engine statuses more precisely in `src/regex.c` and editor messages: bad pattern, too complex, buffer too small, no match. This keeps transient invalid regex typing understandable and makes future ReDoS/backtracking limits visible to users.

6. Add a visible zero-length match cue in kg isearch.

   Zero-length matches currently advance safely, but the UI has no highlight span to show. A minimal status suffix such as `[empty match]` or a cursor-only indication would make behavior less surprising without inventing complex rendering.

7. Move replacement expansion behind a testable helper boundary.

   `query-replace-regexp` replacement expansion is editor-owned policy. It would be easier to unit-test if the expansion helper lived behind a small non-static function or a small test-only seam. Tests should cover `\&`, `\1` through `\9`, missing captures, trailing backslash, and unknown escapes.

8. Update `tiny-regex-c` stringifier expectations.

   `re_string()` still deserves review after the Emacs-like dialect work. Either teach it to print escaped Emacs-style grouping/alternation/interval forms, or document it as a best-effort debug helper not suitable as a canonical pattern serializer.

9. Expand POSIX bracket-class documentation and tests.

   The engine supports several classes. Document the exact subset and add table-style tests for supported and unsupported classes so Fe/kg users do not infer full POSIX regex support.

10. Add regex-focused fuzz seeds.

   Seed corpora should include Emacs-style groups, alternation, intervals, POSIX classes, malformed escapes, nested repeats, and replacement-like strings. This is low risk and improves future fuzzing value immediately.

11. Add a short fuzz smoke gate to CI.

   Keep it bounded and cheap: small `tiny-regex-c`, Fe reader/eval, and kg keypress smoke runs. The goal is not deep fuzzing in CI, just preventing harness link rot and obvious sanitizer regressions.

12. Add clean-clone submodule checks to docs/tests.

   The build now needs `fe/tiny-regex-c` even with `WITH_LISP=0`. Add a small documented verification path for recursive submodule initialization and the intentional error message when the nested engine is missing.

## Slightly Larger But Still Natural

1. Add an engine match-step budget knob.

   The matcher has internal complexity limits. Exposing a configurable budget through the checked API would let Fe and kg report `too-complex` predictably and tune interactive behavior separately from batch scripts.

2. Optimize backward regex search for long rows.

   kg backward search currently finds the last acceptable match by scanning forward. That is simple and correct, but long lines with many matches can be slow. First add a stress test and status benchmark; only then consider smarter iteration.

3. Make regex syntax docs example-driven.

   Add a concise table to kg and Fe docs showing pattern, text, result, and notes. Include bare literal `(` versus escaped `\(` because that is the most likely user confusion.

4. Add M-x discoverability polish.

   Completion already exposes the commands. Help/status text could mention `M-x query-replace-regexp`, `M-C-s`, and `M-C-r` more explicitly without adding new terminal-risky bindings.

5. Reconcile old engine tests that inspect private layout.

   `tiny-regex-c/tests/test_compile.c` still carries xfail-style checks against a stale local `regex_t` layout. Replace private-layout assertions with public API span tests where practical.

## Larger Projects To Defer

1. Multi-line regex in kg.
2. Unicode-aware classes and case folding.
3. Emacs syntax-table-aware word and symbol constructs.
4. Backreferences in matching.
5. Newline-producing regexp replacement that can split and join rows.
6. Full Emacs regexp compatibility claims.

## Suggested First PR Sequence

1. Test-only hardening: kg PTY cases, Fe script cases, tiny-regex interval/POSIX cases.
2. Fe API sugar: `re-match`, `re-match?`, docs, scripts.
3. Engine semantics cleanup: interval zero-counts, `re_string()` dialect update, private-layout test cleanup.
4. kg UI polish: better status messages and zero-length match indication.
5. Fuzz/CI hygiene: richer seed corpora and bounded smoke gates.

## Validation Baseline

For any change in this stack, the expected local checks are:

1. In `fe/tiny-regex-c`: `make check` plus an ASan/UBSan check when matcher code changes.
2. In `fe`: `make check` and `make format-check` when extension code or scripts change.
3. In kg: `make check`, `make check WITH_LISP=0`, `make format-check`, and `.ci/run-ci-steps.sh` before declaring the stack green.
4. For fuzz harness or parser/matcher changes: bounded fuzz campaigns in this order: `tiny-regex-c`, Fe, kg.
