# Plan 06 sub-plans — Runtime and Lisp extensibility

Nine phases, grouped into four sub-plans by dependency and natural
delivery order, plus two debt sub-plans the phases uncovered.  Each
sub-plan is a self-contained document that names its entry criteria,
concrete tasks, output artifacts, and tests.

## Grouping

| Sub-plan | Phases | Focus | Prerequisites |
|----------|--------|-------|---------------|
| [A](06a-adapter-decomposition-and-direct-calls.md) | 0, 1 | Decompose `src/lisp.c`, add `FeCallWithOptions`, kill the trampoline | None (Plans 01–04 done) |
| [B](06b-runtime-context-editing-and-markers.md)    | 2, 3 | Buffer objects, runtime execution context, editing/search/marker primitives | Sub-plan A |
| [C](06c-unwind-hooks-and-registries.md)            | 4, 5, 6 | Fe unwind, hooks via the event queue, registry-dependent APIs | Sub-plan B; mode/variable registries for Phase 6 |
| [D](06d-processes-packages-and-proofs.md)          | 7, 8 | Bounded process table, package hygiene, proof packages | Sub-plan C (hooks/unwind for filters/sentinels) |
| [E](06e-fe-recursion-depth-bound.md)               | —    | Bound Fe's recursion by depth; closes **ci-05** | None — independent of A–D |
| [F](06f-include-hygiene.md)                        | —    | Apply IWYU across 16 files; closes **ci-06** | None — independent of A–E |

E and F are **debt, not phases.**  Both lanes were already red before
Plan 06 began, and sub-plan D wrote them down rather than fixing them
because neither was priced in a phase's budget row.  They are independent
of each other and of A–D, so they can run in either order or in parallel
— in separate worktrees, since `.ci/run-ci-steps.sh` takes a per-tree
lock.

E is the one with substance behind it: a stack overflow is neither the
step budget nor `C-g`, so a debug or sanitizer build crashes where a
release build raises a clean error.  F is mechanical, with one finding
that must be refused rather than applied.

## Prerequisite status (2026-08-02)

All Plan 01–04 prerequisites for Phases 0–5, 7–8 are met:

- Plan 01 (command identity & keymaps): **complete** — layered maps,
  command IDs, descriptors, introspection, runtime define/remove.
- Plan 02 (edit gateway): **Phases 0–4 complete** — `kg_buffer_replace()`
  is the single publication seam; Phase 5 (legacy retire) is deferred and
  not blocking.
- Plan 03 (markers, decorations, events): **complete** — marker handles,
  decorations, bounded typed event queue, safe-point drain.
- Plan 04 (window handles & session lifecycle): **Phases 0–3 complete** —
  generation-checked buffer/window handles, slot table, view events.
  Phase 4 (session rename) deferred.

Phase 6 alone has two missing prerequisites that are not in Plans 01–04:
a mode registry and a typed-variable registry.  Those are external to
this plan and Phase 6 gates on them; the sub-plan documents it.

## Current state (2026-08-03)

**Sub-plans A–D are complete.**  All nine phases landed; the tree is at
`scc` 5401 against the 5500 cap granted by the follow-ups README's Plan 06
Decision, so the budget held without a raise.

`src/lisp.c` is gone, split into twelve `src/lisp_*.c` modules.  Fe has
`FeCallWithOptions`, `unwind-protect` and `FeProtectWithCleanup`.  There
is a hook registry, a bounded process table, generation-checked buffer /
marker / process objects, `provide`/`require`/`featurep` with a bounded
load-path, `auto-fill-mode` as a proof package, and a versioned
`doc/lisp-api.md`.  The command trampoline is gone — `cmd_invoke()` is the
single route in.

Two things Phase 6 wanted are still blocked on registries that do not
exist (a mode registry and a typed-variable registry), so
`define-derived-mode`, `defvar` and `read-string` are documented as
blocked rather than written; sub-plan D's status section lists them
alongside whitespace-mode, conf-mode, docstrings and `describe-function`,
each with what it needs.

Sub-plans E and F are open, and are the only reason the full CI runner is
not green.
