# DAP support — moved

The reviewed implementation plan lives at
`doc/plans/2026-08-11-dap.md`, with stage detail in `doc/plans/dap/`:

- `doc/plans/dap/00-infrastructure.md` — prerequisites (json/framing
  extraction, async poll seam, F-key/CSI input rewrite, keymap capacity,
  winmgr.h + window configurations, MAX_BUFFERS)
- `doc/plans/dap/01-protocol.md` — feature axis, transport, client,
  session state machine, breakpoints, execution, tests
- `doc/plans/dap/02-ui.md` — panes, layout, keybindings, REPL
- `doc/plans/dap/03-java.md` — Java via nbcode (measured end-to-end);
  jdtls road recorded, deferred
- `doc/plans/dap/04-go.md` — Go via delve (measured end-to-end)

The exploratory ChatGPT draft that previously lived here was reviewed
against the tree and against live traces of lldb-dap 21.1.8 and debugpy
1.8.21 on 2026-08-11; its architecture survived (validated by a C
prototype running the full choreography over kg's unmodified
lsp_transport + lsp_json), its initialization choreography and several
repo claims did not. See the parent plan's "Measured protocol facts" for
what replaced them.
