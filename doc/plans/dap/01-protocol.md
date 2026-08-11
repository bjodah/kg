# DAP subplan 01 — feature axis, transport, client, session, breakpoints, execution

Parent: `doc/plans/2026-08-11-dap.md`; prerequisites: subplan 00 stages
A, B (C-F only gate the UI subplan). The measured facts referenced as
"[M-n]" are the parent plan's "Measured protocol facts" list; every one
gets a scripted `fake_dap_adapter.py` mode and a native test.

## Stage 1 — feature axis and facade

`WITH_DAP ?= 1` (no build-time dependency — adapters are found at run
time, same rationale as `WITH_LSP`): Makefile axis + validation +
`DAP_SRCS` + `-DKG_USE_DAP=1`, extending the single feature-stamp *name*
(Makefile:194) — never a second stamp. `kg -V` gains `+dap`/`-dap`
(src/main.c:150-162), which makes `requires_feature: dap` free in PTY
cases. `src/dap.h` is the one editor-facing facade with `WITH_DAP=0`
inline stubs, modeled on src/lsp.h + lsp_core.c, exposing only editor
integration — `dap_init()`, `dap_shutdown()`, `dap_poll()`,
`dap_wait_fds()` — while transport/client/session headers are included
directly by their real C consumers; no editor module carries
`KG_USE_DAP` conditionals. `dap_init()` creates the three keymaps —
`dap-breakpoint` and `dap` (minor), `dap-info` (major) — before init.el
loads and immediately deactivates them (subplan 00-D explains both
halves of that rule). The `WITH_DAP=0` command story follows the
existing disabled-feature precedent in the tree (decide by reading how
`WITH_LSP=0` treats the xref commands, then test whichever it is): the
stub facade must not create maps, subscribe to events, or leave
commands claiming a feature `-V` denies. New CI lane
`.ci/ci-16-with-dap-0.sh` mirroring ci-14 covers all four meaningful
combinations — `WITH_DAP=0`, `WITH_DAP=1 WITH_LSP=0`,
`WITH_DAP=1 WITH_LISP=0`, and `WITH_DAP=0 WITH_LISP=0 WITH_LSP=0` —
ending clean. The runner's glob picks it up.

## Stage 2 — dap_transport

`src/dap_transport.[ch]` composes framed_io (subplan 00-A): stdio child
via `kg_process_spawn_bidi()` (stdin_fd must be -1, stderr never joined
to stdout — the process.h:68-78 contract), or TCP connect for attach.
Owns: reaping its own child poll-driven (`waitpid(WNOHANG)`, no SIGCHLD
— house design) and the half-close + grace-deadline + kill-backstop
end-of-session ladder [M-8]. Half-close is `close(write_fd)` for a pipe
and `shutdown(SHUT_WR)` for a socket, performed only after the outbox
has flushed; the grace deadline starts then. EOF is a terminal
connection edge like any transport failure — the precise rule is that
it is not the edge kg may *wait* for after `disconnect`: the session
must be able to end on the disconnect response while debugpy's adapter
lingers alive (measured), and must also end on EOF alone when an
adapter crashes.

Ownership differs per transport kind and no close path may kill a child
it does not own:

| kind | child owner | protocol fds | close backstop |
| --- | --- | --- | --- |
| stdio | DAP | child stdin/stdout | close, TERM/KILL group, reap |
| tcp attach | none | one socket | shutdown/close only |
| spawn-port (04) | DAP | socket + child log pipes | close socket, drain logs, kill/reap |
| lsp-sibling (03) | LSP | one DAP socket | close DAP socket only |

Conversely every owned child is reaped on *every* failure path: spawn,
announce, connect, initialize, launch, protocol error, user cancel.
Deadlines are defined independently per lifecycle step —
spawn/announce, connect, initialize, ordinary request, graceful
shutdown, kill/reap. Launch/attach is exempt from the ordinary request
deadline [M-1] but the session keeps a cancellable "starting" state
with visible elapsed status — C-g cancellation never depends on a
launch response arriving.

Tests: `test_dap_transport.c` driving `test/fake_dap_adapter.py`'s
framing modes — copied nearly verbatim from the fake LSP server's
(`echo`, `split`, `batch`, `garbage`, `huge-header`, `die`, `truncated`),
since the framing layer is shared and already proven; what is new here is
lifecycle (linger, half-close, EOF-vs-disconnect ordering).

## Stage 3 — dap_client

`src/dap_client.[ch]`: the protocol brain, no editor state.

- **Envelope validation before dispatch**: `type` ∈
  request/response/event; responses require `request_seq`, `success`,
  `command`; reverse requests require `command` and a usable inbound
  `seq` (no correlatable seq → log and continue, never guess); events
  require `event`. Protocol IDs live in range-checked int32 (parent
  plan M-10); out-of-range is a logged protocol error.
- **Two seq spaces** [M-5]: one monotonic client counter starting at 1,
  stamped on requests *and* on responses to reverse requests (spec:
  per-actor, per-message), never emitting zero; responses matched by
  `request_seq` **only** — lldb-dap stamps `seq:0` on everything
  (tolerated inbound, echoed back verbatim in `request_seq` when
  answering its reverse requests); a string `seq` is coerced
  (netcoredbg). Client seq exhaustion (int32) fails the session rather
  than wrapping onto a used value — unreachable in practice, one line
  to state.
- **Pending table** copied from lsp_client's proven shape: flat array,
  slot copied out and cleared before the callback runs, per-request
  `CLOCK_MONOTONIC` deadlines, exactly-once callback on answer,
  deadline or death (src/lsp_client.c:408-455, 554-628;
  src/lsp_client.h:249-276). Launch/attach is the one deadline-exempt
  request [M-1]. Also defined: table-full behavior (send nothing,
  fail the callback locally, once), callback-context ownership on every
  exit path (send failure, timeout, match, duplicate, death, cancel),
  and a `command`-mismatch check — correlate by `request_seq` but
  refuse and log a body whose `command` disagrees with the slot.
- **Bounded dispatch**: parse one frame off the transport borrow (the
  parsed document owns its data, detaching it), dispatch it, continue
  under a per-poll work budget — an unbounded drain-then-dispatch queue
  lets a peer monopolize the poll with up-to-32-MiB documents.
  Suggested starting bounds: 64 pending requests, 64 messages or 4 MiB
  queued (with a one-message path for a legal oversized body), a fixed
  message count per poll so terminal input is never starved. Handlers
  may send/flush; they never recurse into receive/dispatch
  (`in_dispatch` assertion + follow-up-poll flag) — the jsonrpc.el
  reentrancy bug class.
- **Reverse requests**: a dispatch table with a **default arm that sends
  `success:false`** — silence hangs adapters [M-3]; dap-mode's missing
  default clause is the cautionary bug. v1 advertises neither
  `supportsRunInTerminalRequest` nor `supportsStartDebuggingRequest`
  [M-2, M-3].
- **Errors**: `message` → `body.error.format` → synthesized; preserve
  `showUser`/`url`/`urlLabel` when present; only user-safe text reaches
  the echo area [M-6]. A `success:false` response is data, not a
  transport failure.
- **Capabilities**: a mutable struct, merged member-by-member from the
  initialize response *and* `capabilities` events [M-7] — merge means
  member-present-overwrites, **including explicit false** (an event may
  turn a feature off); never OR. Reads both `supportTerminateDebuggee`
  (spec spelling, precedence) and debugpy's
  `supportsTerminateDebuggee`; absent ≠ false (`KG_JSON_NONE`).
  Exception filters are retained as bounded copied records (filter,
  label, description, default); a later event replaces the list only
  if it supplies one. One accessor, `dap_capable(session, CAP_X)`, so
  every gate reads the live set.
- Unknown events and unknown fields are ignored after a debug-level log
  [M-13]. Events before the initialize response (measured: debugpy
  telemetry) are *not* buffered wholesale: telemetry is dropped
  immediately, user output is appended bounded once a destination
  exists, an early `initialized` is remembered as one bool and
  processed after capabilities — a spec violation absorbed in one flag,
  not an arbitrary pre-init event store.
- The honest initialize payload (parent plan) and nothing more.

Tests (`test_dap_client.c`, bounded pump loops, never sleeps — the
test_lsp_client.c:22 discipline): responses out of order; response with
`seq:0`; string seq; event between request and response; reverse request
mid-launch answered with failure; unknown reverse request; duplicate
response; unknown `request_seq`; missing seq; malformed type; capability
merge from event; both error shapes; deadline expiry vs the exempt
launch; death with callbacks pending (all flushed exactly once).

Fuzz: `test/fuzz_dap_dispatch.c` — request/response/event
discrimination, seq handling, arbitrary event bodies, breakpoint and
stack/scopes/variables payloads, through the real client with a null UI.
The frames-level fuzzer is already covered by subplan 00-A's retarget.

## Stage 4 — dap_config and session startup

`src/dap_config.[ch]`: adapter specs + launch configs.

- `struct dap_adapter_spec { name; argv; transport; cwd; }` and
  `struct dap_launch_config { name; adapter; request; arguments_json }`.
  Transport kinds: `stdio` (lldb-dap, debugpy), `tcp` (plain attach),
  `spawn-port` (spawn then scrape a port announce then connect —
  delve, subplan 04) and `lsp-sibling` (endpoint discovered through a
  named LSP session — nbcode, subplan 03); only the first two ship in
  v1. `cwd` is the *adapter's* working directory, distinct from the
  debuggee's `arguments.cwd` — delve resolves programs and writes its
  build artifact relative to it (measured, subplan 04), so
  `kg_spawn_request.directory` is plumbed through adapter spawn from
  the start even though v1's two adapters don't need it.
  Built-ins: `lldb-dap` (stdio, PATH lookup) and `python3 -m
  debugpy.adapter` (stdio — the prototype ran debugpy over stdio
  end-to-end; TCP+autoport becomes necessary only with child sessions).
  kg-side defaults baked into the shipped configs: debugpy gets
  `"console":"internalConsole"`, `"subProcess":false`,
  `"justMyCode":false` [M-2, M-3]; lldb-dap gets nothing magical.
- User config file: `.kg-dap.json` (parent plan, decision 2), one
  versioned root schema settled before coding:

  ```json
  { "version": 1,
    "configurations": [
      { "name": "Python current file",
        "adapter": "debugpy",
        "request": "launch",
        "build": { "command": "make", "cwd": "${workspaceRoot}" },
        "arguments": { "program": "${file}", "cwd": "${workspaceRoot}" } } ] }
  ```

  `adapter` is a built-in spec name *or* an inline object
  (`command`/`args`/`transport`/`cwd`) for custom adapters. Rejected:
  unknown `version`, duplicate configuration names, duplicate JSON
  keys, invalid `request`, NUL-bearing path/command strings, unknown
  transport kinds; unknown *arguments* keys pass through untouched
  (deliberately opaque). Bounds: 256 KiB file, 64 configurations, 32
  argv elements, 4 KiB per ordinary string, a measured
  expanded-arguments bound; every error names the config path and JSON
  parse offset. The config is trusted code, stated in kg(1) — same
  trust story as init.el.
- **Discovery cannot use `lsp_workspace_root()`** — it is LSP-owned
  and absent in `WITH_LSP=0` builds. Nearest-config rule: walk upward
  from the current file's directory to the first `.kg-dap.json`; its
  directory is `${workspaceRoot}`. No file → built-in configs with the
  file's own directory. Bounded walk, symlinks resolved consistently,
  behavior defined for unsaved buffers and nonexistent paths. (A
  shared marker-based root module is a possible later extraction; DAP
  does not link LSP for it.)
- **Substitution is a decoded-string transform, never a textual splice
  into raw JSON** — an env value of `a"b\c` pasted textually produces
  invalid or attacker-shaped JSON. Pipeline: parse + validate the
  config; recursively copy `arguments`, substituting inside decoded
  string values; serialize the copy with the writer (this stage adds
  the missing `kg_json` capabilities: serialize-a-value, finite-number
  output, duplicate-key rejection); hand those bytes to
  `kg_jsonw_raw()` as one value. Semantics preserved exactly: omission
  stays omission, null stays null, floats stay numbers (debugpy errors
  on explicit nulls where it expects omission, dap-python.el:220-236).
  The closed substitution set — `${file}`, `${fileDir}`,
  `${workspaceRoot}`, `${env:NAME}` — expands longest-key-first; an
  **unknown substitution is a configuration error**, not empty+warning
  (`${workspaecRoot}` → "" launches the wrong program), and an unset
  `${env:NAME}` errors too until an explicit default syntax exists.
  Tests: quotes, backslashes, newlines, Unicode, adjacent
  substitutions, recursion/expansion caps.
- Env override hook for tests, per-adapter with **fixed shell-safe
  names**: `KG_DAP_ADAPTER_LLDB`, `KG_DAP_ADAPTER_DEBUGPY` (mirroring
  `KG_LSP_SERVER_<MODE>`, src/lsp_server.h:135-159) — never derived
  from display names that may contain hyphens. The override's shape
  (shell command, as `kg_spawn_request` takes) is documented.
- Built-in launch behavior when no config file exists: Python launches
  `${file}`; **lldb prompts for a program path — never a silent
  `${workspaceRoot}/a.out` guess**; `dap-debug` lists nearest-file
  configs plus applicable built-ins, remembers the last successful
  choice for restart and re-invocation, and refuses duplicate display
  names.
- The optional `"build"` step needs a seam that does not exist:
  `compilation_start()` is private, may prompt, and reports no
  completion. This stage adds and tests
  `compilation_start_programmatic(command, directory, source,
  done_fn, ctx)` with: BUSY returned rather than prompting;
  exactly-once completion callback at the top-level safe point after
  output finalizes; exited/signaled/spawn-failed status;
  cancellation/ignore by generation when the DAP start was abandoned;
  never invoking the callback from inside `compilation_poll()` while a
  prompt is open; documented context ownership through shutdown.
  Ordinary `*compilation*` behavior and diagnostics are preserved —
  dap-mode's `preLaunchTask` lesson: for C++ this is the difference
  between usable and not.

`src/dap_session.[ch]`: launch response and configuration progress are
*independent* (three measured orderings), so a single linear enum cannot
represent them. The session is a coarse phase plus explicit milestones:

```
enum dap_phase { STARTING, ACTIVE, STOPPED, RESUMING, ENDING, DEAD };
bool initialize_done;
bool launch_sent, launch_done, launch_failed;
bool initialized_seen;
bool configuration_started, configuration_done;
```

Transitions are idempotent functions, not assignments scattered among
callbacks; the session also retains **whether it was launched or
attached** — the shutdown matrix reads it.

- initialize first, nothing before its response (spec-normative);
  launch/attach sent from the initialize response handler,
  fire-and-report-error, **no deadline** [M-1] but always C-g
  cancellable (stage 2).
- Configuration starts exactly once, from
  `initialized_seen && initialize_done`: snapshot the breakpoint source
  list, one `setBreakpoints` per source gathered by a
  success/failure-counting join — one failed source is reported, never
  wedges the join — then `setExceptionBreakpoints` built from the live
  `exceptionBreakpointFilters` honoring each filter's `default`
  (debugpy `uncaught` defaults true — Python uncaught-exception stops
  for free), then `configurationDone` **only if the capability says
  so**; without it the milestone completes after exception
  configuration.
- The session becomes ACTIVE idempotently from the launch response *or*
  the configurationDone response [M-1]; a stop may land while
  configurationDone is still pending [M-10]; a failed launch is
  retained and reported even while the adapter keeps emitting
  configuration traffic (measured) — the sequence runs,
  configurationDone fails, the session reports the launch error and
  tears down without wedging. No second execution command is sent while
  RESUMING.
- Teardown [M-8]: `exited` records the exit code and clears stopped
  state — it does **not** end the session; DEAD comes from
  `terminated` (whose `restart` value is retained for restart policy),
  the matched disconnect response, transport EOF/failure, or the
  forced-close grace deadline; idempotent, duplicate `terminated`
  tolerated, pending callbacks flushed with synthetic failures exactly
  once. The command matrix:

  | cause | launched session | attached session |
  | --- | --- | --- |
  | natural `terminated` | record end, close owned transport | same |
  | `dap-disconnect` | `disconnect {terminateDebuggee:false}` | same — detach is the point |
  | `dap-terminate`, capable | `terminate`, then grace/close | `terminate` (explicit user ask) |
  | `dap-terminate`, no cap but `terminateDebuggee` | `disconnect {terminateDebuggee:true}` | same, only because the user chose terminate |
  | editor exit / cleanup | terminate the debuggee | detach without terminating |
  | launch failure | disconnect/close; kill only an owned child | close socket |

  A timed-out `terminate` is forceful user-requested termination, not
  protocol success, and never loops into endless disconnects — close
  the transport, whose kill is the owned-child backstop.
- One session in v1, but the struct carries `parent`/`children` and all
  commands go through one session/thread oracle (parent plan,
  Architecture).

## Stage 5 — dap_breakpoint

`src/dap_breakpoint.[ch]`: the editor-owned table — breakpoints exist
with no session running and survive sessions (the dap-mode lesson that
matters most: the table is editor property, not session property).

- Keyed by `realpath()`ed absolute source path (display path retained
  beside the canonical key), ordered vector per path [M-9, M-11].
  Location is a sum type: a **right-gravity marker at the first byte of
  the line** in a live buffer — so inserting a newline exactly there
  keeps the breakpoint with the code — **or** `(path, line)` for files
  not open. Whole-line deletion has a decided, tested policy (retain at
  the resulting line). Buffers that visit no existing file (generated,
  unsaved) refuse breakpoints with a clear message. The demote/re-anchor
  round trip rides the event system: `KG_EVENT_BUFFER_KILLING` demotes
  while the marker still resolves, `KG_EVENT_BUFFER_OPENED` re-anchors
  (dape's overlay↔cons round-trip, dape.el:3482-3543).
- **At most one `setBreakpoints` in flight per source.** Each source
  carries `desired_generation` / `sent_generation` /
  `request_in_flight` / `dirty`: a snapshot is sent only when nothing is
  in flight, mutations during flight set dirty, the response applies to
  *its snapshot* — never by indexing the live vector (dape.el:1760-1762's
  hazard) — and a dirty source immediately re-sends the newest full
  snapshot. Removal of the last breakpoint sends the empty array, with
  the source resolved *before* the marker dies. Responses zip
  positionally; missing/malformed elements mark their snapshot entries
  unverified and are reported. Adapter ids are re-keyed after **every**
  successful set — measured on lldb-dap *and* debugpy, which reissues a
  new id even for an unchanged line — stored as `has_id + id`
  (debugpy's first id is 0), and the *returned* line is what renders
  (debugpy silently relocates and says verified) [M-9].
- `breakpoint` events: update the local projection by id, idempotent
  (lldb-dap fires `changed` right after each set response) — and they
  **never trigger a new `setBreakpoints` by themselves**, or an adapter
  that fires changed-after-every-set creates an id-churn loop. An
  unknown id with reason ≠ "removed" creates a client-side breakpoint
  only when it carries a usable source path and positive line
  (`sourceReference`-only is listed non-navigable in v1). A moved
  breakpoint re-anchors, merges deterministically with any breakpoint
  already on the target line (local id/condition retention defined),
  and re-notifies.
- Wire fields `condition`/`hit_condition`/`log_message` ship in the
  struct now, capability-gated at send; their UI is a follow-on. A
  `temporary` flag ships too — the UI's `dap-breakpoint-temporary` and
  `dap-until` need it, with this algorithm: create/enable the temporary
  breakpoint; wait for that source's newest set response; continue
  **only if it verified** (else remove and report); on stop, remove it
  when `hitBreakpointIds` matches, or — for adapters that omit hit ids,
  measured on debugpy and nbcode — after the stack fetch when actual
  source/line matches; never remove it because some *other* stop
  intervened; always remove on session end or cancel, resyncing if the
  session lives.

Tests: replacement-by-source; overlapping mutations with out-of-order
responses (the generation protocol); adapter moves a breakpoint;
verified → unverified → verified; id re-keying incl. debugpy id 0 and
same-line churn; out-of-range relocation; empty-array removal; the
closed-buffer round trip via the two events; edits above a breakpoint
(marker relocation); whole-line deletion; temporary-breakpoint
install-before-continue, hit without hit ids, intervening exception
stop, and session death.

## Stage 6 — execution and the stopped-state model

Commands land in `src/dap_commands.c` (cmdtable rows: sorted, ≤60-col
summaries, `CMD_READS_TERMINAL` + `reads_terminal[]` for `dap-evaluate`
and the config prompt; each command either `CMD_LISP_CALLABLE` or a
`not_lisp_callable[]` entry — decide per command, and regenerate
`utils/forecast/AUDIT.md` if a forecast sketch is added):
`dap-debug`, `dap-disconnect`, `dap-terminate`, `dap-continue`,
`dap-pause`, `dap-next`, `dap-step-in`, `dap-step-instruction`,
`dap-step-out`, `dap-breakpoint-toggle`, `dap-breakpoint-temporary`,
`dap-until`, `dap-goto`, `dap-restart`, `dap-evaluate`, `dap-repl`,
`dap-frame-up`, `dap-frame-down`, `dap-many-windows` — the full set the
UI subplan binds, audited here once (every prompting command carries
`CMD_READS_TERMINAL`; breakpoint/layout commands carry no
`CMD_EDITS_BUFFER` and work from read-only buffers). The multi-request
commands own their algorithms here, not in UI handlers: `dap-until` =
the temporary-breakpoint protocol from stage 5; `dap-goto` =
`gotoTargets` for source/line (column 1), a target choice (prompt when
several), then `goto` — both halves capability-gated, tagged with the
selection generation; `dap-restart` = the adapter `restart` request
when advertised (the spec's stated preference; lldb-dap advertises it
via the capabilities event, delve at initialize), client-side
terminate+relaunch otherwise (debugpy, nbcode), preserving config,
breakpoint table and layout with a bounded teardown of the old
session.

The stop model, in dape's proven order [M-10, M-11]:

1. On `stopped`: record reason (open string — lldb-dap's stopOnEntry
   says `"exception"/"signal SIGSTOP"`, nbcode's step says `"pause"`),
   bump the suspension epoch, reset frame selection to top, and set
   thread status immediately with the **spec defaults**: absent or
   false `allThreadsStopped` marks only the named thread stopped; true
   marks all. `threadId` itself is optional — with none, defer
   selection until `threads` answers and pick a stopped/first thread,
   never `stackTrace` with a guess. Honor `preserveFocusHint` (panes
   update, focus stays).
2. Then `threads`, tagged with a refresh serial so a second stop
   mid-flight cannot be overwritten by the late response (dape's
   threads-update-handle, dape.el:991-1023, upgraded from bool to
   serial).
3. Then `stackTrace` with `startFrame:0, levels:1`. If frame zero has
   an existing absolute path, navigate it; otherwise fetch one bounded
   page (20) and navigate the first frame whose source is openable —
   one frame alone cannot "prefer the topmost navigable frame" — and if
   none is, leave source focus where it was and say why. Relative,
   nonexistent and `sourceReference`-only paths are never passed to
   file visiting; those frames stay listed, non-navigable in v1. Then
   `scopes` → `variables` for visible/expanded panes only; no variable
   paging. Bounds (UI subplan carries the same numbers): 256 threads,
   200 retained frames, 64 scopes, 4096 variable nodes, depth 32, with
   cycle detection on repeated `variablesReference` ancestry and a
   visible truncation row.
4. The **suspension epoch** tags every continuation in this waterfall
   plus `evaluate`/`setVariable`. It bumps *before* any resume-implying
   send (next/step/continue/goto/restart), on accepted `stopped`, on
   unsolicited `continued`, and on session end — never only-on-stop,
   or a late reply lands in the continue-to-next-stop window [M-4].
   The **selection generation** bumps on same-stop thread/frame
   selection changes, so frame A's scopes reply cannot paint after the
   user chose frame B. If an execution request *fails*, the session
   returns to STOPPED and refetches the selected frame — the old
   references were already conservatively invalidated.
5. Step/continue: refuse without a stopped thread (one oracle, one
   error message); enter RESUMING before send; `granularity` only if
   capable; synthesize a local `continued` on success, handler
   idempotent — a real one may arrive before the response (delve),
   after it (lldb-dap), or never [M-10]; `continued`'s
   `allThreadsContinued` absent ⇒ **all** threads (the deliberate
   asymmetry with `allThreadsStopped`).
6. Debounce `thread` events' `threads` re-request (measured: three
   events in 70 ms from one `t.start()`; dape: "some adapters will blow
   unless :thread is throttled").

Decorations: `KG_DECOR_FACE_BREAKPOINT`, `_BREAKPOINT_PENDING`
(unverified), `_DEBUG_CURRENT` — three edits each per the decor recipe,
new `HL_*` colours only because these colours are genuinely new
(src/lsp_diag.c:363-364 precedent says otherwise reuse). The DAP tables
remain the source of truth; decorations are projections into open
buffers, rebuilt from the table on visit/kill/session-end.

## Stage 7 — acceptance and real adapters

`test/fake_dap_adapter.py` protocol modes, one per measured fact:
launch-response-early (lldb shape) / launch-response-after-configDone
(debugpy shape) / launch-response-between (nbcode/delve shape);
events-before-initialize-response; stopped-before-
configurationDone-response; seq-zero-on-everything; error-in-message vs
error-in-body.error.format; breakpoint relocation + id churn; multi
thread stop; nested variables; stale-reference-answers-success (returns
wrong data for old handles — asserts kg dropped it); output flood;
mid-line output splits; disconnect-then-linger (no exit); crash during
initialize; crash while stopped; delayed responses.

Native-test additions the review contributed, beyond the per-stage
lists above: stopped with no `threadId`; absent `allThreadsStopped`
touching only the named thread vs absent `allThreadsContinued` touching
all; `exited` followed much later by `terminated`, and `exited` with no
`terminated` ending only via the grace path; disconnect-vs-terminate
for launched and attached sessions; a capability event turning true to
false; a response whose `command` mismatches a valid `request_seq`;
pending-table full; a sustained event flood that cannot monopolize one
poll or exceed queue bounds; cancelling a launch that has no deadline;
frame-A scopes reply after selecting frame B; a variables reply landing
between resume and the next stop; top frame non-navigable with a
navigable second.

PTY cases (`requires_feature: dap`, fake adapter injected via
`KG_DAP_ADAPTER_<NAME>`, `workspace_files:` planting sources and config):
the full v1 milestone script — breakpoint visible, run to stop, line
highlighted, step, evaluate, frames, layout toggle — plus a
`WITH_DAP=0` case asserting clean absence.

Real-adapter smoke, skip-with-reason like clangd/ty: `requires_tool:
lldb-dap` on a `-g -O0` fixture; debugpy needs a harness extension
first — the YAML schema takes one `requires_tool` string and has no
import probe, so this stage adds either a list form (one skip reason
naming all missing tools; existing scalar cases stay valid — the Go
subplan needs `dlv` + `go` too) or a `requires_python_module:`
predicate, *before* any case promises to skip on a missing debugpy.
Timing: settle floors, not key delays, sized for
debugpy-under-sanitizer — ~41 ms/round-trip on an idle box, and the
review measured ~6.8 s to first stop for a real Python program, so
launch gets its own startup allowance distinct from per-key budgets.
These are the only honest timing tests; the fake adapter is faster than
every real one.

Docs land with their stages: README + `doc/kg.1` for commands and
config; `doc/coordinates.md` gains the DAP line/column space row
(1-based lines negotiated via `linesStartAt1`; columns ignored for
navigation in v1 — they are not `row->chars` byte offsets).
