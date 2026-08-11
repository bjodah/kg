# DAP subplan 00 — infrastructure that precedes any DAP code

Parent: `doc/plans/2026-08-11-dap.md`. Every stage here is valuable
independently of DAP, lands with its own tests, and leaves `make check`
plus the ratchets green. Order within this file is dependency order, but
A/B, C/D and E/F are mutually independent and can interleave with review.

## Stage A — json rename and framed_io extraction

At least three separate commits: the JSON rename, the
behavior-preserving framing extraction, and the new
channel/connect/announce features.

`src/lsp_json.[ch]` → `src/json.[ch]` (symbols `lsp_json_*` →
`kg_json_*`, `lsp_jsonw_*` → `kg_jsonw_*`): a mechanical rename — the
module already depends on libc alone (src/lsp_json.h:6-33) — executed
with IWYU (`make iwyu`) driving the include updates. Do not add features
in the rename commit. The JSON capabilities subplan 01's config stage
needs — serialize a parsed value, emit a finite number, reject duplicate
object keys, transform strings recursively — land **there**, with tests
for `-0`, exponents, quote/backslash/control escaping, embedded NUL,
non-finite rejection and exact null-vs-absent behavior; they do not
belong in the rename.

Extract `src/framed_io.[ch]` from `src/lsp_transport.c`: the
Content-Length frame reader/writer, bounded inbox/outbox, nonblocking
send/flush/receive, stderr line channel, and the sticky-failure model —
the parts the prototype exercised unchanged. `lsp_transport` keeps: child
spawn policy, the nbcode announce/hash state machine and its
`LSP_TRANSPORT_ANNOUNCE_*` policy strings, and composes framed_io.

The extraction ships with an explicit ownership contract, stated in
framed_io.h, so framed_io, lsp_transport and dap_transport can never
each believe they own the same socket or child:

- the constructor takes ownership of its fds and sets `O_NONBLOCK` and
  `FD_CLOEXEC` on every distinct one; passing the same fd as both read
  and write side (a socket) is legal and it is closed exactly once;
- half-close is `close()` of a pipe write fd but `shutdown(SHUT_WR)`
  for a bidirectional socket, so reads stay possible; a second
  half-close is harmless;
- EOF is recorded only after already-buffered complete frames are
  delivered, and EOF with a partial frame pending is a protocol
  failure, not a quiet end;
- a borrowed message stays valid until the next receive/close call
  (the existing `lsp_transport_next_message()` rule, restated);
- framed_io owns framing only: it never reaps a child, parses an
  announce, labels stderr, or decides a session ended;
- the existing 32 MiB frame / 4 MiB outbox bounds carry over unchanged
  unless a measured adapter requires otherwise.

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
4. **Announce parsing separates from connecting.** Today
   `announce_take()` connects to one compile-time prefix
   (src/lsp_transport.c:711-712). The measured needs (subplans 03/04):
   nbcode announces two servers on one stdout and the DAP port must be
   scraped from the *LSP* transport's log channel for a later, separate
   connect; delve announces a bare port with no hash at all. The scan
   becomes a pure incremental line scanner producing an endpoint —
   `struct kg_announced_endpoint { host; port; secret[]; secret_len; }`
   — configured per prefix with whether a secret separator
   (`" with hash "`) is required and strict host/port rules; the
   *connection owner* decides when to connect and whether to write the
   secret first. Endpoints are **cached on the owning transport, tagged
   with the producing child's generation** — a callback-only "report
   upward" design loses the common late-subscriber case, since Java
   debugging typically starts long after the LSP announce scrolled by —
   and an endpoint is invalidated when that child dies or restarts,
   never blindly reconnected. Prefix moves from macro to per-transport
   data with NULL meaning today's LSP prefix, so existing callers are
   untouched. Measured size for all three parameterizations: 117 lines
   on today's `lsp_transport.{c,h}` (the probe's
   transport-parameterization.diff), plus the caching. Secret bytes are
   redacted from user-visible logs. While there, fix the comment at
   src/lsp_transport.h:100-104 — the measured sibling line reads
   `Java Debug Server Adapter listening at port ...`, with a leading
   `Java ` the comment omits.

One wait-seam limitation is accepted, not solved: the editor wait
carries readable fds only, and a nonblocking connect-in-progress or a
full outbox becomes *writable*. v1 lets those complete on the 100 ms
idle tick — with the added latency measured in a test — and completion
is always checked with `getsockopt(SO_ERROR)`, never inferred from
writability. The `struct pollfd`-carrying generalization of the seam is
recorded as a follow-on in the parent plan.

Tests: `test_lsp_transport.c` stays green untouched (the strongest
regression proof of a faithful extraction); new native cases for
wrap-fd-pair (separate pipe fds *and* one shared socket fd), TCP connect
(loopback listen fixture), half-close (both fd shapes, double
half-close), and the announce scanner: prefix split across reads, bare
line + hash line in one read, two different announces in one stream,
CRLF, EOF after a partial line, log text around the announce,
invalid/zero/overflow ports, empty/oversized hash, and a stale endpoint
after child replacement. Fuzz: retarget `fuzz_lsp_frames.c` →
`fuzz_frames.c` at framed_io; seeds move with it (`test/fuzz-seeds/`),
and no orphaned target remains. Ratchets: scc raise with bisect proof;
new headers join `make header-check` automatically.

## Stage B — `editor_async_*` at both poll sites

`src/async.[ch]`: `editor_async_wait_fds(int *fds, int max)` and
`editor_async_poll(void)`, today aggregating LSP alone, DAP added in
subplan 01. Call sites — all three: src/tty.c:763-770, the `KG_IDLE_FD`
branch at src/tty.c:805-811 (whose "a server, and only a server" comment
dies), and src/main.c:236-240. Size the array as a checked sum with a
`static_assert` (the src/lsp_server.c:517-519 pattern), and either
deduplicate identical fds or state that the registries can never share
one; a bounds failure is a compile-time assertion, never a silently
omitted fd. `editor_async_poll()` keeps the existing return convention —
nonzero only when a repaint is needed — and never drains the editor
event queue from inside a prompt: DAP actions that select buffers,
arrange windows or edit source test `kg_event_prompt_active()` and defer
to the top-level safe point. Tests: a fake DAP event arriving while kg
blocks for input, and while a minibuffer prompt is open. No callback
registry — two subsystems do not justify one. Note for later:
`lsp_transport_wait_fds()` reports read fds only; a backed-up outbox has
no writability wake (see stage A's accepted wait-seam limitation);
record it in async.h rather than solving it.

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
- Decode every common spelling: SS3 `ESC O P..S` for F1-F4, the CSI
  tilde forms `ESC[11~`..`ESC[14~` for F1-F4 (the review caught that
  omitting these would advertise F1-F12 while failing a common F1-F4
  spelling; F3/F4 already decode `13~`/`14~` today), and
  `ESC[15~,17~..21~,23~,24~` for F5-F12 — 16 and 22 are gaps, not
  function keys — each with the full `;<1+bitmask>` modifier range
  (values 2-8); M-arrows (`ESC[1;3A` etc.). Accept empty/default CSI
  parameters where terminals send them; reject parameter overflow,
  excess parameter count and colon subparameters; an overlong or
  incomplete sequence must not consume a later ordinary key. The
  lone-ESC timeout behavior is preserved.
- PTY harness: add `F1`-`F12` and modified (`C-F5`, `M-Up`,
  `PageUp`/`PageDown`) key tokens to utils/pty_accept.py — in **one**
  named-key table shared by the pexpect and tmux backends (the current
  code spells several tokens twice), with a direct `token_to_bytes()`
  unit test so a future harness edit cannot silently send the literal
  letters `F5`. No token, no DAP PTY case.

Tests: test_keyevent (parse/format round-trips), test_tty (byte-sequence
→ key_event for every new decode, including the two-digit+modifier forms
that regress to bare_esc today), fuzz_keypress seeds extended, and a PTY
case proving an F-key reaches a binding end-to-end. `doc/kg.1` documents
the newly-recognized keys only when something user-visible binds them
(subplan 02).

## Stage D — keymap capacity and minor-layer proving

This stage raises and *measures* capacity and proves the minor layer; it
creates no DAP map — subplan 00 must be independently buildable, and
`dap_init()` first exists in subplan 01 stage 1, which is where the
`dap`, `dap-breakpoint` and `dap-info` maps are created (before init.el
loads, and immediately deactivated: `keymap_create()` makes a map active
from the start, and an active-at-startup map would shadow global keys
before DAP owns any state; the startup creation matters because the Lisp
`define-key` fallback creates missing maps at `KEYMAP_LAYER_MAJOR`,
src/lisp_cmd.c:733, and an init.el that runs first would permanently
claim the names in the wrong layer. In `WITH_DAP=0` the stub
`dap_init()` creates nothing; an init.el binding into `dap-mode-map`
then creates a never-activated major map — harmless, and worth one
test.)

Raise `keymap_max_maps` 15 → 18 and the name pool 2560 → ~3072
(src/keymap.c:13-34), rationale + measured occupancy in the commit
message per house ratchet policy. Occupancy is measured through new
test-only accessors (maps used, entries used, name-pool bytes used),
not calculated from comments. The built-in tables hold ~167 entries
today and DAP's plans name 18+ commands plus pane bindings, so the
256-entry pool is a bound to re-measure, not assume. The capacity test
that matters is user-shaped: after the eventual DAP defaults install
(subplan 01), it must still be possible to create at least four user
maps and bind a command in each — capacity exists for user
configuration, not merely for kg to start.

`KEYMAP_LAYER_MINOR` has zero production users (only test/test_keymap.c
creates minor maps). Before DAP relies on it: native tests for
minor-over-major precedence, minor-map activation/deactivation churn, and
prefix interaction with the global C-c user map.

## Stage E — `winmgr.h` and window configurations

Create `src/winmgr.h` and migrate the ~14 `win_*` declarations out of
def.h:529-548 (house rule; IWYU drives consumers; `make header-check`
picks it up). Then, in winmgr:

- `struct kg_window_configuration` + `win_configuration_save/restore()`.
  Snapshot per-window every field restoration needs: buffer identity
  (by `kg_buffer_handle`, never pointer), `cx`, `cy`, `rowoff`,
  `coloff`, `rowoff_visual`, `desired_visual_col`, `col_group`, active
  slot — plus the selected-window identity and the count. Geometry
  (`x/y/h/w`) is derived by `win_reflow()` and `vgeom` is
  discarded/rebuilt: **never memcpy `struct editor_window`**, even
  temporarily — the opaque `vgeom` pointer is copy-hostile by
  documented design (src/winmgr.c:452-457, 502-504); a bulk API builds
  fresh records field by field.
- Save/restore go through the same lifecycle operations as ordinary
  window changes: detach old buffer views (publishing detach events),
  claim fresh window identities rather than reviving stale ones, attach
  restored buffers (publishing attach events), keep `buf_current`
  synchronized with `win_current`, free/invalidate old vgeom indexes,
  and end with a reflow + handle check.
- Restoration is **atomic**: validate everything, then mutate. If the
  saved selected buffer is dead, select the first surviving saved
  window; if none survives, show `*scratch*` (created only if capacity
  permits). Never leave half the new layout and half the old after a
  failure.
- `win_arrange_grid(cols, rows_per_col[], bufs[])` or equivalent:
  construct a 2-column × 3-row arrangement in one call via the
  col_group model (per-group window counts already differ legally,
  src/winmgr.c:168-172). It validates everything — duplicate/dead
  buffer handles, window count, a terminal too small for three rows of
  mode lines — *before* touching the live layout, and returns a status
  the caller (F12) can report. Equal column widths are a v1 limitation
  of `win_reflow()` (src/winmgr.c:158-164), recorded, not fixed here.

Tests: native save/restore round-trip; resize between save and restore;
the selected saved buffer killed; an unselected saved buffer killed;
arrange at the minimum dimensions and at one-less-than-minimum (clean
refusal); a PTY case asserting a 2×3 arrangement paints (tmux backend,
`expected_screen_contains` on pane titles).

## Stage F — MAX_BUFFERS

`MAX_BUFFERS` 20 → 32 (src/def.h:330), its own commit. The 1968 B/slot
`buflist` growth (~23.6 KB) is a *lower bound* on the cost, not the
total: the 128-byte namebuf table (src/bufmgr.c:2126, +1.5 KB), the
event-overflow array (src/event.c:38), the order/ring arrays
(src/bufmgr.c:2091) and the Lisp per-buffer point table (when built)
all grow with it — so the commit reports `size`/BSS before and after
for both `WITH_LISP=1` and `WITH_LISP=0` rather than multiplying one
struct. Tests exercise the new *last* slot through open, display,
event overflow, Lisp point (when enabled), kill and reuse — counting to
32 alone does not touch every sibling array indexed by the bound.
