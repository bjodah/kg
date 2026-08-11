### Prompt

We currently have LSP support and syntax highlighting via tree-sitter. There is one "leg" missing for kg to become a good IDE, and that is debugging. I'd like to add support for DAP. In the targeted environment we for example have:

```
$ python -c "import debugpy; print(debugpy.__version__)"
1.8.21
$ which lldb-dap && lldb-dap --version
/opt-2/llvm-21/bin/lldb-dap
lldb-dap: LLVM (http://llvm.org/):
  LLVM version 21.1.8
  Optimized build.
liblldb: lldb version 21.1.8
```

here exists prior art for "emacs proper" in the form of svaante/dapee (and to some extent, but with more baggage emacs-lsp/dap-modee).Since we recently taught kg to speak over TCP sockets, and parse json I'm hopeful that this is doable. One question that arises is: should this be in elisp via fe or coded in C directly. I'm leaning towards C directly since kg's elisp support via fe is immature still.As for what features needed: gdb-many-windows like experience (similar to what dape offers). Along with user customizeable keybindings. e.g. from my personal emacs config I have two relevant blocks that I'd like to reproduce somewhat closely in kg:

```
(add-hook 'gdb-mode-hook
     (function (lambda ()
                 (define-key gud-minor-mode-map (kbd "<f4>") #'gud-print)
                 (define-key gud-minor-mode-map (kbd "<f5>") #'gud-step)
                 (define-key gud-minor-mode-map (kbd "C-<f5>") #'gud-stepi)
                 (define-key gud-minor-mode-map (kbd "<f6>") #'gud-next)
                 (define-key gud-minor-mode-map (kbd "<f7>") #'gud-finish)
                 (define-key gud-minor-mode-map (kbd "<f8>") #'gud-cont)
                 (define-key gud-minor-mode-map (kbd "<f9>") #'gud-break)
                 (define-key gud-minor-mode-map (kbd "C-<f9>") #'gud-tbreak)
                 (define-key gud-minor-mode-map (kbd "<f10>") #'gud-until)
                 (define-key gud-minor-mode-map (kbd "M-<f10>") #'gud-jump)
                 (define-key gud-minor-mode-map (kbd "<f11>") #'gud-run)
                 (define-key gud-minor-mode-map (kbd "M-<f11>") #'gud-kill)
                 ;(define-key gud-minor-mode-map (kbd "<f12>") #'gdb-many-windows)
                 (define-key gud-minor-mode-map (kbd "<f12>")
                   (lambda() (interactive) (progn (gdb-many-windows) (other-window) (other-window))))
                 (define-key gud-minor-mode-map (kbd "M-<up>") #'gud-up)
                 (define-key gud-minor-mode-map (kbd "M-<down>") #'gud-down)
                 (define-key gud-minor-mode-map (kbd "<prior>") #'gud-up)
                 (define-key gud-minor-mode-map (kbd "<next>") #'gud-down)
 )))
```

or my older dap-mode config:

```
(add-hook 'gdb-mode-hook
     (function (lambda ()
                 (define-key gud-minor-mode-map (kbd "<f4>") #'gud-print)
                 (define-key gud-minor-mode-map (kbd "<f5>") #'gud-step)
                 (define-key gud-minor-mode-map (kbd "C-<f5>") #'gud-stepi)
                 (define-key gud-minor-mode-map (kbd "<f6>") #'gud-next)
                 (define-key gud-minor-mode-map (kbd "<f7>") #'gud-finish)
                 (define-key gud-minor-mode-map (kbd "<f8>") #'gud-cont)
                 (define-key gud-minor-mode-map (kbd "<f9>") #'gud-break)
                 (define-key gud-minor-mode-map (kbd "C-<f9>") #'gud-tbreak)
                 (define-key gud-minor-mode-map (kbd "<f10>") #'gud-until)
                 (define-key gud-minor-mode-map (kbd "M-<f10>") #'gud-jump)
                 (define-key gud-minor-mode-map (kbd "<f11>") #'gud-run)
                 (define-key gud-minor-mode-map (kbd "M-<f11>") #'gud-kill)
                 ;(define-key gud-minor-mode-map (kbd "<f12>") #'gdb-many-windows)
                 (define-key gud-minor-mode-map (kbd "<f12>")
                   (lambda() (interactive) (progn (gdb-many-windows) (other-window) (other-window))))
                 (define-key gud-minor-mode-map (kbd "M-<up>") #'gud-up)
                 (define-key gud-minor-mode-map (kbd "M-<down>") #'gud-down)
                 (define-key gud-minor-mode-map (kbd "<prior>") #'gud-up)
                 (define-key gud-minor-mode-map (kbd "<next>") #'gud-down)
 )))
```

What per-requisities do we need to prepare first? What is a reasonable implementation plan? (i.e. what features do we need to add/modify, what structs are affected in existing files, what new files do we expect that we need to create). What's a good testing strategy? Languages I want to target first are C++, Python and eventually Java.

---

### **ChatGPT**

Yes. Given the current state of kg, I would implement DAP primarily in C, not in fe/Elisp.
The important distinction is that I would not make it anti-Elisp: the debugger engine, protocol state, breakpoint model, and debugger windows should be native C, while the existing Lisp/keymap interface can be used as a thin customization layer. That gives you debugging in WITH_LISP=0, while still allowing an init file to say something close to (define-key dap-mode-map (kbd "<f5>") ...) later.
There is also quite a lot more reusable infrastructure in the branch than I initially expected. In particular, DAP is a very natural sibling of the LSP implementation.
1. Why C is the right layer
DAP and LSP use essentially the same outer wire framing: a Content-Length header followed by a UTF-8 JSON body. DAP supports both an adapter started as a child over stdin/stdout and an already-running adapter reached over TCP.
kg already has almost exactly the infrastructure necessary for the first case:

kg_process_spawn_bidi() provides nonblocking stdin/stdout and a separate stderr pipe.

lsp_transport already does bounded, nonblocking Content-Length framing, outbound backpressure, process lifetime handling, stderr separation, and socket operation for its Java special case.

lsp_json is actually a general strict JSON parser/writer despite its name.

the editor's idle loop already polls server descriptors so responses can wake kg immediately rather than waiting for a keystroke.

the LSP tests already established a very good pattern: a deterministic Python fake server, real child processes, bounded pump loops rather than sleeps, native tests, fuzzing, PTY acceptance tests, and optional real-server smoke tests.

The part that must not be shared is lsp_client. DAP is not JSON-RPC. DAP messages have seq and type, requests use command, responses use request_seq, and adapters can independently send events and requests back to the client. So I would share transport + JSON, but build a completely separate dap_client.
The current LSP plan made LSP intentionally independent of Lisp and tree-sitter, and I think DAP should follow exactly that architectural principle.

2. Prerequisites I would land before the DAP implementation
There are four fairly clear preparatory changes.
A. Extract the accidentally-LSP-specific generic pieces
I would do this before writing dap_transport.c.
Today:

```
lsp_transport.c
    process spawning
    nonblocking IO
    Content-Length framing
    bounded inbox/outbox
    stderr/log side channels
    special nbcode socket handshake

lsp_json.c
    completely generic JSON
```

I would turn that approximately into:

```
json.c / json.h
    kg_json_parse()
    kg_json_get()
    kg_jsonw_*
    ...

framed_io.c / framed_io.h
    Content-Length frame reader/writer
    bounded inbox/outbox
    nonblocking send/receive

lsp_transport.c
    stdio child setup
    nbcode listen-hash setup
    composes framed_io

dap_transport.c
    stdio child setup
    ordinary TCP connection
    composes framed_io
```

A full rewrite is unnecessary. This can be mostly a mechanical extraction from the existing implementation, whose current contract deliberately knows nothing about JSON or requests anyway.
I would definitely rename lsp_json_* to something generic rather than introduce a second JSON implementation.
B. Generalize the idle FD integration
This is probably the most important structural prerequisite.
At the moment tty.c contains things like:

```C
int fds[KG_LSP_WAIT_FDS_MAX];
count = lsp_wait_fds(fds, KG_LSP_WAIT_FDS_MAX);
...
changed |= lsp_poll();
```

and even the KG_IDLE_FD branch assumes that the waking descriptor belongs to LSP.
DAP shouldn't turn this into:

```C
lsp_wait_fds(...);
dap_wait_fds(...);
lsp_poll();
dap_poll();
```

scattered around tty.c.
I'd extract something deliberately modest, perhaps:

```C
int editor_async_wait_fds(int *fds, int max);
int editor_async_poll(void);
```

with the implementation currently aggregating LSP and DAP. I would not build an elaborate callback/plugin registry yet; two native asynchronous subsystems don't justify it.
Then:

```
tty.c
    knows only editor_async_*

lsp.c
dap.c
    know their respective protocols
```

C. Finish the function-key input model
This one is mandatory for your desired UX.
Right now enum key_base only defines F3 and F4, and keyevent.c likewise only knows <f3> and <f4>.
So before debugging controls, add:

```
KEY_BASE_F5 ... KEY_BASE_F12
```

and teach tty.c the usual terminal sequences for all of them.
I would take the opportunity to generalize the modified-CSI handling too. Right now parts of it explicitly distinguish modifier 2 = Shift and 5 = Ctrl.
You need at least:

```
<f4> ... <f12>
C-<f5>
C-<f9>
M-<f10>
M-<f11>
M-<up>
M-<down>
```

So parse xterm's modifier number generically into Ctrl/Meta/Shift instead of adding one case for every debugging key.
D. Add a real window-layout API
MAX_WINDOWS == 8, so kg already has enough slots for the classic six-window GDB arrangement. win_reflow()'s column-group model can actually represent two columns of three windows quite naturally.
What is missing is an API to construct and restore such a layout deterministically. win_display_buffer_other_window() is convenient for one auxiliary buffer, but chaining that six times isn't a good debugger-layout implementation.
I'd add something along the lines of:

```C
struct kg_window_configuration;
int win_configuration_save(struct kg_window_configuration *);
int win_configuration_restore(const struct kg_window_configuration *);

int win_arrange_grid(...);
int win_show_buffer_in(int window, int buffer);
```

The snapshot should not blindly memcpy struct editor_window: it owns an opaque vgeom pointer, and the current split implementation already contains careful comments about why copying that pointer is unsafe.
This is useful beyond DAP.

3. DAP architecture
I would make the dependency graph look like this:

```
dap_commands.c
                         │
             ┌───────────┴────────────┐
             │                        │
         dap_ui.c                dap_session.c
                                      │
                         ┌────────────┼────────────┐
                         │            │            │
                 dap_breakpoint.c  dap_config.c  dap_client.c
                                                   │
                                             dap_transport.c
                                                   │
                                   ┌───────────────┴──────────────┐
                                   │                              │
                              framed_io.c                     process.c
                                   │
                                json.c
```

I would initially allow one active debug session. Make struct dap_session opaque enough that it can become a registry later, but multiple simultaneous sessions would add complexity everywhere—pane ownership, breakpoint verification, active frame, output routing—without helping the first useful release.
The DAP lifecycle is materially richer than LSP. The adapter first answers initialize with capabilities; after launch/attach it emits initialized, at which point the client sends all its breakpoints and finally configurationDone. setBreakpoints replaces the whole breakpoint set for one source, rather than adding/removing one breakpoint incrementally. That fact should shape the data model from the beginning.

4. Core structs I would introduce
Something approximately like:

```C
enum dap_session_state {
    DAP_SESSION_STARTING,
    DAP_SESSION_CONFIGURING,
    DAP_SESSION_RUNNING,
    DAP_SESSION_STOPPED,
    DAP_SESSION_TERMINATING,
    DAP_SESSION_DEAD,
};

struct dap_capabilities {
    bool configuration_done;
    bool function_breakpoints;
    bool conditional_breakpoints;
    bool hit_conditional_breakpoints;
    bool evaluate_hover;
    bool restart;
    bool terminate;
    bool goto_targets;
    bool stepping_granularity;
    bool instruction_breakpoints;
    ...
};

struct dap_breakpoint {
    uint64_t local_id;
    char *path;
    int requested_line;
    int requested_column;

    int adapter_id;
    int actual_line;
    int actual_column;

    bool enabled;
    bool verified;
    bool temporary;

    char *condition;
    char *hit_condition;
    char *log_message;
};

struct dap_frame {
    long long id;
    long long thread_id;
    char *name;
    char *path;
    int line;
    int column;
};

struct dap_thread {
    long long id;
    char *name;
};

struct dap_variable {
    char *name;
    char *value;
    char *type;
    long long variables_reference;
    ...
};

struct dap_session {
    struct dap_client *client;
    enum dap_session_state state;
    struct dap_capabilities caps;

    long long selected_thread;
    long long selected_frame;

    uint64_t stop_epoch;

    ...
};
```

The stop_epoch is worth having from day one. DAP specifies that object references used for scopes/variables are valid only during the current suspended state and become invalid after execution resumes. So every async reply associated with:

```
stackTrace
scopes
variables
evaluate
```

should effectively belong to stop generation N. If a late answer for N arrives after the program continued and stopped again at N+1, drop it.
That's a cheap guard against some very unpleasant stale-UI bugs.
I would not add DAP fields to struct editor_buffer, struct editor_window, or struct editor_config. Those structures are already carrying fundamental editor state. Keep debugging state owned by the DAP subsystem.

5. Breakpoint and source display
The existing decoration layer is a good fit for source annotations. It already provides marker-anchored transient visual ranges with fixed faces and priorities.
I'd add faces such as:

```C
KG_DECOR_FACE_BREAKPOINT
KG_DECOR_FACE_BREAKPOINT_PENDING
KG_DECOR_FACE_DEBUG_CURRENT
```

and map them in display.c.
For v1, highlighting the source line is sufficient. A proper Emacs-style fringe/gutter breakpoint icon can be a separate renderer enhancement later.
The DAP breakpoint table must remain the source of truth, not the decoration handles. Decorations are just projections into currently-open buffers.
That is particularly important because adapters may:

reject a requested breakpoint,

move it to another line,

later change its verification state.

DAP explicitly reports the actual breakpoint back to the client and can later send breakpoint events.
6. The gdb-many-windows experience
I'd explicitly target the actual Emacs arrangement:

```
+---------------------------+---------------------------+
| Debug/REPL                | Locals / Registers        |
+---------------------------+---------------------------+
| Source                    | Debuggee I/O              |
+---------------------------+---------------------------+
| Stack                     | Breakpoints / Threads     |
+---------------------------+---------------------------+
```

That's the layout Emacs documents for gdb-many-windows, including saving/restoring window configurations.
For kg I'd start with special buffers:

```
*dap-repl*
*dap-locals*
*dap-output*
*dap-stack*
*dap-breakpoints*
*dap-threads*
```

but display breakpoints/threads in the same logical slot, as GDB does. Locals/registers can similarly share a slot later.
dape is useful prior art here: its current GNU ELPA version has a GUD-style window arrangement, variable explorer/watch, REPL, breakpoints, memory and disassembly views. I would regard variable explorer + REPL + GUD layout as the v1 inspiration and deliberately defer memory/disassembly.
One capacity issue: MAX_WINDOWS=8 is fine, but MAX_BUFFERS=20 is less comfortable when DAP may want 5–6 generated buffers on top of a project.
I'd consider raising MAX_BUFFERS to 32 or 64 as part of the layout stage.

7. Keybindings
The existing keymap architecture has exactly the layer we want:

```
transient > minor > major > global
```

So debugging should be the first real use of a native minor-mode map:

```C
dap_map = keymap_create("dap", KEYMAP_LAYER_MINOR);
```

active only while a DAP session is live.
That's much cleaner than inserting DAP into the current name-keyed major-mode table.
Your bindings translate reasonably well:
Keykg commandDAP operationF4dap-evaluateevaluateF5dap-step-instepInC-F5dap-step-instructionstepIn with instruction granularityF6dap-nextnextF7dap-step-outstepOutF8dap-continuecontinueF9dap-breakpoint-togglelocal table + full setBreakpointsC-F9dap-breakpoint-temporaryclient-side one-shot bpF10dap-untiltemporary bp + continueM-F10dap-gotogotoTargets + goto, capability-gatedF11dap-restartrestart, otherwise relaunchM-F11dap-terminateterminate/disconnectF12dap-many-windowstoggle debugger layoutM-up / Priordap-frame-upselect older caller frameM-down / Nextdap-frame-downselect younger frame
Two deserve caveats.
gud-until has no direct portable DAP counterpart, so it should be emulated with a temporary source breakpoint and continue. gud-jump corresponds to DAP's goto facility, but it must be capability-gated. In particular, the current LLDB lldb-dap documentation says supportsGotoTargetsRequest is not supported, while instruction stepping is supported.
So M-F10 should say something like lldb-dap does not support goto rather than silently approximating it.
User customization
The C-only keybind API currently restricts configurable bindings to C-c <key>, so by itself it can't reproduce your F-key configuration.
But there is a nice middle ground: pre-create the "dap" map in C. The existing Lisp define-key path looks up an existing native map before creating one, and keymap_find() already understands Emacs-style -mode-map naming aliases.
So something like this can eventually work:

```
elisp(define-key dap-mode-map (kbd "<f5>") #'dap-step-in)
(define-key dap-mode-map (kbd "<f6>") #'dap-next)
...
```

while none of the debugger implementation depends on Fe.
That is the split I would choose.
The keymap table itself is also only sized for 15 maps, with comments saying that leaves four configuration maps beyond the current built-ins. DAP will consume at least one, so I'd bank some capacity here too.

8. Launch configuration
I would avoid creating language-specific C structs like:

```C
struct lldb_config;
struct debugpy_config;
struct java_config;
```

DAP deliberately leaves launch/attach arguments adapter-specific.
Instead:

```C
struct dap_adapter_spec {
    argv...
    transport;        /* stdio / tcp / special */
};

struct dap_launch_config {
    name;
    adapter;
    request;          /* launch / attach */
    raw_arguments_json;
};
```

Then kg only owns a small set of substitutions such as:

```
${file}
${fileDir}
${workspaceRoot}
```

For example:

```
JSON{
  "name": "C++ debug",
  "adapter": {
    "command": "/opt-2/llvm-21/bin/lldb-dap"
  },
  "request": "launch",
  "arguments": {
    "program": "${workspaceRoot}/build/foo",
    "cwd": "${workspaceRoot}"
  }
}
```

and:

```
JSON{
  "name": "Python current file",
  "adapter": {
    "command": "python",
    "args": ["-m", "debugpy.adapter"]
  },
  "request": "launch",
  "arguments": {
    "program": "${file}",
    "cwd": "${workspaceRoot}",
    "justMyCode": false
  }
}
```

Microsoft's current debugpy adapter source explicitly supports DAP over stdio when it is run without a port, which makes the second model a good fit for the bidi-process code kg already has.
I'd call this file something kg-specific such as .kg-dap.json initially, rather than claiming compatibility with VS Code launch.json. dape makes the same general design choice—it doesn't treat VS Code's launch files as the debugger configuration abstraction.

9. Suggested implementation stages
I think this breaks down into eight clean, reviewable slices.
Stage 0 — reusable infrastructure
No debugger yet.
Modify:

```
src/lsp_json.[ch]       -> src/json.[ch] or equivalent
src/lsp_transport.[ch]  -> compose shared Content-Length framer
src/tty.c
src/keyevent.[ch]
src/keymap.c
src/winmgr.c
```

Deliver:

generic JSON API,

generic framed channel,

generic async fd aggregation,

F5–F12 + modifier decoding,

save/apply/restore window configuration,

capacity increases where justified.

All existing LSP tests must remain green.
This is the riskiest refactoring stage and therefore worth separating from DAP.
Stage 1 — DAP feature axis and transport
New:

```
src/dap.h
src/dap_core.c
src/dap_transport.[ch]
test/fake_dap_adapter.py
test/test_dap_transport.c
```

Add:

```
WITH_DAP ?= 1
kg -V -> +dap / -dap
```

and a WITH_DAP=0 CI lane analogous to LSP.
Test both:

```
WITH_LISP=0 WITH_DAP=1
WITH_LSP=0  WITH_DAP=1
```

DAP should not depend on either.
Stage 2 — protocol client
New:

```
src/dap_client.[ch]
test/test_dap_client.c
```

Implement:

outgoing request seq,

response matching by request_seq,

events,

reverse adapter requests,

deadlines,

capabilities,

disconnect/dead/crash states,

log hook.

Unsupported reverse requests must get a proper failure response rather than being ignored. In particular, runInTerminal can initially be unsupported; adding a real debuggee PTY should not block v1.
Stage 3 — configuration and session startup
New:

```
src/dap_config.[ch]
src/dap_session.[ch]
src/dap_adapter.[ch]    // optional; could live in config initially
```

Implement the actual sequence:

```
spawn/connect
    ↓
initialize
    ↓
launch / attach
    ↓
initialized event
    ↓
setBreakpoints*
setExceptionBreakpoints
configurationDone
    ↓
running
```

DAP specifically requires this initialization/configuration choreography.
Start with:

lldb-dap

python -m debugpy.adapter

plus explicit command overrides for tests and users.
Stage 4 — breakpoints and execution
New:

```
src/dap_breakpoint.[ch]
src/dap_commands.c
```

Commands:

```
dap-debug
dap-disconnect
dap-terminate
dap-continue
dap-pause
dap-next
dap-step-in
dap-step-out
dap-breakpoint-toggle
dap-evaluate
```

Add verified/unverified breakpoint decorations and current-line decoration.
At the end of this stage you should already be able to use kg as a minimal debugger, even before the fancy pane layout exists.
Stage 5 — stopped-state model
Implement:

```
stopped
  └── threads
       └── stackTrace
            └── select frame
                 └── scopes
                      └── variables
```

DAP's recommended flow is essentially this thread/stack/scopes/variables waterfall.
Add:

```
dap-frame-up
dap-frame-down
```

and source navigation/current-line highlighting.
This stage should introduce the stop_epoch stale-response protection.
Stage 6 — GDB-style UI
New:

```
src/dap_ui.[ch]
```

Create/update special buffers and implement the six-pane layout.
I'd use one common DAP-info major map where possible rather than consuming a separate keymap for every special buffer. For example RET can dispatch according to whether the current DAP pane represents a frame, thread, or breakpoint.
F12:

```
normal editor layout
       ⇅
gdb-many-windows layout
```

and exiting the session restores the pre-debug configuration.
Stage 7 — ergonomic parity
Add:

conditional breakpoint,

logpoint,

hit condition,

temporary breakpoint,

until,

restart,

watch expressions,

variable expansion,

better REPL,

per-thread navigation.

This gets you close to the parts of dape that matter most for ordinary source-level debugging. dape currently also includes memory/disassembly support, but I would leave those for later.
Stage 8 — Java and advanced transports
Don't choose the Java architecture yet.
Interestingly, kg already contains a clue: the nbcode-specific LSP transport explicitly distinguishes

```
Java Language Server listening ...
```

from

```
Debug Server Adapter listening ...
```

because the same process can announce both.
That suggests there may eventually be a neat Oracle/NetBeans Java path using a sibling announce/hash transport.
On the other hand, current dape supports Java through JDTLS plus Microsoft's Java Debug Server plugin, so there is also a very different ecosystem path.
The important architectural decision now is therefore simply:

DAP core must know nothing about C++, Python, Java, LLDB, debugpy, or JDTLS.

An adapter spec chooses how to start/connect and passes opaque launch arguments.

10. Testing strategy
I'd copy the philosophy of your LSP work almost verbatim.
Native unit tests
Pure logic:

```
test_dap_client.c
test_dap_breakpoint.c
test_dap_config.c
test_dap_session.c
```

Cases worth explicitly testing:

request/response matching with responses reversed,

event arriving between request and response,

adapter-initiated request,

unknown response sequence,

duplicate response,

malformed message type,

missing seq,

capability parsing,

breakpoint replacement-by-source,

adapter moves breakpoint to another line,

verified → unverified → verified event,

stale variables response after continue,

session ends while callbacks are pending.

Fake-adapter integration tests
test/fake_dap_adapter.py should have scripted modes analogous to the fake LSP server. The existing LSP tests' real-child/deadline approach is exactly right.
Have modes for:

```
normal launch
normal attach
response reverse order
split headers
split bodies
many frames in one write
malformed Content-Length
oversized body
malformed JSON
crash during initialize
crash while stopped
delayed response
runInTerminal reverse request
output flood
breakpoint relocation
multi-thread stop
nested variables
```

PTY acceptance
These are especially important for DAP because a lot of the feature is UI.
For example:

```
open foo.c
F9
start fake adapter
F8
adapter stops at foo.c:17
assert source moved to line 17
assert *dap-stack* contains main/foo
assert *dap-locals* contains x = 42
F6
assert next stop
F12
assert six debugger panes
F12
assert previous layout restored
```

Also test the exact function-key escape sequences, including modified F keys.
Real adapter smoke tests
Skip with a reason when unavailable.
LLDB:
Build:

```bash
c++ -g -O0 ...
```

then test:

```
launch
source breakpoint
continue
stop
threads
stack
locals
evaluate
next
step-in
step-out
disconnect
```

lldb-dap exposes plenty of capability flags, so tests should primarily assert capability-driven behavior rather than hard-code expectations beyond the core protocol.
debugpy:
Temporary Python script:

```
launch through python -m debugpy.adapter
set breakpoint
continue
stack
local variable
evaluate
next
terminate
```

Microsoft's adapter source confirms its no-port mode is a stdio DAP client connection.
Fuzzing
If framing is extracted properly, do not create a second DAP frame fuzzer. Rename/generalize the existing LSP framing fuzz target and run the same bytes against the shared parser.
Add a DAP-specific JSON/message-dispatch fuzzer instead:

```
request / response / event discrimination
seq handling
arbitrary event bodies
breakpoint payloads
stack/scopes/variables payloads
```

And, as with the LSP plan, land implementation and tests together so the branch's complexity/coverage ratchets don't temporarily regress.

What I would consider the first useful milestone
I would resist the temptation to make the first milestone “DAP protocol implemented.”
A much better definition is:

From a C++ or Python file, F9 sets a visible breakpoint, dap-debug launches the program, F8 runs to the breakpoint, kg highlights the current source line, F5/F6/F7 step, F4 evaluates an expression, M-up/M-down navigate frames, and F12 toggles a GDB-style stack/locals/output/breakpoint layout.

Everything before that is infrastructure. Everything after that—conditional/log breakpoints, watches, variable trees, disassembly, memory, runInTerminal, multiple sessions—is enhancement.
The preparatory sequence I would use is therefore:

```
generic JSON/framing
        ↓
generic async polling
        ↓
F5–F12 input + DAP minor keymap
        ↓
window configuration API
        ↓
DAP transport/client
        ↓
launch + breakpoint + stop
        ↓
threads/stack/scopes/variables
        ↓
gdb-many-windows UI
```

That gives you a fairly low-risk path because each of the first four changes improves a subsystem kg already has, and the actual DAP implementation then lands on foundations that are already exercised by LSP rather than creating a parallel mini-editor inside dap.c.
