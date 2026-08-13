# DAP pre-merge adversarial review

Date: 2026-08-13

Source: `dap` at `74b0e27`

Target: `stricter-emacs-adherence` at `5a9a373`
Merge base: `5a9a373b1e1ce12a4fb04ad1fde426ae370badf4`

## Verdict

**Do not merge `dap` as-is.** The branch is unusually well documented and its
happy-path test breadth is strong, but the review found ten merge blockers.
The most serious are an unsaved-buffer erasure path, breakpoint state diverging
from the adapter after source edits, a teardown race that can launch a program
after the user asked to end the session, and a cancellation path that can
silently relaunch a previously selected configuration. The CI changes also
make the whole repository privileged and make every live-session DAP acceptance
case blind to the editor's actual exit status.

The comparison is large: 204 files, 39,762 insertions and 3,443 deletions. I
split the review into independent protocol/transport, editor/UI, and
test/build/documentation passes, then re-read and severity-graded their concrete
findings against the code. Passing tests are recorded below; they do not cover
the races and failure transactions described here.

## Tier 1 — merge blockers

### T1.1 — DAP pane creation can erase an ordinary unsaved buffer

**Evidence.** kg deliberately permits `C-x b` to create a buffer with any name
(`README.md:28-32`). DAP pane discovery is name-only: `pane_slot()` adopts any
open buffer named `*dap-stack*`, `*dap-locals*`, and so on
(`src/dap_ui.c:113-132`). When a model pane is repainted, `rb_commit()` calls
`buf_prepare_special_text()` (`src/dap_ui.c:331-354`). Its existing-name path
unconditionally calls `buf_clear_special_text()` (`src/bufmgr.c:2805-2812`),
which replaces the rows and destroys the undo stack (`src/bufmgr.c:2918-2933`).

**Impact.** A user can create `*dap-stack*`, type unsaved text, and then lose it
merely by showing the DAP layout or repainting that pane. This is direct data
loss, not cosmetic special-buffer behavior.

**Required change.** Give special buffers an explicit owner/kind or retain and
validate the exact handle kg created. Refuse a name collision, or create an
unambiguous internal buffer; never promote an arbitrary user buffer by name.
Add a PTY/native regression that creates each reserved pane name with text,
shows/refreshes DAP, and proves the text and undo history remain intact.

### T1.2 — Teardown can race initialization and launch after disconnect

**Evidence.** `dap_session_end()` marks the session ending and sends
`disconnect`/`terminate` (`src/dap_session.c:754-779`) but leaves earlier
requests pending. A later initialize response runs `on_initialize()`, which
unconditionally calls `send_launch()` at `src/dap_session.c:495-516` (line
508). The `ending` guard in `maybe_start_configuration()` is too late to stop
that launch. Configuration continuations have the same defect:
`on_set_breakpoints()` advances at lines 342-364 and
`on_exception_breakpoints()` advances at lines 292-303 without checking
`ending`.

**Impact.** An initialize response racing `dap-disconnect`, `dap-terminate`, or
editor cleanup can start the debuggee after teardown began. In-flight
breakpoint replies can also emit configuration traffic behind the end request.

**Required change.** Make every handshake/configuration continuation terminal
when `s->ending`, or cancel/ignore all non-teardown pending requests atomically
on entry to teardown. Hold each initialize/configuration reply in tests, call
`dap_session_end()`, release the reply, and assert that no launch or subsequent
configuration request goes out.

### T1.3 — Breakpoints move in the editor but not in the running adapter

**Evidence.** The user contract says breakpoints are marker-anchored so edits
above them take them along (`README.md:840-847`). On
`KG_EVENT_BUFFER_CHANGED`/`BROAD_CHANGE`, `refresh_buffer()` updates only the
cached marker line (`src/dap_breakpoint.c:1480-1546`). It never calls
`source_mutated()`, so the desired generation is not advanced and no
`setBreakpoints` request is sent. The exported repair hook
`dap_breakpoint_sync()` (`src/dap_breakpoint.c:1123-1136`) has no callers.

**Impact.** The pane and decoration move to the edited line while the adapter
continues stopping at the old line. The debugger's visible breakpoint table
therefore ceases to describe the program being debugged.

**Required change.** Detect line changes per source during the safe-point event,
bump each affected source once, and use the existing in-flight/generation
machinery to resend it. Extend the current marker-movement tests with a live
fake adapter: insert above a verified breakpoint and assert the next
`setBreakpoints` contains the new line.

### T1.4 — Cancelling `dap-debug` can launch the previous configuration

**Evidence.** The last choice is process-global and persistent
(`src/dap_commands.c:36-44`). `editor_dap_debug()` sets `choice.valid` only
inside the successful chooser branch (`src/dap_commands.c:513-516`), then
launches whenever that old flag is true at lines 518-519. Cancelling a later
chooser therefore leaves the prior valid choice live and launches it. The same
function ignores the result of `current_file()` at line 506, so invoking
`dap-debug` from a no-file buffer can retain the previous project's filename.

**Impact.** C-g/ESC at a launch prompt can start a program the user explicitly
cancelled, and `dap-debug` from a scratch buffer can reopen the previous
project's configuration instead of refusing the operation.

**Required change.** Build a new choice in local storage and commit it only
after file validation, configuration selection, and program prompting all
succeed. A cancelled attempt must not call `dap_launch()`, while the previous
choice may remain only for an explicit restart. Test a successful run followed
by chooser cancellation, invalid input, and invocation from a scratch buffer;
assert no adapter request or build is started in all three cases.

### T1.5 — Closing the DAP client breaks its exactly-once callback contract

**Evidence.** The pending-table API promises that every callback runs exactly
once and explicitly permits a failure callback to submit another request
(`src/dap_client.h:35-42`). `dap_client_close()` calls `pending_fail_all()`
before setting `c->dead` (`src/dap_client.c:1001-1013`). Each slot is cleared
before its callback runs (`src/dap_client.c:321-336`), so a re-entrant callback
can reuse a slot the loop already passed, successfully write a request, and
register work. Close then frees the client without failing that new callback.

**Impact.** This loses a callback, leaks its context, and emits wire traffic
during close, violating the foundational ownership invariant used throughout
the async protocol stack.

**Required change.** Set the dead flag and stable death reason before failing
pending callbacks. A nested request must fail synchronously via
`request_refuse()`. Add a close callback that submits another request and
assert nested callback count one, no new frame, and no pending work.

### T1.6 — Stale breakpoint-pane rows can operate on another file

**Evidence.** The breakpoint API says a source index is stable only until the
next mutation (`src/dap_breakpoint.h:175-177`). The pane nevertheless stores
that compacted index as row identity (`src/dap_ui.c:867-896`). Breakpoint rows
are exempt from `row_is_current()` freshness checks
(`src/dap_ui.c:1184-1197`), and RET/d/D later resolve the saved index
(`src/dap_ui.c:1234-1245,1318-1359`). Adapter events can remove the last
breakpoint in a source and compact the vector before the debounced repaint.

**Impact.** During that interval, a row for file A can resolve to file B. If B
has a breakpoint on the same line, RET visits it and d/D deletes or toggles the
wrong breakpoint rather than reporting the stale row.

**Required change.** Store a stable source epoch/path identity plus the
breakpoint's local id, not a vector index and line. Re-resolve both immediately
before acting. Test removal/compaction between render and RET/d/D with two
files sharing a breakpoint line.

### T1.7 — `dap-until` can delete an existing permanent breakpoint

**Evidence.** Temporary arming sets `opts.temporary` and routes through the
ordinary add/update path (`src/dap_breakpoint.c:1334-1348`). If the target line
already has a breakpoint, `bp_put()` updates that existing object
(`src/dap_breakpoint.c:822-847`), and `bp_apply_options()` overwrites its
`temporary` flag (`src/dap_breakpoint.c:805-815`). A hit then forgets the whole
breakpoint (`src/dap_breakpoint.c:1372-1383`); session cleanup also deletes
every object carrying the temporary bit (`src/dap_breakpoint.c:1153-1161`).

**Impact.** Running until a line where the user already placed a permanent
breakpoint silently converts it to one-shot state and deletes it on hit or
session end. A motion command should not destroy persistent editor state.

**Required change.** Represent the run-until waiter separately when it attaches
to an existing permanent breakpoint. Completing or cancelling the waiter must
preserve that breakpoint's options and enabled state. Test verify→hit,
verification failure, cancellation, and session end with an existing
permanent breakpoint, asserting it remains synchronized throughout.

### T1.8 — A failed `goto` wedges a stopped session in RESUMING

**Evidence.** `send_goto()` invalidates the epoch, clears the stop view, and
calls `dap_session_begin_resume()` before sending
(`src/dap_exec.c:1059-1083`). Its response uses `on_simple()`, which only
reports failure and frees context (`src/dap_exec.c:1021-1032`). A normal step
failure has dedicated rollback to STOPPED and refetches the stack
(`src/dap_exec.c:951-973`), but `goto` does not. Restart similarly clears the
view before a capable adapter's response and uses `on_simple()`
(`src/dap_exec.c:1193-1213`). Allocation/send failure after the early state
transition has the same shape.

**Impact.** If an adapter advertises goto but rejects one target, the debuggee
remains stopped while kg believes it is resuming. The panes are blank and all
subsequent step/continue commands are refused until unrelated adapter news or
session restart. A rejected restart leaves STOPPED with its stop model erased.

**Required change.** Make resume-implying requests transactional. Use a
goto/restart callback that restores STOPPED and refetches on failure, and do
not commit the phase/epoch transition until request allocation and queueing
can succeed. Test `success:false`, timeout, and local send/allocation failure,
then prove another execution command works.

### T1.9 — tmux DAP tests discard kg's real exit status

**Evidence.** `run_editor_tmux()` hardcodes exit code zero
(`utils/pty_accept.py:1024`); the evaluator documents that tmux cannot apply the
normal sanitizer/nonzero-exit gate (`utils/pty_accept.py:1155-1168`). Every
DAP acceptance case that actually starts a session uses `backend: tmux`. A
review probe ran a child that exited 7 through this path and received
`reported_exit_code=0`, `error=None`.

**Impact.** A teardown assertion, crash, ASan/LSan report, or Valgrind failure
after the saved-file/screen assertions can leave the case green. This weakens
the newly added Valgrind lane precisely around adapter shutdown and cleanup.

**Required change.** Run the tmux pane command through a wrapper that records
its real status in a sidecar, read it after the session ends, and apply the
same default nonzero-status rule as pexpect. Add a harness self-test for exit
7 and one for a post-trailer sanitizer-style failure.

### T1.10 — Hosted CI runs the entire repository privileged

**Evidence.** `.woodpecker.yaml:10` sets `privileged: true` on the single step
that runs every repository script. Commit `0dd6a62` adds only that line, is
titled `priveleged`, and records no need or threat-boundary rationale.

**Impact.** Every pushed revision, fake server, fuzzer, debuggee, build script,
and test runs with the container's full device/capability access. This is a
material and unnecessary expansion of CI blast radius.

**Required change.** Remove privileged mode unless a measured requirement
proves it necessary. If a real debugger needs ptrace privileges, isolate only
those acceptance cases in a separate step with the narrowest supported
capability/seccomp adjustment; keep builds, static analysis, fake adapters,
fuzzers, and unit tests unprivileged. Document the measured reason.

## Tier 2 — major correctness and release-confidence issues

These should be fixed before calling the DAP merge complete. If any are
deferred, create an owned issue with the stated regression test as its
acceptance criterion.

### T2.1 — Malformed DAP tokens can correlate to real pending requests

`id_int32()` truncates JSON numbers through `kg_json_int()`
(`src/dap_client.c:155-176`, `src/json.c:783-800`), so
`request_seq: 1.9` becomes 1. Its string branch ignores decoded JSON length,
so `"1\u0000junk"` also becomes 1. Lengthless `strcmp()` similarly accepts
`"launch\u0000junk"` as `"launch"` for command/type dispatch
(`src/dap_client.c:818-878`). Malformed adapter traffic can therefore retire
a legitimate pending slot and run its state-changing callback. Require exact
integral values, full length-aware numeric-string consumption, and
length-aware token equality; add fractional and embedded-NUL correlation
tests.

### T2.2 — Several advertised pane interactions are not implemented safely

- README says RET selects a thread (`README.md:823-825`), but the thread case
  merely prints that kg is already looking at it (`src/dap_ui.c:1299-1302`).
  Add an execution-layer thread-selection API, refetch that thread's stack,
  and test selection of a non-current stopped thread.
- A stop reached while point is in any DAP pane deliberately suppresses source
  navigation (`src/dap_commands.c:158-173`) rather than updating a source
  window without stealing point. `source_buffer()` also chooses the first
  arbitrary non-pane buffer when F12 is invoked from a pane
  (`src/dap_ui.c:1017-1044`). Track the layout's source window explicitly and
  update it on stops/frame selection.
- `layout_restore()` ignores restore failure, marks the layout inactive, and
  destroys the only saved snapshot (`src/dap_ui.c:1109-1117`). Retain the
  snapshot/state on failure and report it. Test terminal shrink and event
  reservation failure.
- Client-side relaunch unconditionally reapplies the layout after
  `dap_launch(false)` (`src/dap_commands.c:205-214`). A missing configuration
  or failed spawn can therefore reopen six panes with no session. Make launch
  return success and apply layout only after a session actually starts.

### T2.3 — The visible truncation guarantee fails at the row cap

`dap_ui.h:98-101` promises a visible “N omitted” line whenever an
adapter-controlled collection is cut. `rb_line()` refuses all rows once
`DAP_UI_ROWS_MAX` is reached (`src/dap_ui.c:237-256`), and each renderer then
tries to append its omission marker through that same full path
(`src/dap_ui.c:607-610,821-824,859-862,916-919`). Reserve one row for the
marker or replace the last data row; test maximum threads, frames, variables,
and breakpoints.

### T2.4 — DAP acceptance has false assertions in addition to the exit blind spot

- `dap-milestone` waits for/evaluates `42`, but `42` is already present in the
  locals pane before evaluation (`test/pty/dap-milestone.yaml:63-64,109-133`),
  and `wait_change_tmux()` accepts text already present in its starting frame
  (`utils/pty_accept.py:454-468`). Use a sentinel absent from all source and
  panes, and add native success/error/stale/out-of-order evaluate tests.
- `dap-layout-restores-and-disconnects` claims F8 proves the DAP map was
  removed (`test/pty/dap-layout-restores-and-disconnects.yaml:57-65`), but
  both unbound F8 and still-bound `dap-continue` leave the file unchanged.
  Bind a globally observable F8 or add a native inactive→active→inactive map
  test.
- `test_step_refused_without_a_stopped_thread()` contains
  `CHECK(!dap_exec_pause() || true)` (`test/test_dap_exec.c:539-552`), which
  is tautologically true. Replace it with an explicit pause policy, arguments,
  and refusal-response assertion.

### T2.5 — Hosted CI does not require its claimed real-adapter coverage

The real lldb, debugpy, delve/Go, and nbcode cases declare tool requirements,
but missing tools are warnings/SKIPs unless `--require-tools` is supplied
(`utils/pty_accept.py:1061-1071,1215-1256,1335-1345`). The hosted workflow sets
only `CI_EXPENSIVE=1`; `PTY_ACCEPT_ARGS` stays empty. An image regression can
therefore remove every real adapter while CI remains green. Add a dedicated
required-tools lane or set `PTY_ACCEPT_ARGS=--require-tools` in the hosted DAP
configuration.

The preflight must first be fixed to filter by `requires_feature`: it currently
scans every case before feature mismatch is applied, so a `WITH_DAP=0`
required-tools run demands adapters for cases that cannot run
(`utils/pty_accept.py:1061-1062,1224-1249`).

### T2.6 — The shared framing header bound is bypassable

`inbox_take_message()` enforces `FRAMED_IO_MAX_HEADER_BYTES` only while no
delimiter has been found (`src/framed_io.c:438-455`). If the read that crosses
8192 bytes also carries the final blank line, the complete oversized header is
accepted. The overshoot is bounded by the 16 KiB read chunk, but the documented
hard bound is false for both DAP and LSP. Reject `head` above the limit before
parsing and add a complete 8193-byte-header test.

### T2.7 — The no-slack complexity ratchet has seven points of slack

The Makefile states that `SCC_COMPLEXITY_MAX` equals measured actual with no
slack (`Makefile:716-729`), but the ceiling is 10552 and current measured
complexity is 10545. Lower it to 10545 and record the required old/new-limit,
per-file, and pmccabe proof in the fix commit.

### T2.8 — User documentation contradicts the implementation

`README.md:761-763` says two adapters ship built in, while `README.md:877-878`
correctly says four. `doc/kg.1:3991-4002` says three, then lists four. State
four consistently and distinguish three spawned command adapters from the
nbcode LSP-sibling adapter. Also reconcile the historical plans:
`doc/plans/2026-08-11-dap.md` still says Java is out of v1 and nothing in the
tree references DAP, while the Java plan still labels the shipped nbcode path
“planned.” Add clear historical/status banners if preserving original plans.

## Tier 3 — hardening and maintenance recommendations

1. **Avoid undefined arithmetic in strict JSON parsing.**
   `check_unique_keys()` forms `p->stack + base` before discovering the empty
   object has zero members (`src/json.c:553-565`). Calculate `n` first and
   return for zero before deriving the pointer; test `{}` and nested empty
   objects under UBSan.

2. **Do not silently alter adapter-owned identifiers.** Exception filter ids
   and reverse-request commands are truncated to 31 bytes
   (`src/dap_client.c:481-512,709-737`; `src/dap_client.h:207-213`) and later
   sent back changed. Length-carry them or reject an over-limit value
   explicitly, and test long valid identifiers.

3. **Repair transcript recreation bookkeeping.** If a transcript is killed
   and explicitly recreated by name before new output, `pane_slot()` replaces
   the stale handle before `transcript_slot()` can observe the kill
   (`src/dap_ui.c:410-428,997-1012`). The old 4 MiB budget survives and no
   restart notice is emitted. Detect ownership/handle death before name lookup.

4. **Finish DAP fuzzer bookkeeping.** A stale `test/fuzz_dap_dispatch` is not
   removed by `make clean WITH_DAP=0`; the three DAP fuzz targets are missing
   from `.PHONY`, and `doc/FUZZING.md` does not describe them
   (`Makefile:575-588,1832-1845`). Clean it unconditionally, declare the
   targets, and document input/corpus/replay commands.

5. **Exhaustively gate the disabled facade.** The `WITH_DAP=0` PTY test samples
   only two commands while documentation promises the same diagnostic for
   every `dap-*` command. Add a table-driven disabled-build unit test covering
   the complete command-table prefix and no buffer mutation.

## Validation performed

- `make check`: 59/59 native tests passed; PTY acceptance 574 passed, 5
  skipped, 0 failed out of 579. On this box the real lldb-dap, debugpy,
  delve/Go, delve build-failure, and nbcode Java cases all passed; the five
  skips were feature-inapplicable/tree-sitter cases, not those adapters.
- `.ci/ci-16-with-dap-0.sh`: passed, including the disabled-DAP facade and
  build-axis checks.
- A separate real-adapter run with `--require-tools`: 5/5 passed.
- Focused native tests for framed I/O, DAP transport/client/session,
  breakpoints, execution, commands, UI and Java, plus LSP transport/client/JSON
  and window management: passed.
- `make complexity-check`: passed at 10545/10552, exposing the seven-point
  policy drift. `make pmccabe-check`: passed, 2803/2803 symbols and maximum
  function complexity 67/110.
- `make docs-check`, `make check-pty-tokens`, and `make gateway-check`: passed;
  the gateway census observed its allowed 47 raw-mutation sites.
- Seven deterministic DAP PTY cases and all five real-adapter cases were also
  run separately and passed. All 24 tracked DAP-dispatch seeds had complete,
  length-consistent framing.
- A fresh coverage run and the complete `.ci/run-ci-steps.sh` matrix were not
  run for this review. Existing `coverage/src.info` predates the branch and is
  not valid evidence for the DAP files.

The green baseline is meaningful evidence for ordinary paths. It does not
invalidate the findings above: the blockers are missing negative-path tests,
explicit state/ownership contradictions, or failures hidden by the tmux exit
status behavior itself.

## Recommended merge sequence

1. Remove privileged whole-suite CI and make tmux propagate the real status.
2. Fix the data-loss, teardown, stale-choice, breakpoint-resync, run-until,
   callback-close, stale-pane-row, and goto/restart transaction bugs; add each
   negative test beside its owning native/PTY layer.
3. Re-run real adapters with required tools, the full default and `WITH_DAP=0`
   suites, sanitizers, Valgrind, static analysis, complexity/pmccabe/gateway,
   and fresh coverage.
4. Fix or explicitly track Tier 2 items, bank the complexity reduction, and
   reconcile the user documentation before merging.
