# Plan: Fix `Home` / `End` keys (xterm tilde sequences) + PTY tooling

## 1. Problem Description
kg's `editor_process_keypress` / `editor_move_cursor` already handle
`HOME_KEY` and `END_KEY` correctly for the escape sequences it recognises
(`ESC [ H`, `ESC [ F`, `ESC O H`, `ESC O F`).  Confirmed via a probe case:
sending `M-[` then `H`/`F` moves to bol/eol and inserts as expected.

What is **missing** is parsing of the **xterm tilde** sequences that the
majority of modern terminals send for the dedicated Home/End keys:

- `ESC [ 1 ~` → Home
- `ESC [ 4 ~` → End
- (rxvt also uses `ESC [ 7 ~` Home / `ESC [ 8 ~` End)

`src/tty.c:parse_escape` handles `ESC [ N ~` only for `N` in `{3,5,6}`
(Delete, PageUp, PageDown).  For any other `N` it falls through and returns
`ESC`; the dispatcher then re-reads the next keystroke and tries to treat
`ESC <key>` as an Alt combo, so the following literal key (`X`/`Y`) is
swallowed and reported as an undefined `ESC`-prefixed key.

Probe (fails today):
```yaml
initial: |
  hello world
  foo bar baz
keys: [C-n, 'M-[', '4', '~', X, C-p, 'M-[', '1', '~', Y]
expected_saved: |
  Yhello world
  foo bar bazX
```

Additionally `utils/pty_accept.py` has no named tokens for Home/End, so a
case author cannot write `Home`/`End`; the token falls through to
`token.encode("utf-8")` and is sent as the literal text "Home"/"End".  That
makes writing regression tests for these keys awkward.

## 2. Design

### 2.1 Recognise the tilde sequences in `src/tty.c:parse_escape`
In the `if (seq[2] == '~') { switch (seq[1]) { … } }` block (currently only
`'3'`, `'5'`, `'6'`), add:
```c
case '1':
	return HOME_KEY;
case '4':
	return END_KEY;
case '7':
	return HOME_KEY;   /* rxvt Home */
case '8':
	return END_KEY;    /* rxvt End */
```
No new key codes are needed: these map onto the existing `HOME_KEY` /
`END_KEY`.  The dispatcher (`case HOME_KEY` / `case END_KEY` in `kbd.c`) and
`editor_move_cursor` already do the right thing.  The `;`-modified variants
(`ESC [ 1 ; 5 H` → `CTRL_HOME`, etc.) are already handled in the
`seq[2] == ';'` branch, so the new plain-tilde cases do not collide.

### 2.2 Add named Home/End tokens to `utils/pty_accept.py`
Extend the recognised token tables in `token_to_bytes`,
`send_token_pexpect`, and `tmux_key_name` so that:
- `Home` → `\x1b[1~` (pexpect) / tmux key `Home`
- `End`  → `\x1b[4~` (pexpect) / tmux key `End`
- `C-Home` → `\x1b[1;5H` / tmux `C-Home`
- `C-End`  → `\x1b[1;5F` / tmux `C-End`
- `S-Home` → `\x1b[1;2H` / tmux `S-Home`
- `S-End`  → `\x1b[1;2F` / tmux `S-End`

Use the xterm tilde forms (`ESC [ 1 ~`, `ESC [ 4 ~`) for the plain Home/End
so the new additions are actually exercised by the tests (rather than the
already-working `ESC [ H`/`F`).  For the modified variants, the
`ESC [ 1 ; N <letter>` form is what kg already parses, so emit those
(`\x1b[1;5H`, `\x1b[1;5F`, `\x1b[1;2H`, `\x1b[1;2F`).

### 2.3 Add PTY regression tests
Two new cases under `test/pty/`:

- `home-end-keys.yaml` — multi-line buffer, drive `End`, `X`, `C-p`, `Home`,
  `Y`, `C-n`, `C-Home` (→ beginning of buffer), `C-End` (→ end of buffer),
  assert the saved file.  Use the new `Home`/`End`/`C-Home`/`C-End` tokens so
  both the parses and the tooling are covered.
- `home-end-long-line.yaml` — a single line longer than the viewport
  (`dimensions: [10, 20]`) so `End` must also scroll `coloff`; assert the
  cursor lands after the last char and a following typed char appends.  This
  guards the long-line branch of `case END_KEY` in `src/basic.c`.

Keep cases minimal and deterministic (no oracle, no `xfail`).

### 2.4 Documentation
- `doc/kg.1`: the Home/End entries already document the action; no change
  needed unless the table lists recognised terminals — it does not, so skip.
- `AGENTS.md`: add a short note that individual PTY cases can be run with
  `python3 utils/pty_accept.py --kg src/kg <case.yaml>` (see the separate
  AGENTS.md update task), and that Home/End now have named tokens.

## 3. Verification
1. `make` builds.
2. The probe case above PASSes once 2.1 lands.
3. New PTY cases PASS with 2.1 + 2.2.
4. `make check` stays green (no existing case uses the literal strings
   "Home"/"End", so adding the named tokens is safe).
5. Run with a real-ish `TERM` if iterating manually outside `make check`.

## 4. Risks / Notes
- Do not remove the existing `ESC [ H` / `ESC [ F` / `ESC O H` / `ESC O F`
  parsing; many terminals still send those.  The new tilde cases are purely
  additive.
- Keep the change in `parse_escape` to a couple of `case` labels — no
  restructuring — to stay within the complexity budget and match the
  file's tab style.
- `pty_accept.py` edits are Python; keep them formatting-neutral (the file
  uses tabs, so match that).