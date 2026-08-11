# DAP subplan 03 — Java via nbcode (measured); jdtls road recorded

Parent: `doc/plans/2026-08-11-dap.md`; prerequisites: subplans 00-02
complete (Java is the first adapter extension, not part of the v1
milestone). Everything marked measured was observed live on this box on
2026-08-11: a probe drove a real `nbcode` (oracle-java extension 26.0.1,
installed by `utils/install-nbcode.sh`) through the full choreography —
breakpoint hit, threads/stack/scopes/variables/evaluate/next/continue —
over kg's transport plus a 69-line parameterization diff. The jdtls +
java-debug road was assessed from source and a successful plugin build,
but not measured live (no jdtls on this box); it is recorded, deferred.

## Verdict

Java debugging is feasible on the planned architecture, via the same
nbcode process kg already speaks LSP to. It needs strictly less new
machinery than the plan budgeted for "Java someday", because session
initiation carries the program directly — no executeCommand resolver
protocol — and both Java roads share one new seam (a DAP connection
whose endpoint is discovered through an LSP session).

## The nbcode road (planned)

### How it hangs together (verified in oracle/javavscode source)

One `nbcode` process, started with **both**
`--start-java-language-server=listen-hash:0` and
`--start-java-debug-adapter-server=listen-hash:0`
(vscode/src/lsp/launchOptions.ts:54-62), announces two servers on one
stdout, each twice, only the `with hash` line being the announce:

```
Java Language Server listening at port N
Java Language Server listening at port N with hash <128 hex>
Java Debug Server Adapter listening at port M
Java Debug Server Adapter listening at port M with hash <128 hex>
```

**Correction to src/lsp_transport.h:100-104**: the sibling line begins
`Java Debug Server Adapter ...`, with a leading `Java ` the comment
omits (Oracle's own regex, vscode/src/lsp/initializer.ts:66-85, is an
unanchored search so both spellings match it; kg's stored prefix must be
the measured one). The DAP socket handshake is byte-identical to the LSP
one: connect 127.0.0.1:M, write the 128 hash bytes, no newline, then
Content-Length frames (vscode/src/debugger/debugger.ts:70-91).

The launch carries the program directly — `{type:"jdk",
request:"launch", file:<file URI>, classPaths:["any"],
console:"internalConsole"}` (debugger.ts:199-224); no
`workspace/executeCommand` resolver round-trips. `file` is parsed as a
URI (`new URI(s)`), **not** a path. Other accepted launch keys:
`mainClass`, `classPaths`/`classpath`, `modulePaths`, `sourcePaths`,
`noDebug`, `vmArgs`, `methodName`, `projectFile`, `project`,
`nativeimage`. Single-file launch (no pom) works via NetBeans'
java.file.launcher module, present and enabled in the installed
extension.

### The hard dependency: an nbcode-shaped LSP session, first (measured)

- DAP `initialize` works with no LSP client attached (1.44 s).
- DAP `launch` **NPEs** (`NbLaunchDelegate.java:226`,
  "Internal error.") without one — the DAP connection captures the LSP
  session's `OperationContext` at connection-construction time.
- A *bare* LSP client is not enough: without
  `initializationOptions.nbcodeCapabilities` (the object
  vscode/src/lsp/nbLanguageClient.ts:61-74 sends — notably
  `wantsJavaSupport: true`, `commandPrefix: "jdk"`), DAP `initialize`
  hangs indefinitely. And the LSP client must answer
  `workspace/configuration` with an array of the right length (one
  `null` per item) — a server blocked on that request never finishes
  opening the project the DAP side waits on.

**Ordering rule for the plan**: kg's Java LSP session up and
`initialized` → then connect the DSA socket. Never the reverse.

### Measured choreography deviations kg's client must absorb

All of these extend the fake adapter's scripted modes and the client's
tolerances (subplan 01 stages 3-6):

1. **A third launch-response ordering**: after `initialized`, before
   `configurationDone` (debugpy: after configurationDone; lldb-dap:
   before initialized). The event-driven state machine already handles
   it; the fake adapter gains the third mode.
2. **No `terminated`, no `exited` event, ever** (waited 60 s past
   program completion). Session end is inferred from
   `thread{reason:"exited"}` for the debuggee thread plus a deadline —
   the teardown lattice gains that edge.
3. `next` stops with **`reason:"pause"`**, not `"step"` — reason is
   already an open string; never branch UI on it.
4. `setBreakpoints` answers **`verified:false`**; verification arrives
   ~600 ms later as a `breakpoint{reason:"changed"}` event. The
   table-is-truth/decorations-are-projections design is what makes this
   render correctly (pending face until the event).
5. Minimal `stopped` body ({reason, threadId} only); `continue` answers
   `allThreadsContinued:false` — per-thread semantics, defaults matter.
6. One scope, named `Local`, containing a pseudo-variable `Static`;
   frames include JVM internals with `line:0` and no `source` (the
   non-navigable-frame path from subplan 01 stage 6 gets daily use).
7. Program stdout arrives as **two output events per line** (text, then
   the newline separately), interleaved with nbcode's own debugger
   chatter on the same `stdout` category — append-as-bytes already
   handles it.
8. Zero reverse requests observed (capability not advertised — the v1
   policy holds).
9. Latency: both announces ≈1.4-2.1 s from spawn on a cold fresh
   userdir (~31 KB of module-list stdout precedes them — inside the
   256 KiB announce bound); DAP round trips 38-104 ms. Fresh
   `--userdir` per session is mandatory (NetBeans single-instance
   handling; kg's nbcode wrapper already does this).
10. **`disconnect` leaves nbcode alive — correctly**: it is the user's
    Java LSP server. The DAP socket is the only thing closed; the
    session-end path must not kill or reap the shared child.

### Work items (beyond subplans 00-02), measured sizes

1. `attach_socket(fd)` on the transport: wrap an already-connected
   socket, no child (`pid -1`, reaped), phase OPEN — **and it sets
   `O_NONBLOCK` itself** (the probe hung 8 minutes when the caller
   forgot; the transport must own it). ~20 lines. This generalizes
   subplan 00-A's "wrap an already-open fd pair" from a fuzz-only seam
   to a real constructor.
2. Announce scanning becomes **report-every-announce**: today
   `announce_take()` connects to the one compile-time prefix
   (src/lsp_transport.c:711-712). One nbcode announces two servers on
   one stdout, and the DSA port must come out of the *LSP* transport's
   log channel to be connected later — so the scanner reports (prefix,
   port, hash) tuples upward and the owner decides. Prefix moves from
   macro to per-transport data (NULL = today's LSP prefix; existing
   callers unchanged), ~15 lines. The `" with hash "` separator stays
   shared.
3. A third adapter-spec transport kind, `lsp-sibling`: no command of
   its own; names an LSP session ("java"), an announce prefix, and the
   hash handshake. Connects only after that session is `initialized`.
4. LSP-side prerequisites that Java debugging cannot work without (and
   which improve the nbcode LSP session regardless):
   `initializationOptions.nbcodeCapabilities` on the Java server spec,
   and a `workspace/configuration` responder that answers an array of
   the requested length.
5. `${fileUri}` in the substitution set (URI-valued; `lsp_uri.c`
   already converts).
6. Client tolerances 1-5 above, each with a fake-adapter mode and a
   native test.

### `.kg-dap.json` shape

```json
{
  "name": "Java (nbcode)",
  "adapter": {
    "transport": "lsp-sibling",
    "lsp": "java",
    "announce": "Java Debug Server Adapter listening at port ",
    "handshake": "hash"
  },
  "request": "launch",
  "arguments": {
    "type": "jdk",
    "file": "${fileUri}",
    "classPaths": ["any"],
    "console": "internalConsole"
  }
}
```

### Tests

Fake-adapter modes for deviations 1-5; native tests for the announce
reporter (two servers on one stdout, bare-line-before-hash-line, log
noise around the announce) and `attach_socket` (including the
nonblocking guarantee); a real-adapter smoke case `requires_tool:
nbcode` in the mold of the existing `lsp-nbcode-definition.yaml`
(fresh userdir per run, settle budget sized for the ~2 s announce and
the LSP-session warm-up the DAP connect waits on).

## The jdtls + java-debug road (recorded, deferred)

Assessed from source; not measured (no jdtls on this box — kg's
`server_specs[]` Java row invokes a bare `jdtls`,
src/lsp_server.c:75-76, which does not resolve here).

- The adapter is an OSGi bundle, `com.microsoft.java.debug.plugin`
  (built here in 45 s with the repo's own `./mvnw` — Maven 3.9.9 fails
  against Tycho 5, the pinned 3.9.11 works; network required), loaded
  into jdtls via `initializationOptions.bundles`.
- Reached by `workspace/executeCommand vscode.java.startDebugSession` →
  the plugin opens `ServerSocket(0,1)` — ephemeral loopback port,
  backlog 1, **plain TCP, no hash** — and returns the port
  (JavaDebugDelegateCommandHandler.java:35,55-58).
- Unlike nbcode, kg would have to resolve launch config itself first:
  `vscode.java.resolveMainClass`, `resolveClasspath`,
  `buildWorkspace`, `validateLaunchConfig` — a small resolver protocol
  per session.
- kg-side work beyond this subplan's items: the executeCommand-returning-
  a-scalar seam, `initializationOptions.bundles` on the server spec, a
  jdtls install (~250 MB) plus the jar shipped or located. Defer until
  someone actually runs jdtls; the transport pieces are shared with the
  nbcode road either way.
