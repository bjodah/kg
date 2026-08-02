# Plan 06 sub-plans — Runtime and Lisp extensibility

Nine phases, grouped into four sub-plans by dependency and natural
delivery order.  Each sub-plan is a self-contained document that names
its entry criteria, concrete tasks, output artifacts, and tests.

## Grouping

| Sub-plan | Phases | Focus | Prerequisites |
|----------|--------|-------|---------------|
| [A](06a-adapter-decomposition-and-direct-calls.md) | 0, 1 | Decompose `src/lisp.c`, add `FeCallWithOptions`, kill the trampoline | None (Plans 01–04 done) |
| [B](06b-runtime-context-editing-and-markers.md)    | 2, 3 | Buffer objects, runtime execution context, editing/search/marker primitives | Sub-plan A |
| [C](06c-unwind-hooks-and-registries.md)            | 4, 5, 6 | Fe unwind, hooks via the event queue, registry-dependent APIs | Sub-plan B; mode/variable registries for Phase 6 |
| [D](06d-processes-packages-and-proofs.md)          | 7, 8 | Bounded process table, package hygiene, proof packages | Sub-plan C (hooks/unwind for filters/sentinels) |

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

## Current state

`src/lisp.c` is 2,207 lines — over 4× the 520-line per-file scc cap.
Fe has no `FeCallWithOptions` yet.  No `src/lisp_*.c` split files, no
hook registry, no mode or variable registry exist.  The command
trampoline (`pending_command` → string eval → `native_run_pending`) is
still the only path into a Lisp-defined command.
