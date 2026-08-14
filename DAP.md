## DAP (Debug Adapter Protocol) in kg

A binary built without it still has every `dap-*` command, and each one
answers "kg was built without DAP support" rather than leaving `M-x` to
report an unknown command.

Adapters are found on `PATH` at run time; nothing is downloaded or
installed, the same rule the LSP client follows. Four ship built in:
`lldb-dap` for C and C++, `python3 -m debugpy.adapter` for Python, `dlv
dap` for Go, and `nbcode-java` for Java — which is not a command at all but
the debug adapter the Java language server already announces.

An adapter kg starts, and the program it runs, get no controlling
terminal — kg's own belongs to kg. The program's output arrives as
protocol output events and shows up in `*dap-output*`; a program that
wants a terminal of its own is not one this version can launch under the
debugger, and attaching to it while it runs elsewhere is the way to debug
one.

The keys are gud's, so an Emacs user's fingers already know them:

| Key | Command | What it does |
| --- | --- | --- |
| `F9` / `C-F9` | `dap-breakpoint-toggle` / `-temporary` | Set or clear one |
| `F4` | `dap-evaluate` | Evaluate an expression in the selected frame |
| `F5` / `C-F5` | `dap-step-in` / `dap-step-instruction` | Step into, one instruction |
| `F6` / `F7` | `dap-next` / `dap-step-out` | Step over, step out |
| `F8` | `dap-continue` | Let the program run until it stops again |
| `F10` / `M-F10` | `dap-until` / `dap-goto` | Run to this line; jump to it |
| `F11` / `M-F11` | `dap-restart` / `dap-terminate` | Restart; end it and the program |
| `F12` | `dap-many-windows` | Toggle the debug layout, and restore it |
| `M-up` / `M-down` | `dap-frame-up` / `dap-frame-down` | Walk the stack |
| `PageUp` / `PageDown` | the same two | Walk the stack |
| — | `dap-debug` | Start a session from a launch configuration |
| — | `dap-pause` | Ask a running program to stop |
| — | `dap-repl` | Evaluate an expression in `*dap-repl*` |
| — | `dap-disconnect` | End the session, leaving the program running |

`F9` and `C-F9` work in any buffer visiting a file, with nothing running —
setting a breakpoint before starting is the ordinary first step. Every
other key is live **only while a session exists**, so outside one they are
still your own keys. `dap-debug` is reachable through `M-x` whatever the
keys say.

Both key tables are ordinary native keymaps, so init.el can rebind them:

```elisp
;; VS Code habits, say.
(define-key 'dap-mode-map "<f5>" 'dap-continue)
(define-key 'dap-mode-map "<f11>" 'dap-step-in)
(define-key 'dap-breakpoint-mode-map "<f9>" 'dap-breakpoint-toggle)
```

`global-set-key` stays `C-c`-only by design; F-keys are reachable through
`define-key` precisely because these are real native maps (see
`doc/lisp-api.md`).

All the commands are Lisp-callable; the four that prompt (`dap-debug`,
`dap-evaluate`, `dap-goto`, `dap-repl`) are refused from a Lisp activation
with no terminal. None of them edits buffer text, so all of them work in a
read-only buffer — debugging somebody else's source is the ordinary case.

**The panes.** `F12` arranges six windows — `*dap-repl*` and `*dap-locals*`
on the top row, the source and `*dap-output*` in the middle, `*dap-stack*`
and `*dap-breakpoints*` below — and `F12` again puts your own windows back.
A terminal too small for six windows refuses cleanly and keeps the layout
you have. `*dap-breakpoints*` and `*dap-threads*` share the bottom-right
slot, as GDB's do, and `dap-info-toggle-breakpoints-threads` (`t` in the
pane) swaps them.

In any of the six panes, `RET` acts on **what the line is**: a frame is
selected and its source visited, a thread line reports which thread kg is
looking at (there is no thread switch yet, and the line says so rather than
pretending), a breakpoint is visited, and a variable is expanded or
collapsed. `d` deletes the
breakpoint on the line, `D` enables or disables it — a disabled breakpoint
is one kg keeps and the adapter is never told about — and `q` buries the
pane. A line from an earlier stop is refused rather than acted on: every
adapter measured answers a stale frame or variable handle with success and
silently wrong data.

`*dap-output*` and `*dap-repl*` are **transcripts**: append-only, bounded
at 4 MiB each, with one visible marker where they cut. The debuggee's bytes
are untrusted and reach the screen only through kg's own glyph spelling, so
an escape sequence in a program's output is displayed rather than obeyed.
The REPL's input is the minibuffer — `dap-repl` prompts with `DAP> `, echoes
the expression into the transcript and appends the answer beside it, since
answers can arrive out of order.

**Breakpoints belong to the editor, not to a session.** One can be set with
nothing running, it survives the session that verified it, and it is
anchored to the text in an open buffer so an edit above it takes it along.
They are marked in the first column of their line, in one colour when an
adapter has verified them and a dimmer one when none has; the line the
program is stopped on is marked across its width. All three marks are
projections of the tables and are rebuilt when a buffer is opened and when
a session ends.

**Launch configurations live in `.kg-dap.json`**, in the nearest ancestor
directory of the file being debugged; that directory is what
`${workspaceRoot}` means. With no such file, kg's built-in configurations
apply.

```json
{ "version": 1,
  "configurations": [
    { "name": "Python current file",
      "adapter": "debugpy",
      "request": "launch",
      "build": { "command": "make", "cwd": "${workspaceRoot}" },
      "arguments": { "program": "${file}", "cwd": "${workspaceRoot}" } } ] }
```

`adapter` is a built-in name or an inline object (`command`, `args`,
`transport`, `cwd`). The optional `build` step runs through kg's ordinary
compilation machinery — same `*compilation*` buffer, same diagnostics, same
`C-c C-k` — and the launch happens only if it exited zero. `arguments` is
passed to the adapter untouched, with five substitutions made inside its
strings: `${file}`, `${fileDir}`, `${fileUri}`, `${workspaceRoot}` and
`${env:NAME}`. The set is closed and an unknown one is an error rather than
an empty string — `${workspaecRoot}/prog` expanding to `/prog` is a
debugger that silently debugs the wrong program. `${fileUri}` is `${file}`
as a `file:` URI, for the one adapter that parses that argument as a URI
rather than as a path; a relative buffer name is made absolute against kg's
working directory first, since a URI cannot be relative.

Four adapters are built in: `lldb-dap`, `debugpy` and `delve`, all found on
`PATH` at run time, and `nbcode-java`, which is not a command at all.

### Java

kg debugs Java through the language server it is already talking to. One
`nbcode` process announces a Java language server and a Java debug adapter
on one standard output (see *Java setup* above), and kg's `nbcode-java`
adapter is the second of those — so `M-x dap-debug` in a `.java` buffer
starts, or reuses, the same nbcode your `M-.` uses, waits for its language
session to finish initializing, and then opens a second socket to it.

That ordering is not a preference. The debug connection captures the
language session's state when it is made, so kg will not connect before the
language server is up; a slow start says which step it is on in the echo
area, and `M-x dap-disconnect` stops waiting.

The built-in `Java (nbcode)` configuration debugs the file under the cursor
with no project setup at all:

```json
{ "name": "Java (nbcode)",
  "adapter": "nbcode-java",
  "request": "launch",
  "arguments": { "type": "jdk", "file": "${fileUri}",
                 "classPaths": ["any"], "console": "internalConsole" } }
```

`transport: "lsp-sibling"` with an `lspLanguage` is how an inline adapter
asks for the same wire. The port and the handshake are not a user's to
repeat: they are the built-in spec's, and kg gets them from the language
server.

Two consequences worth knowing. **Ending a Java debug session leaves nbcode
running** — it is your language server, and `dap-disconnect` closes one
socket and nothing else; sequential debug sessions share the one process.
And nbcode never sends the protocol's `terminated` event, so kg infers the
end of a run from the debuggee's threads going away and disconnects after a
short grace period in which late output is still collected. A new thread or
a stop cancels that inference.

If `KG_LSP_SERVER_JAVA` points at a server that is not nbcode — jdt.ls, say
— there is no debug adapter to reach, and `dap-debug` says so rather than
starting something.

### Go

`M-x dap-debug` in a `.go` buffer offers `Go (delve)`, which debugs the
package the file is in:

```json
{ "name": "Go (delve)",
  "adapter": "delve",
  "request": "launch",
  "arguments": { "mode": "debug", "program": "${fileDir}",
                 "cwd": "${fileDir}", "stopOnEntry": false } }
```

`mode: "debug"` is delve building the package itself and debugging what it
built, which is why `program` is a directory rather than a binary — and why
you get readable locals for free, since delve passes
`-gcflags="all=-N -l"` on that path. Nothing is installed for you: `dlv`
and the Go toolchain have to be on `PATH`, and `KG_DAP_ADAPTER_DELVE`
replaces the command line for a delve somewhere else.

`mode: "exec"` debugs a binary you built, and `program` is then that
binary. Build it with

```bash
go build -gcflags="all=-N -l" -o prog .
```

or the locals will be missing or wrong and the only thing that says so is
the scope's own name, which kg shows verbatim:
`Locals (warning: optimized function)`. `mode` also takes `test`,
`replay` and `core`.

Three things are worth knowing about Go sessions.

**Your program's output arrives on delve's own standard error**, not as
protocol output events, so kg routes that channel into `*dap-output*` as
raw bytes — no newline is invented at a read boundary, and a program that
printed CRLF or stopped mid-line looks like it did.

**A failed build lands in `*compilation*`.** The launch response says only
"Check the debug console for details"; the real `./main.go:8:14: undefined:
...` came earlier, so kg keeps launch-phase output and, when the launch is
refused, feeds it through the ordinary compilation machinery — `M-g n`
walks the errors like any compiler's. A compilation of your own that is
already running keeps the buffer, and the output stays in `*dap-output*`.

**delve runs where kg's configuration says, not where kg is.** It resolves
`program` and writes its `__debug_bin` artefact relative to its own working
directory, so the built-in `delve` adapter runs in `${workspaceRoot}`. An
inline adapter's `cwd` overrides that; it is the ADAPTER's directory and
has nothing to do with `arguments.cwd`, which is the program's. kg never
deletes delve's build artefacts — they are delve's to clean up.

**`.kg-dap.json` is trusted code.** It names a command to spawn and
arguments handed to a debugger that will run a program; there is nothing to
sandbox and no pretence of one, the same story as `init.el`. The validation
is there for the other failure — a mistyped file that would otherwise start
the wrong thing without saying so.
