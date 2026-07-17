# kg Phase 0–7 Follow-Up Plan

**Status:** Implemented 2026-07-17

This replaces the superseded follow-up plan.  It contains only findings
verified by the follow-up review; it deliberately does not repeat already-
completed CI, submodule, resize reflow, write-loop, or atexit work.

## Objectives and completion evidence

| ID | Objective | Implementation and focused evidence |
|---|---|---|
| F1 | Make SIGWINCH installation explicit and fallible. | Initialise `sa_mask`, use zero flags, and report an installation failure. Existing native coverage verifies coalescing, errno preservation, and the second no-op poll. |
| F2 | Keep allocation-backed history all-or-nothing. | `undo_push()` links only fully copied records; kill rings allocate before replacing old contents; all audited `len + 1` rectangle allocations use checked arithmetic. |
| F3 | Preserve transactional-load invariants under oversized input or staged OOM. | One staged-row helper checks narrowing, addition, and multiplication; staged rendering restores `running` before returning an error. |
| F4 | Make atomic-save permissions intentional and testable. | New files use `0666 & ~umask`; existing files retain only ordinary permission bits. The code documents inode metadata and directory-sync limitations. Hooks cover fsync, close, and rename failures. |
| F5 | Reject invalid bulk deletion before mutation and compact rows once. | The serialized range is fully validated; multi-row deletion frees the range, performs one array memmove, and reindexes once. Native tests cover newline boundaries and overrun rejection. |
| F6 | Finish cleanup ownership. | Cleanup releases the rectangle kill ring as well as the normal ring and is guarded against repeated invocation. Buffer-slot undo ownership remains a move invariant: stale slot tails must never be freed independently. |
| F7 | Reset reload state when committing a successful load. | `commit_load_result()` clears `disk_changed` before callers copy state back to the active slot. |
| F8 | Keep shutdown single-shot. | The display-OOM path relies on the registered `atexit` chain rather than calling terminal cleanup before `exit()`. |

## Deliberate policy

Atomic replacement creates a new inode. kg preserves ordinary mode bits for
an existing target and obeys the process umask for a new target. It does not
preserve owner, ACLs, extended attributes, hard links, or special mode bits;
it also does not fsync the containing directory. Those constraints are
documented next to the save implementation rather than hidden as an implied
guarantee.

## Validation

Run before review:

```sh
make format-check
make check
make clean && make check WITH_LISP=0
.ci/run-ci-steps.sh
```

Phase 8 keyboard and broader acceptance-test work remains separate; do not
start unrelated refactoring in that area without its behavior tests.
