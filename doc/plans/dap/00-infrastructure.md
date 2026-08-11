# DAP subplan 00 — infrastructure that precedes any DAP code

Parent: `doc/plans/2026-08-11-dap.md`. Every stage here is valuable
independently of DAP, lands with its own tests, and leaves `make check`
plus the ratchets green. Order within this file is dependency order, but
A/B, C/D and E/F are mutually independent and can interleave with review.

## Stage A — json rename and framed_io extraction

`src/lsp_json.[ch]` → `src/json.[ch]` (symbols `lsp_json_*` →
`kg_json_*`, `lsp_jsonw_*` → `kg_jsonw_*`): a mechanical rename — the
module already depends on libc alone (src/lsp_json.h:6-33) — executed
with IWYU (`make iwyu`) driving the include updates. Do not add features
in the rename commit.

Extract `src/framed_io.[ch]` from `src/lsp_transport.c`: the
Content-Length frame reader/writer, bounded inbox/outbox, nonblocking
send/flush/receive, stderr line channel, and the sticky-failure model —
the parts the prototype exercised unchanged. `lsp_transport` keeps: child
spawn policy, the nbcode announce/hash state machine and its
`LSP_TRANSPORT_ANNOUNCE_*` policy strings, and composes framed_io.

Three genuinely new capabilities land on the extracted layer (the
prototype proved these are the only real gaps):

1. **Wrap an already-open fd** as a channel — both the fuzz seam's
   successor (today only the `KG_FUZZ`-gated, read-only
   `lsp_transport_attach_fuzz_fd()` exists, src/lsp_transport.c:884)
   and a real `attach_socket(fd)` constructor (no child, phase OPEN),
   which the Java `lsp-sibling` road needs (subplan 03). Measured
   gotcha: **the constructor sets `O_NONBLOCK` itself** — a caller that
   forgets hangs silently on the first read (the probe lost 8 minutes
   to exactly this).
2. **Nonblocking TCP connect to host:port, no child, no handshake** —
   generalized from the correct-but-static loopback connect at
   src/lsp_transport.c:605-658. v1 may keep loopback-only with the
   restriction documented; the entry point takes a host anyway.
3. **stdin half-close** (graceful end-of-session): send EOF to the child
   without killing it, so a lingering adapter (debugpy never exits after
   `disconnect` — measured) can be given a deadline before the close
   path's kill backstop.
4. **Announce scanning becomes a three-knob scanner.** Today
   `announce_take()` connects to one compile-time prefix
   (src/lsp_transport.c:711-712). The measured needs (subplans 03/04):
   nbcode announces two servers on one stdout and the DAP port must be
   scraped from the *LSP* transport's log channel for a later, separate
   connect; delve announces a bare port with no hash at all. So the
   scanner takes *prefix*, *optional secret separator*, *what to write
   on connect* — delve = (prefix, none, nothing), nbcode = (prefix,
   `" with hash "`, the hash), jdtls = no scan (attach_socket) — and
   reports (prefix, port, hash) tuples upward rather than connecting
   itself. Prefix moves from macro to per-transport data with NULL
   meaning today's LSP prefix, so existing callers are untouched.
   Measured size for all three parameterizations together: 117 lines
   on today's `lsp_transport.{c,h}` (the probe's
   transport-parameterization.diff). While there, fix the comment at
   src/lsp_transport.h:100-104 — the measured sibling line reads
   `Java Debug Server Adapter listening at port ...`, with a leading
   `Java ` the comment omits.

Tests: `test_lsp_transport.c` stays green untouched (the strongest
regression proof of a faithful extraction); new native cases for
wrap-fd-pair, TCP connect (loopback socketpair/listen fixture) and
half-close. Fuzz: retarget `fuzz_lsp_frames.c` → `fuzz_frames.c` at
framed_io; seeds move with it (`test/fuzz-seeds/`). Ratchets: scc raise
with bisect proof; new headers join `make header-check` automatically.

## Stage B — `editor_async_*` at both poll sites

`src/async.[ch]`: `editor_async_wait_fds(int *fds, int max)` and
`editor_async_poll(void)`, today aggregating LSP alone, DAP added in
subplan 01. Call sites: src/tty.c:763-770 + the `KG_IDLE_FD` branch at
src/tty.c:805-811 (whose "a server, and only a server" comment dies), and
src/main.c:236-240. Size the array as a sum with a `static_assert`, the
src/lsp_server.c:517-519 pattern. No callback registry — two subsystems
do not justify one. Note for later: `lsp_transport_wait_fds()` reports
read fds only; a backed-up outbox has no writability wake. Editor-loop
consequence today: none (the tick flushes); record it in async.h rather
than solving it.

## Stage C — the CSI rewrite: F1-F12, Meta modifiers, M-arrows

The current decoder hardcodes `;5`=Ctrl and `;2`=Shift per final byte
(src/tty.c:539-635); Meta (modifier 3+) is decoded nowhere; modified
two-digit params (`ESC[15;5~` = C-F5) fall to `bare_esc()`. Rewrite
`parse_escape()`'s CSI arm as: collect numeric parameters generically,
then decode xterm's `1+bitmask` modifier (Shift=1, Meta=2, Ctrl=4) into
`KEY_MOD_*`, then map the final byte / first param to a base. Table-driven
— every new function ≤ 15 pmccabe. Grow `seq[]` (6 bytes today,
src/tty.c:462) to hold the longest modified form with margin.

- `enum key_base`: add `KEY_BASE_F1..F2`, `F5..F12` (src/keyevent.h:48).
- `key_names[]`: `<f1>`..`<f12>` rows (src/keyevent.c:79-97) — this
  alone makes `(define-key ... "<f5>" ...)` parse, including `C-<f5>`,
  `M-<f10>`, `M-<up>`, since `key_parse()` already composes modifiers
  and `key_base_takes_shift()` already covers named keys.
- Decode both CSI (`ESC[15~`..`ESC[24~`, with modifier variants) and SS3
  (`ESC O P..S` for F1-F4) spellings; M-arrows (`ESC[1;3A` etc.).
- PTY harness: add `F5`-style and modified (`C-F5`, `M-Up`) key tokens
  to utils/pty_accept.py's token table — no token, no DAP PTY case.

Tests: test_keyevent (parse/format round-trips), test_tty (byte-sequence
→ key_event for every new decode, including the two-digit+modifier forms
that regress to bare_esc today), fuzz_keypress seeds extended, and a PTY
case proving an F-key reaches a binding end-to-end. `doc/kg.1` documents
the newly-recognized keys only when something user-visible binds them
(subplan 02).

## Stage D — keymap capacity and the `dap` map's birth

Raise `keymap_max_maps` 15 → 18 and the name pool 2560 → ~3072
(src/keymap.c:13-34), rationale + measured occupancy in the commit
message per house ratchet policy. Create the `dap` map in `dap_init()` at
startup — **never lazily** — because the Lisp `define-key` fallback
creates missing maps at `KEYMAP_LAYER_MAJOR` (src/lisp_cmd.c:733) and an
init.el that runs first would otherwise permanently claim the name in the
wrong layer. (The map exists even in `WITH_DAP=0`? No: stub `dap_init()`
creates nothing; an init.el binding into `dap-mode-map` then creates a
major map that is simply never activated — harmless, and worth one test.)

`KEYMAP_LAYER_MINOR` has zero production users (only test/test_keymap.c
creates minor maps). Before DAP relies on it: native tests for
minor-over-major precedence, minor-map activation/deactivation churn, and
prefix interaction with the global C-c user map.

## Stage E — `winmgr.h` and window configurations

Create `src/winmgr.h` and migrate the ~14 `win_*` declarations out of
def.h:529-548 (house rule; IWYU drives consumers; `make header-check`
picks it up). Then, in winmgr:

- `struct kg_window_configuration` + `win_configuration_save/restore()`.
  Snapshot per-window: buffer identity (by `kg_buffer_handle`, not
  pointer), cursor/scroll, `col_group`, active flag. **Never memcpy
  `struct editor_window`** — the opaque `vgeom` pointer is
  copy-hostile by documented design (src/winmgr.c:452-457, 502-504);
  restore rebuilds windows and lets vgeom repopulate. Restore must cope
  with buffers that died since the save (skip, fall back to scratch —
  decide and test).
- `win_arrange_grid(cols, rows_per_col[], bufs[])` or equivalent:
  construct a 2-column × 3-row arrangement in one call via the
  col_group model (per-group window counts already differ legally,
  src/winmgr.c:168-172). Equal column widths are a v1 limitation of
  `win_reflow()` (src/winmgr.c:158-164), recorded, not fixed here.

Tests: native unit tests for save/restore round-trip including
dead-buffer restore; a PTY case asserting a 2×3 arrangement paints
(tmux backend, `expected_screen_contains` on pane titles).

## Stage F — MAX_BUFFERS

`MAX_BUFFERS` 20 → 32 (src/def.h:330), its own commit: measured cost
~24 KB BSS (1968 B/slot), sibling arrays named in the commit
(src/event.c:38, src/bufmgr.c:2091, src/bufmgr.c:2126 — the 128-byte
namebuf table), suite re-run. No behavior change intended; the buffer-menu
and event-overflow paths get a test poke at the new bound.
