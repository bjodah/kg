# Plan — the kill ring reaches the prompts (C-y in C-s and M-!)

Written 2026-08-07 from a measured investigation (code map against
HEAD `4882559`, Emacs behaviour against the pinned oracle
`/opt-3/emacs-31-lucid/bin/emacs`, 31.0.90).  Sequencing: **after
Phase 12 of the elisp program closes** — this plan shares the caps
and gates and must not run concurrently.  Every figure below is a
2026-08-07 measurement; the implementing slice re-measures at start
and trusts nothing carried forward.

## What is wrong today (two different things)

kg has two unrelated prompt-input loops, and neither talks to the
real kill ring (`struct kill_ring killring`, `src/yank.h`, 16
entries):

1. **The minibuffer** (`editor_read_line_with_history()`,
   `src/bufmgr.c:1066-1131`; per-key editing in `minibuf_edit_key()`,
   `:858-975`) — used by M-!, M-|, buffer-name and path prompts,
   goto-line — *already binds* C-y and C-k, but against a private
   single-slot `static char minibuf_kill[1024]` (`bufmgr.c:743`).
   Measured Emacs: the minibuffer is an ordinary buffer sharing the
   one global kill ring **in both directions** (kill inside M-!'s
   prompt, C-g out, C-y in the buffer pastes it — verified).  kg's
   prompt kill store is therefore a dead-end sandbox: a
   divergence-from-Emacs, not merely a missing key.  No M-y exists
   in the prompt at all.
2. **isearch** (`do_isearch()`, `src/search.c:452-695`, a separate
   self-contained key loop) — C-y is entirely unbound and silently
   swallowed.  Measured Emacs 31: C-y there is `isearch-yank-kill`
   — **append the newest kill to the search string and re-search
   immediately**; M-y is `isearch-yank-pop-only` — replace the
   just-yanked span with the next-older ring entry and re-search.

## The work

### Part 1 — minibuffer: retarget, don't invent

Retarget `minibuf_edit_key()`'s existing C-y and C-k to the real
`killring` (`kill_ring_get()`/`kill_ring_get_len()`/
`kill_ring_kill_forward()` — all exist, no signature changes
measured necessary), retiring `minibuf_kill` outright.  Add M-y with
a small prompt-local yank-pop state (ring index + inserted span,
plain ints scoped to `editor_read_line_with_history()`'s loop): the
buffer-side `yank_pop_state` gate cannot be reused because
`cmd_clear_transient()` runs at prompt entry (`bufmgr.c:1089`) and
resets `cmd_last_kill_class()`.  This fixes every
`editor_read_line_with_history()` caller at once.

### Part 2 — isearch: append-and-research

New branches in `do_isearch()`'s chain, each factored into a small
static helper in the file's existing style (`isearch_handoff_key`,
`isearch_lookup_move` are the models; the new-function pmccabe cap
is 15): C-y appends the newest kill to the query and re-searches
immediately; M-y replaces the just-appended span with the next-older
entry and re-searches, carried by a loop-local "last keystroke
appended entry N of length L" state.  Deliberately **not** shared
with Part 1 beyond the ring-read primitives — plain insertion and
append-and-research are structurally different, as they are in Emacs
(`yank` vs `isearch-yank-kill`).

No readonly interaction: query-string editing is unaffected by
buffer read-only state (Emacs agrees), and prompt-field keys bypass
`cmd_invoke()` entirely, so `cmdtable` and
`readonly_blocked_keys[]` are not implicated.

### Part 3 — docs

`doc/kg.1`: the shell-command/minibuffer section (near `:1748-1765`)
and the yank entries (near `:2435-2494`) gain the prompt behaviour —
an addition, not a rewrite (prompt-local C-y/C-k is currently
undocumented).  `README.md`: a line under search and shell-command.
`src/help.c` expected unchanged (C-y/M-y already appear;
`make docs-check` only checks help-table keys appear in kg.1) —
verify, don't assume.

## Enumerated tests (PTY-only — `editor_read_line_with_history()` is
stubbed in the native harness for exactly this reason; `oracle:
emacs` is viable in both prompts, precedent
`test/pty/01-isearch-ctrl-a-bol.yaml`)

- M-!: kill in the buffer (C-w), M-!, C-y — command line contains it
  (`expected_screen_contains`); M-y flips to the next-older entry.
- The reverse direction: C-k inside the M-! prompt, C-g out, C-y in
  the buffer — `expected_saved` carries the prompt-killed text (the
  divergence this plan closes).
- isearch: seed two kills; C-s, C-y — query and match position
  assert the newest entry; M-y — the older one, re-searched.
- Newline-containing kill in each prompt.  Emacs measured: M-!'s
  minibuffer grows a line; isearch searches across the real `\n`
  and renders it `^J` in the echo area.  Pin what kg's echo-area
  rendering actually does (a display judgement, made explicitly in
  the case's comment), but the *search semantics* must match.
- Empty kill ring: C-y in each prompt is a non-destructive no-op
  with a "kill ring is empty"-style message.
- Existing minibuffer and isearch PTY cases stay green (the C-k
  retarget changes where a prompt kill lands — audit cases that
  relied on the sandbox, if any).

## Price and funding

At investigation time: kg scc total 5806/5830 — 24 points of
repo-wide headroom; `src/bufmgr.c` is the worst file at 479/520;
`src/search.c` 239/520; `do_isearch` pmccabe 49, `minibuf_edit_key`
27.  Comparable features have cost +5..24 scc.  **Plan for a funded
raise as a recorded Decision with the temporary-lowering proof**,
sized at re-measure time (Phase 12 spends against the same total, so
today's headroom figure is already stale by construction).

## Does not do

- No routing of prompts through real `editor_buffer` machinery —
  Emacs' true architecture, rejected here as disproportionate
  (undo/erow/window plumbing for one-line fields).  Recorded as the
  natural end-state if a future keymap/minibuffer plan wants it.
- No other prompt keys, no Lisp exposure of prompt editing, no
  kill-ring semantic changes outside the two prompts.

## Gates

The standing set, exit-status-checked at every commit: `make check`,
`make complexity-check`, `make pmccabe-check`, format; coverage at
src-touching commits; `make docs-check` after Part 3; final green
light `JOBS=8 .ci/run-ci-steps.sh --parallel` 12/12.
