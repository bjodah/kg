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
inline stubs, modeled on src/lsp.h + lsp_core.c; no editor module
carries `KG_USE_DAP` conditionals. New CI lane `.ci/ci-16-with-dap-0.sh`
mirroring ci-14: build, `-V | grep -- '-dap'`, `make check WITH_DAP=0`,
then orthogonality builds `WITH_LISP=0 WITH_DAP=1` and
`WITH_LSP=0 WITH_DAP=1`, ending clean. The runner's glob picks it up.

## Stage 2 — dap_transport

`src/dap_transport.[ch]` composes framed_io (subplan 00-A): stdio child
via `kg_process_spawn_bidi()` (stdin_fd must be -1, stderr never joined
to stdout — the process.h:68-78 contract), or TCP connect for attach.
Owns: reaping its own child poll-driven (`waitpid(WNOHANG)`, no SIGCHLD
— house design), the stdin half-close + deadline + kill-backstop
end-of-session ladder [M-8], and the rule that **EOF is not the
end-of-session signal** (debugpy lingers forever after disconnect;
lldb-dap exits immediately — both measured).

Tests: `test_dap_transport.c` driving `test/fake_dap_adapter.py`'s
framing modes — copied nearly verbatim from the fake LSP server's
(`echo`, `split`, `batch`, `garbage`, `huge-header`, `die`, `truncated`),
since the framing layer is shared and already proven; what is new here is
lifecycle (linger, half-close, EOF-vs-disconnect ordering).

## Stage 3 — dap_client

`src/dap_client.[ch]`: the protocol brain, no editor state.

- **Two seq spaces** [M-5]: one monotonic client counter stamped on
  requests *and* on responses to reverse requests (spec: per-actor,
  per-message); responses matched by `request_seq` **only** — lldb-dap
  stamps `seq:0` on everything; a string `seq` is coerced (netcoredbg).
- **Pending table** copied from lsp_client's proven shape: flat array,
  slot copied out and cleared before the callback runs, per-request
  `CLOCK_MONOTONIC` deadlines, exactly-once callback on answer, deadline
  or death (src/lsp_client.c:408-455, 554-628; src/lsp_client.h:249-276).
  Launch/attach is the one deadline-exempt request [M-1].
- **Queue-then-dispatch**: frames drain into a message queue inside the
  poll; handlers run after the read loop, because handlers write
  (reverse-request replies) and dispatch-inside-read is the jsonrpc.el
  reentrancy bug class.
- **Reverse requests**: a dispatch table with a **default arm that sends
  `success:false`** — silence hangs adapters [M-3]; dap-mode's missing
  default clause is the cautionary bug. v1 advertises neither
  `supportsRunInTerminalRequest` nor `supportsStartDebuggingRequest`
  [M-2, M-3].
- **Errors**: `message` → `body.error.format` → synthesized; honor
  `showUser` [M-6]. A `success:false` response is data, not a transport
  failure.
- **Capabilities**: a mutable struct, merged (never replaced) from the
  initialize response *and* `capabilities` events [M-7]; reads both
  `supportTerminateDebuggee` and debugpy's `supportsTerminateDebuggee`;
  absent ≠ false (`KG_JSON_NONE`). One accessor,
  `dap_capable(session, CAP_X)`, so every gate reads the live set.
- Unknown events and unknown fields are ignored [M-13]; events arriving
  before the initialize response are buffered and accepted.
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
- User config file: `.kg-dap.json` at the workspace root (parent plan,
  decision 2), entries named, each carrying `adapter`, `request`,
  optional `build`, and a verbatim `arguments` object. Substitutions
  are a closed set — `${file}`, `${fileDir}`,
  `${workspaceRoot}`, `${env:NAME}` — expanded longest-key-first,
  unknown → empty + warning (dap-variables' ordering discipline).
  Arguments pass through **verbatim**: parse once for validation, then
  splice via `kg_jsonw_raw()`; never rebuild user JSON (no float writer,
  and absent ≠ null matters — debugpy errors on explicit nulls where it
  expects omission, per dap-python.el:220-236).
- Env override hook for tests, per-adapter: `KG_DAP_ADAPTER_<NAME>`,
  mirroring `KG_LSP_SERVER_<MODE>` (src/lsp_server.h:135-159), so C++
  and Python PTY cases inject different fakes in one build.
- An optional `"build"` command run to completion (via the existing
  compile machinery) before launch, failure aborting the session —
  dap-mode's `preLaunchTask` lesson: for C++ this is the difference
  between usable and not.

`src/dap_session.[ch]`: the state machine.

```
IDLE → SPAWNING → INITIALIZING → CONFIGURING → RUNNING ⇄ STOPPED
                                        ↘ (any state) → DEAD
```

- initialize first, nothing before its response (spec-normative);
  launch/attach sent from the initialize response handler,
  fire-and-report-error, **no deadline** [M-1].
- Configuration driven solely by the `initialized` event:
  per-source `setBreakpoints` fan-out with a response-counter join, then
  `setExceptionBreakpoints` built from `exceptionBreakpointFilters`
  honoring each filter's `default` (debugpy `uncaught` defaults true —
  Python uncaught-exception stops for free), then `configurationDone`
  **only if the capability says so**.
- RUNNING reachable idempotently from the launch response *or* the
  configurationDone response [M-1]; a `stopped` before the
  configurationDone response is legal and handled [M-10]; a failed
  launch does not suppress `initialized` (measured) — the sequence runs,
  configurationDone fails, the session reports the launch error and
  tears down without wedging.
- Teardown lattice [M-8]: any of terminated/exited/EOF/disconnect
  response → DEAD, idempotent, pending callbacks flushed with synthetic
  failures. Shutdown policy: `terminate` if capable else
  `disconnect {terminateDebuggee:true}` (lldb-dap's *normal* path); a
  timeout is terminal — close the transport, whose kill is the backstop.
- One session in v1, but the struct carries `parent`/`children` and all
  commands go through one session/thread oracle (parent plan,
  Architecture).

## Stage 5 — dap_breakpoint

`src/dap_breakpoint.[ch]`: the editor-owned table — breakpoints exist
with no session running and survive sessions (the dap-mode lesson that
matters most: the table is editor property, not session property).

- Keyed by `realpath()`ed absolute source path, ordered vector per path
  [M-9, M-11]. Location is a sum type: marker in a live buffer
  (decoration/marker layer; survives edits above it) **or** `(path,
  line)` for files not open — buffer kill demotes to the pair, visiting
  the file re-anchors (dape's overlay↔cons round-trip, dape.el:3482-3543).
- Every mutation re-sends the whole per-source set; removal of the last
  breakpoint sends the empty array, with the source resolved *before*
  the marker dies. The request array is a snapshot, never the live table
  (dape.el:1760-1762's in-flight mutation hazard); the response zips
  positionally; adapter ids re-keyed after **every** set (lldb-dap
  reissues ids), stored as `has_id + id` (debugpy's first id is 0), and
  the *returned* line is what renders (debugpy silently relocates and
  says verified) [M-9].
- `breakpoint` events: update by id, idempotent (lldb-dap fires
  `changed` right after each set response); unknown id with reason ≠
  "removed" creates a client-side breakpoint; a moved breakpoint
  re-anchors, merges with any breakpoint already on the target line, and
  re-notifies.
- Wire fields `condition`/`hit_condition`/`log_message` ship in the
  struct now, capability-gated at send; their UI is a follow-on.

Tests: replacement-by-source; adapter moves a breakpoint; verified →
unverified → verified; id re-keying; empty-array removal; the
closed-buffer round trip; edits above a breakpoint (marker relocation).

## Stage 6 — execution and the stopped-state model

Commands land in `src/dap_commands.c` (cmdtable rows: sorted, ≤60-col
summaries, `CMD_READS_TERMINAL` + `reads_terminal[]` for `dap-evaluate`
and the config prompt; each command either `CMD_LISP_CALLABLE` or a
`not_lisp_callable[]` entry — decide per command, and regenerate
`utils/forecast/AUDIT.md` if a forecast sketch is added):
`dap-debug`, `dap-disconnect`, `dap-terminate`, `dap-continue`,
`dap-pause`, `dap-next`, `dap-step-in`, `dap-step-out`,
`dap-breakpoint-toggle`, `dap-evaluate`, `dap-frame-up`,
`dap-frame-down`.

The stop model, in dape's proven order [M-10, M-11]:

1. On `stopped`: record reason (open string — lldb-dap's stopOnEntry
   says `"exception"/"signal SIGSTOP"`), force-select the stopping
   thread, reset frame selection to top, set thread status immediately
   (`allThreadsStopped` absent ⇒ true), bump `stop_epoch`.
2. Then `threads`, guarded by a pre-request handle so a second stop
   mid-flight cannot be overwritten by the late response (dape's
   threads-update-handle, dape.el:991-1023).
3. Then `stackTrace` for **one** frame → navigate (file-exists check;
   prefer the topmost frame with an openable source; frames with dead
   relative paths or `sourceReference` stay listed, non-navigable in v1)
   → `scopes` → `variables` for the visible pane only. Deeper frames
   (20) fetched when the stack pane renders; no variable paging.
4. `stop_epoch` tags every continuation in this waterfall plus
   `evaluate`/`setVariable`; stale replies are dropped; every cached
   `variablesReference`/`frameId` dies on any resume-implying request
   [M-4].
5. Step/continue: refuse without a stopped thread (one oracle, one
   error message); `granularity` only if capable; synthesize a local
   `continued` on success, handler idempotent (lldb-dap also sends a
   real one) [M-10]; `continued`'s `allThreadsContinued` absent ⇒ true.
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
(debugpy shape); events-before-initialize-response; stopped-before-
configurationDone-response; seq-zero-on-everything; error-in-message vs
error-in-body.error.format; breakpoint relocation + id churn; multi
thread stop; nested variables; stale-reference-answers-success (returns
wrong data for old handles — asserts kg dropped it); output flood;
mid-line output splits; disconnect-then-linger (no exit); crash during
initialize; crash while stopped; delayed responses.

PTY cases (`requires_feature: dap`, fake adapter injected via
`KG_DAP_ADAPTER_<NAME>`, `workspace_files:` planting sources and config):
the full v1 milestone script — breakpoint visible, run to stop, line
highlighted, step, evaluate, frames, layout toggle — plus a
`WITH_DAP=0` case asserting clean absence.

Real-adapter smoke, skip-with-reason like clangd/ty: `requires_tool:
lldb-dap` on a `-g -O0` fixture; debugpy via `requires_tool: python3`
plus an import probe in the case setup (the ensure-import discipline from
dap-python). Timing sized for debugpy-under-sanitizer (~41 ms/round-trip
measured on an idle box — the slowest lane under load is the one nobody
measured; use settle floors, not key delays). These are the only honest
timing tests; the fake adapter is faster than both real ones.

Docs land with their stages: README + `doc/kg.1` for commands and
config; `doc/coordinates.md` gains the DAP line/column space row
(1-based lines negotiated via `linesStartAt1`; columns ignored for
navigation in v1 — they are not `row->chars` byte offsets).
