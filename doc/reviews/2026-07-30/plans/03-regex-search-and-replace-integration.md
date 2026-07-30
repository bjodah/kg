# Plan 03 — Regex search and replacement integration

## Goal

Make kg's user-facing regexp operations correct, terminating, and honest about
engine failure before changing the regex engine architecture.

It covers zero-width UTF-8 replacement loops, start offsets inside glyphs,
matcher exhaustion reported as no-match, regexp isearch using rendered instead
of buffer text, byte-at-a-time replacement with repeated row rebuilds, and the
missing operation-sequence, cancellation and differential coverage.

Engine parser/repetition/reentrancy changes are Plan 06. The generic row
range-replacement primitive is Plan 08 phase 5; removing the global
`suppress_undo` is Plan 10.

## Verification status (checked against source 2026-07-30)

| Claim | Status | Evidence |
| --- | --- | --- |
| Zero-width replace-all can loop forever on UTF-8 | Confirmed | `src/search.c:948` `match_col = match_start + expanded_len + (match_len == 0 ? 1 : 0)`; `+1` lands on a continuation byte, `kg_span_snap()` (`src/regex.c:41`) pulls the next match start back to the glyph start |
| Same loop grows memory with a non-empty replacement | Confirmed | each pass inserts the replacement and pushes an undo record; `count` and `editor.dirty` rise without bound |
| Answering `n` repeatedly never advances either | Confirmed | `src/search.c:952` uses the same `+1` |
| Matcher exhaustion reported as no-match | Confirmed | `src/regex.c:126` and `:151` map every non-`RE_STATUS_OK` to `KG_REGEX_NOMATCH`, including `RE_STATUS_TOO_COMPLEX` (`fe/tiny-regex-c/re.c:447`) |
| The project already works around it | Confirmed | `test/regex_differential.c:38-46` `exec_ran_out()` calls `re_exec()` directly "because the engine folds an exhausted step budget into no match" |
| Regexp isearch matches rendered text | Confirmed | `src/search.c:117` and `:120` pass `row->render` |
| Query replace uses buffer text | Confirmed | `src/search.c:642` and `:867` pass `row->chars` |
| Query replace is byte-at-a-time | Confirmed | `src/search.c:707-713` (literal) and `:936-942` (regexp); each `editor_row_insert_char()`/`editor_row_del_char()` reallocs, memmoves and calls `editor_update_row()` (`src/buffer.c:748`, `:892`, `:259`) |
| `KG_REGEX_TOODEEP` is compile-only | Confirmed, plan text corrected | `src/regex.c:107` is the only producer; no runtime path reports it |

New finding, folded into phase 4: after a match `do_isearch()` calls
`editor_reveal_position_centered(match_row, point_col)` (`src/search.c:584`)
with `point_col` a **render** offset, while `editor.coloff + editor.cx` is a
**chars** byte offset everywhere else (`editor_current_filecol()`,
`src/buffer.c:58`). On a row with a tab or a multi-byte glyph, isearch leaves
point at the wrong buffer column; existing PTY fixtures miss it because they
are tab-free ASCII, where render equals chars.

## Read first

- `src/regex.c` (185 lines), `src/regex.h` (37 lines)
- `src/search.c`: `isearch_find_match()` (:84), `do_isearch()` (:386),
  `editor_query_replace()` (:602), `editor_query_replace_regexp()` (:828),
  `expand_replacement()` (:782)
- `src/def.h:656-740` — `utf8_is_cont()`, `utf8_glyph_span_at()`,
  `utf8_glyph_start_before()`, all `static inline`; `src/mode.c:4`
  `chars_to_render_col()` (`render_col_to_chars()` at `:195` is visual-line
  aware, not a plain inverse)
- `fe/tiny-regex-c/re.h` header comment (byte offsets, `^` semantics),
  `re.c:403` `re_exec()`
- `test/test_regex.c`, `test/fuzz_regex.c`, `test/regex_differential.c`,
  `utils/regex_differential.py`, `utils/regex_oracle.el`
- `test/pty/query-replace-regexp*.yaml`, `test/pty/isearch-regexp.yaml`,
  `test/pty/132-query-replace-regexp-caret-anchor.yaml`

## Contracts to write down first

Add comments to `src/regex.h` stating: spans and offsets are byte offsets into
`text`; public start/before offsets must sit on a kg glyph boundary; the
wrapper normalizes an off-boundary offset *forward* and never returns a match
starting before the normalized request; empty matches are valid; callers
iterating matches must use the provided progress helper; runtime exhaustion is
distinct from compile failure and from no-match; embedded NUL is unsupported
until an explicit-length API lands (`re_exec()` takes a NUL-terminated
subject). Status values are `#define`s today, not an enum — add one value, and
convert to an enum later in a separate mechanical commit if wanted, since
callers compare `int status` against them.

```c
#define KG_REGEX_TOODEEP     3 /* compile ran out of budget */
#define KG_REGEX_TOO_COMPLEX 4 /* a match attempt ran out of budget */
```

`KG_REGEX_TOODEEP` is produced only by `kg_regex_compile()` (`src/regex.c:107`)
and is not ambiguous today, so keep it. `test/regex_differential.c:57` switches
on it and must be updated in the same commit as any rename.

## Phase 1 — Normalize offsets and add a shared iterator step

Files: `src/regex.c`, `src/regex.h`, `test/test_regex.c`.

### Changes

Add, in `src/regex.c` with prototypes in `src/regex.h`:

```c
/* First glyph boundary at or after `requested`, clamped to [0, len].
 * A stray continuation byte is a one-byte glyph, matching
 * utf8_glyph_span_at(). */
int kg_utf8_forward_boundary(const char *text, int len, int requested);

/* Offset at which to resume scanning after `match`.  Returns -1 when the
 * subject is exhausted (an empty match at `len`).  Never returns a value
 * <= match->start. */
int kg_regex_next_offset(
    const char *text, int len, const struct kg_span *match);
```

`kg_utf8_forward_boundary()` clamps to `[0, len]`; if `text[requested]` is a
continuation byte of a complete glyph starting earlier it moves **forward** to
that glyph's end, never backward; malformed bytes stay one-byte units; and it
is idempotent (`f(f(x)) == f(x)`).

Call it on `start_offset` in `kg_regex_match_forward()` before `re_exec()`, and
on the scan cursor in `kg_regex_match_backward()`. That is what makes the
existing `kg_span_snap()` defence harmless: once the request is on a boundary,
snapping can no longer produce a span that starts before it. Keep
`kg_span_snap()`; it still rejects out-of-bounds engine output.

`kg_regex_next_offset()` returns `match->end` for a consuming match,
`start + utf8_glyph_span_at(text, len, start)` for an empty match with
`start < len`, and `-1` for an empty match at `len`.

Also fix `kg_regex_match_backward()`'s scan step (`src/regex.c:175`), which
currently does `offset = max(start, offset) + 1` — a byte-wise `+1` with the
same mid-glyph problem, and an O(n) `re_exec()` per byte, so backward search on
one row is O(n²). Use `kg_regex_next_offset()` there, falling back to
`kg_utf8_forward_boundary(text, len, offset + 1)` when the helper returns the
same offset, and break on `-1`.

### Pitfalls

- `regex.c` may only depend on what the standalone binaries link: the fuzz and
  differential targets compile **only** `src/regex.c` + `fe/tiny-regex-c/re.c`
  (`$(FUZZBIN_REGEX)` and `$(REGEX_DIFF_BIN)` rules). The UTF-8 helpers are
  `static inline` in `src/def.h`, so they are safe; a new helper in
  `src/width.c` or `src/buffer.c` breaks `make fuzz-regex-smoke` and
  `make check-regex-differential` with a link error, not a test failure.
- `re_exec()` rejects `start_offset > strlen(text)` with `RE_STATUS_NO_MATCH`
  (`re.c:414`), so `-1` must never reach it.
- `^` anchors at `text`, not at `start_offset` (`re.h` header comment). Do not
  "fix" that while here; PTY case 132 pins it.

### Tests (`test/test_regex.c`)

Follow the file's existing shape: `static void test_x(void)` with `CHECK()`,
registered with `RUN()` in `main()`.

Table every byte offset of `"aåb"`, a CJK string, an emoji, a combining
sequence (`e` + U+0301), and malformed input (`"\xC3"`, `"\xA5x"`,
`"a\xE2\x82"`): `kg_utf8_forward_boundary()` must be monotone, idempotent and
never below its argument. For each subject with pattern `a*`, drive
`kg_regex_match_forward()` through `kg_regex_next_offset()` until `-1`; offsets
must strictly increase and successful positions must number exactly
`glyph_count + 1`. No match from `kg_regex_match_forward(rx, text, k, &m)` may
have `m.spans[0].start < kg_utf8_forward_boundary(text, len, k)` — the snap-back
regression. Extend `test_match_backward` so a multi-byte subject still returns
the same spans.

## Phase 2 — Fix query-replace-regexp progress and cancellation

Files: `src/search.c` (`editor_query_replace_regexp()`, :828), new
`test/pty/` zero-width cases, `test/test_regex.c` for the iteration invariant.

### Changes

Replace both `+ 1` sites (`src/search.c:948` and `:952`) with the phase 1
helper. Concretely, per iteration:

1. keep the pre-edit `match_start`/`match_end`;
2. compute `next_raw = kg_regex_next_offset(row->chars, row->size,
   &match_res.spans[0])` **before** mutating the row;
3. on acceptance, apply the replacement, then
   `match_col = next_raw < 0 ? -1 : next_raw + (expanded_len - match_len)`;
4. on refusal, `match_col = next_raw`;
5. when `match_col < 0` or `match_col > editor.row[filerow].size`, do
   `filerow++; match_col = 0;` and continue;
6. keep the existing region-end adjustment (`end_col += expanded_len -
   match_len` when `filerow == end_row`) — it is correct and must be applied
   once, from the same delta used in step 3.

The delta in step 3 is why an empty match with a non-empty replacement still
progresses: the next glyph moved right by exactly `expanded_len`.

Because `!` synthesizes `y` and never reads a key, add responsiveness to the
`replace_all` branch: every 256 replacements call
`editor_check_quit_pending()` (`src/tty.c:178`, declared `src/def.h:917`) and
break with `Query replace cancelled`. Keep a defensive work cap of
`2 * (region glyph count + content-changing replacements)`; exceeding it is an
internal progress error — report `Internal error: query-replace made no
progress` and break, never continue.

### Pitfalls

- `editor_check_quit_pending()` polls `STDIN_FILENO` directly, ignoring its
  `fd` argument, and buffers every non-`C-g` byte into `pending_input` for
  later replay. Correct here, but it consumes typeahead — do not call it per
  replacement.
- It returns `0` unconditionally under `KG_FUZZ`, so the work cap, not the
  poll, protects the fuzz targets.
- `editor_row_insert_char()` calls `editor_nomem()` and can set `running = 0`
  without returning a status. The loop must also stop when `!running`.
- `expand_replacement()` is called before the y/n prompt (`src/search.c:901`)
  so the prompt can show the expansion; every `break` path must
  `free(expanded)` (`:922` already does; `:955` is the normal path).

### PTY tests

There is **no per-case `timeout` key**. `utils/pty_accept.py` takes a global
`--timeout` (default 5.0 s; `Makefile PTY_TIMEOUT ?= 15.0`; `.ci/ci-env.sh`
raises it to 20–40 s). A regression that loops forever therefore fails as a
harness timeout, which is the intended signal — so keep these cases small.

Key tokens are literal unless named; use `RET` for Enter and quote `'!'` in
YAML. New cases (`backend: pexpect` is enough; no screen assertions needed):

1. `zero-width-replace-utf8-empty`: `initial: "åbc"`, pattern `a*`,
   replacement empty, answer `!`. `expected_saved: "åbc"`.
2. `zero-width-replace-utf8-insert`: same buffer, replacement `X`, answer `!`.
   Expect one insertion per glyph boundary including EOL. Verify the exact
   string on a scratch build before committing; if kg and Emacs disagree, use
   `oracle: emacs` and let the oracle define it (`TERM=xterm-256color` when
   running by hand).
3. `zero-width-replace-refuse`: replacement `X`, then four `n` answers and `q`;
   the file must be unchanged and the run must not time out.
4. `zero-width-replace-region`: mark a two-glyph region, run the same replace,
   assert text outside it is untouched. And `zero-width-replace-empty-file`:
   `initial: ""`, replacement `X`, answer `!`.

Use `key_delay: 0.2` / `startup_delay: 1.0` like the neighbouring
`query-replace-regexp-utf8*.yaml` cases; never below `key_delay: 0.05` (under
30 ms kg treats input as a paste).

Add one native test of the iteration rule alone: for `"åbc"` and `a*` the
`(start, next)` pairs must be `(0,2) (2,3) (3,4) (4,5) (5,-1)`.

## Phase 3 — Preserve runtime complexity status

Files: `src/regex.c`, `src/regex.h`, `src/search.c`, `test/test_regex.c`,
`test/regex_differential.c`.

### Changes

Map raw statuses explicitly in both wrappers:

- `RE_STATUS_OK` → validate/copy match, `KG_REGEX_OK`;
- `RE_STATUS_NO_MATCH` → `KG_REGEX_NOMATCH`;
- `RE_STATUS_TOO_COMPLEX` → `KG_REGEX_TOO_COMPLEX`;
- `RE_STATUS_BAD_PATTERN` / `RE_STATUS_BUFFER_TOO_SMALL` → `KG_REGEX_BADPAT`
  (they mean the compiled program handed in is unusable).

`kg_regex_match_backward()` currently `break`s out of its scan on any non-OK
status (`src/regex.c:151`), silently downgrading exhaustion to "the last
earlier match, or no match". Record exhaustion and return
`KG_REGEX_TOO_COMPLEX` even when an earlier match was found: the requested
"last match before N" was not established.

`do_isearch()` gains a third status string beside the existing `Regexp I-search
[bad regexp]` (`src/search.c:428`): `Regexp I-search [too complex]`. Point,
viewport and the saved highlight stay as they were (`RESTORE_HL` already runs
before the match branch), and `C-g` still cancels via the existing
`ESC/ENTER/CTRL_G` branch.

`editor_query_replace_regexp()`: on `KG_REGEX_TOO_COMPLEX`, stop before
touching text, report `Regexp too complex; stopped after %d replacement(s)`,
and leave already accepted replacements in place (each is its own undo record
today; do not silently change that here).

Delete `exec_ran_out()` from `test/regex_differential.c` (:38-46) and its call
site, replacing it with a `toocomplex` branch on `KG_REGEX_TOO_COMPLEX` from
`kg_regex_match_forward()`. That removal is the proof the fix landed.

### Pitfalls

- `re_exec()` reports `TOO_COMPLEX` only **after** the whole scan fails
  (`re.c:446`); `re_matchp_internal()` resets `re_match_steps` once per
  `re_exec()` call, not per start position. Do not add a per-position reset.
- A deterministic exhausting case is a nested quantified group over a run of
  the repeated character with a failing tail (e.g. `\(a*\)*b` against many
  `a`s). Confirm the exact length that trips `MAX_MATCH_STEPS` (2000000,
  `re.c:85`) on a scratch build and pin it with a comment; do not guess, and do
  not pick a length with visible wall time.
- Plan 06 may change the engine's budget accounting. Assert that
  `KG_REGEX_TOO_COMPLEX` reaches the caller, never a specific step count.

## Phase 4 — Search buffer text, not rendered text

Files: `src/search.c` — `isearch_find_match()` (:84), `do_isearch()` (:386) —
plus new `test/pty/` tab cases.

### Changes

Switch **both** search kinds in `isearch_find_match()` from `row->render` to
`row->chars` — the literal branch (`:133`, `:136`) has the same inconsistency as
the regexp branch (`:117`, `:120`). Clamp `col` against `row->size` instead of
`row->rsize` (`:104-110`).

Then fix the coordinate space at the call site:

- delete the `chars_to_render_col()` conversion of `start_col`
  (`src/search.c:407-410`) — the start is now a chars offset already;
- keep highlight painting in render space: the `memset(row->hl + match_col,
  HL_MATCH, match_len)` at `:579` must become
  `rcol = chars_to_render_col(row, match_col)` and length
  `chars_to_render_col(row, match_col + match_len) - rcol`, guarded by
  `rcol + rlen <= row->rsize` the way `query_replace_mark_match()` (:201)
  already guards it;
- pass a **chars** offset to `editor_reveal_position_centered()` (`:584`),
  which is what fixes the point-placement bug described above.

`row->hl` is indexed over render (`editor_update_row()`, `src/buffer.c:259`),
so the highlight conversion is mandatory, not cosmetic.

### Tests

Fixtures need a real tab on the first line: plant it via `initial:` in the YAML
(a literal tab inside a block scalar survives) and type one with `C-q` then
`TAB` when a search string needs it. kg has no `\t` regexp escape.

- `isearch-regexp-matches-literal-tab`: search a quoted literal tab; the match
  must land on it and a following `C-a` + `X` write to the right column.
- `isearch-regexp-tab-is-not-spaces`: eight spaces must not match a tab, and
  vice versa. `isearch-literal-tab-parity`: same fixture, literal isearch.
- `isearch-point-after-tab`: search a word after a leading tab, then type; the
  saved file proves point was in chars space. This is the regression for the
  coordinate bug and fails before the change.
- One case with a tab plus a wide CJK glyph, and one with `visual-line-mode`,
  to prove the render conversion still lands.

Prefer `expected_saved`; use `backend: tmux` with `expected_screen_contains`
only where the highlight position is itself the assertion.

## Phase 5 — Replace a match in one mutation

Plan 08 phase 5 defines the generic primitive
`int editor_row_replace_range(int row, int at, int delete_len, const char
*insert, int insert_len, const struct edit_options *options)`. If Plan 08 has
not landed, add the narrow row-local helper here and make it the first
implementation Plan 08 generalizes — do not invent a second name.

Files: `src/buffer.c`, `src/def.h`, `src/search.c` (both query-replace
loops), `test/test_buffer.c` (links the row helpers; see `EXTRA_buffer` in the
`Makefile`), `test/pty/query-replace*.yaml`.

### Required behavior

- validate `at`, `delete_len` and the final size with the existing
  `checked_add_int_size()` / `checked_mul_size_t()` helpers (`src/def.h:605-644`,
  used this way in `src/fileio.c:228`);
- copy the undo payload out **before** the overlapping `memmove`;
- reserve the new capacity before mutating anything, so an OOM leaves the row
  byte-identical;
- one `memmove` + one copy; one `editor_update_row()`; one `editor.dirty`
  increment per logical replacement, not per byte; one
  `undo_push(UNDO_REPLACE_TEXT, ...)`;
- no ambient `suppress_undo = 1; ...; suppress_undo = 0;` pair. Until Plan 10
  removes the global, save and restore it rather than forcing it to 0 — the
  current code at `src/search.c:706-714` and `:935-943` clobbers a nested
  suppression.

Migrate `editor_query_replace()` first, then `editor_query_replace_regexp()`,
in two commits.

### Tests

Native, in `test/test_buffer.c`: an allocation failure (reached through a size
that trips the checked arithmetic, since there is no malloc injection hook
today) leaves the row and undo stack unchanged; zero-length delete, zero-length
insert and both; `editor.dirty` advances by exactly 1 per logical replacement.

PTY: captures/backreferences (`\&`, `\1`) still expand — extend rather than
replace `query-replace-regexp.yaml`; one `C-_` restores the exact original text
after a multi-byte replacement (`query-replace-smart-case-undo.yaml` is the
model); a few-KiB single-line replacement completes; a replacement on a line
preceding an unterminated block comment still repaints downstream syntax.

## Phase 6 — Stateful and differential operation traces

Files: `test/fuzz_regex.c`, `test/regex_differential.c`,
`utils/regex_differential.py`, `utils/regex_oracle.el`, and the tracked seeds
under `test/fuzz-seeds/regex`.

### Changes

`test/fuzz_regex.c` today makes exactly two calls: one forward match at offset
0 and one backward match at `txt_len` (:59-60). Extend the input encoding —
documented in the file's header comment, which **must** be updated in the same
commit or hand-written seeds stop meaning what they say — to also drive forward
matches at every byte offset, repeated `kg_regex_next_offset()`-driven
iteration to exhaustion, backward matches at every offset, and replacement
expansion from a fuzzed template.

Assert inside the harness: spans ordered and within `[0, strlen(text)]`;
participating spans on glyph boundaries for well-formed input; no result
preceding `kg_utf8_forward_boundary()` of the request; empty iteration strictly
progressing and terminating in `<= glyphs + 1` steps; repeated execution
byte-identical; wrapper status equal to the mapping of `re_exec()`'s status.

Run `make fuzz-regex-seed` then `make fuzz-regex-seed-replay` after changing the
encoding; every tracked seed must still decode to something meaningful.

Differential (`utils/regex_differential.py` + `utils/regex_oracle.el`):

- compare compile acceptance exactly over the declared supported subset;
- compare **all** successive matches, not only the first — the change that
  would have caught the zero-width iteration bug;
- include ICASE, backward search, zero-width patterns, captures, and non-zero
  start offsets;
- keep the byte-offset conversion in the oracle (Emacs reports character
  offsets);
- keep an explicit allowlist with a written reason per intentional dialect
  difference, and commit every new divergence as a minimized seed.

Keep malformed-UTF-8 properties kg-only: Emacs' buffer encoding makes the
comparison undefined.

## Documentation

- `doc/kg.1`: the search section (around :700 and :1091-1128) gains the "too
  complex" status and a sentence saying regexp and literal search both match
  buffer text, so a tab is one character and never eight spaces.
- `doc/FUZZING.md`: the new fuzz input encoding and the iterator invariants.
- `README.md` only if user-visible status text changes; `src/help.c` only if a
  keybinding changes (none here); `fe/tiny-regex-c/README.md` only inside a
  submodule patch that changes its API.

## Commit sequence

1. Offset/progress helpers in `regex.c` + native tests (no behavior change to
   callers yet).
2. Zero-width PTY cases, then the `editor_query_replace_regexp()` fix.
3. Runtime status propagation, including the `exec_ran_out()` removal.
4. Buffer-text isearch and the point coordinate fix.
5. One-shot row replacement, literal then regexp.
6. Stateful fuzz and differential traces.

## Acceptance

```sh
make check
REGEX_DIFF_CASES=200000 make check-regex-differential
make fuzz-regex-seed-replay && make fuzz-regex-smoke
make WITH_LISP=0 clean all check
.ci/run-ci-steps.sh --parallel
```

Iterate with `python3 utils/pty_accept.py --kg src/kg --jobs 1 test/pty/<case>`.
Exit criteria: no query-replace path contains a byte-wise empty-match `+ 1`;
`test/regex_differential.c` no longer calls `re_exec()` directly;
`isearch_find_match()` no longer mentions `row->render`.

## Budget warning

`make complexity-check` caps total scc complexity at 4208 (`Makefile:143`); the
tree measures 4171 today — 37 points of headroom for **all thirteen plans**.
Per-function pmccabe is capped at 120 (`Makefile:152`) and the worst function
here is `do_isearch` at 52, so the total is the binding constraint. Phases 1, 3
and 5 should therefore remove branches from `src/search.c` (262 today, per-file
cap 520) as they add helpers. Do not raise either ratchet to land this plan; if
the total genuinely must rise, that is a separate reviewed commit with a
written justification.
