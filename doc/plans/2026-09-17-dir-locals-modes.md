# Plan — mode-scoped `.dir-locals.el` (`java-mode`, `c-mode`, `c++-mode`)

Written 2026-09-17 from a measured investigation against HEAD `d41bfad`.
Sequencing: standalone — shares no caps with the elisp program but must
re-measure `complexity-check` / `pmccabe-check` at slice start. Every
figure below is a 2026-09-17 measurement; the implementing slice
re-measures and trusts nothing carried forward.

## What is wrong today

Two independent gaps, plus one traversal that already works:

1. **Traversal works.** `dirlocals_find()` (`src/localvars.c:422`) starts
   at the visited file's directory (via `realpath`) and walks parents to
   `/`, checking `<dir>/.dir-locals.el` at each level. A file 3–4
   directories below the `.dir-locals.el` is already found. Nothing to
   build here except depth tests as proof.
2. **Selector gap.** `dirlocals_parse()` (`src/localvars.c:901-1039`)
   only applies the `nil` selector (`:988`). Any other selector —
   `java-mode`, `c-mode`, `c++-mode`, and the `-ts-` aliases
   (`java-ts-mode`, `c-ts-mode`, `c++-ts-mode`) — is consumed by
   `dlr_skip_sexp()` (`:1019-1023`) and dropped. Pinned by
   `test_dl_cmode_skipped` (`test/test_localvars.c:646`).
3. **Variable gap.** `localvars_kind()` (`src/localvars.c:50-68`) knows
   exactly two names: `compile-command` (string) and `buffer-read-only`
   (bool). `c-basic-offset`, `tab-width`, `indent-tabs-mode` and
   `*-indent-offset` all fall into `LOCAL_VAR_NONE` → `ignored_entries`.
   The envelope-consistency matrix
   (`test_same_value_through_every_envelope`, `test/test_localvars.c:1053`)
   pins this as intentional-per-name, so a new variable is one table row
   plus one applier plus envelope parity.

Net effect on the reported file: all six blocks are skipped at the
selector before any variable is examined. Even rewritten under `nil`,
only `compile-command` / `buffer-read-only` would apply.

## What "right" means in kg (scope decision)

kg has no C/Java indenter. `editor_insert_newline()`
(`src/buffer.c:1338`) copies the current line's leading whitespace
verbatim; `TAB` self-inserts a literal `\t` (`src/kbd.c:362-364`). So of
the three variables in the reported file, only one has a consumer today:

| Variable | kg consumer today | Proposal |
|---|---|---|
| `tab-width` (int 1..1000) | Yes — `b->display.tab_width` via `editor_set_tab_width()` (`src/buffer.c:427`), renders tabs; also a Lisp buffer-local (`lisp/prelude.el:1002`, `setq` / `setq-local`) | **Functional**: dir-locals sets the per-buffer display width |
| `indent-tabs-mode` (`t` / `nil`) | None | **Parsed + stored + Lisp-visible, no editing consumer yet.** Documented as accepted-but-inert. Retargeting `TAB` / newline-indent to honour it is a behaviour change to every edit and belongs in a follow-up indent plan. |
| `c-basic-offset` / `java-ts-indent-offset` / `c-ts-mode-indent-offset` (int) | None | **Same as above**: parsed, stored, exposed; no indenter consumes them. Do not invent fake semantics (e.g. silently aliasing to `tab-width`). |

In scope: `java-mode`, `c-mode`, `c++-mode` **plus** their `-ts-`
aliases mapping onto kg's single `C` row (`src/syntax.c:31,209`) and
single `Java` row (`:53,215`): `java-ts-mode` → Java,
`c-ts-mode` → C, `c++-ts-mode` → C. Out of scope: every other mode
selector, `eval`, subdir-qualified forms `(("src/" . …))`,
`dir-locals-2.el` — parsed-and-skipped as today, with tests pinning the
skip.

## Investigation to re-run first

1. **Emacs oracle measurements** (pinned `emacs`; `make check
   KG_PTY_EMACS=…` / `--emacs`):
   - A `.java` file gets `java-mode` + `nil`, not `c-mode`. A
     `.c`/`.h`/`.cpp` file gets `c-mode` / `c++-mode` + `nil`.
   - `nil` vs mode precedence within one file (expected: mode-specific
     wins for the same variable — verify, don't assume).
   - Two `.dir-locals.el` files (root + 3 deep): which wins per variable
     (expected: nearer file wins; verify merge-vs-replace per variable).
   - Value shapes: `(tab-width . 2)` int, `(indent-tabs-mode . nil)`
     symbol, case-insensitivity of `t` / `nil`, out-of-range int,
     string `"2"` (malformed, not coerced).
   - `-ts-` aliases: which buffer each applies in (informs kg's mapping,
     does not dictate it).
2. **Re-measure caps**: `make complexity-check`, `make pmccabe-check`,
   `scc` per-file for `src/localvars.c`, `src/bufmgr.c`;
   `DL_MAX_TOKENS` / `DL_MAX_NESTING` headroom for the new branches. The
   kill-ring precedent cost +5..24 scc; size any raise at implementation
   time as a recorded Decision.

## The work

### Phase 1 — mode-aware parse (no new variables yet)

Thread the buffer's mode through the parse: new
`dirlocals_parse_for_mode(source, len, out, mode_key)` with
`dirlocals_parse()` kept as the `nil`-only wrapper so the fuzz entry
point and existing tests don't churn. Resolve `mode_key` once per visit
from `bcur()->syntax` (after `editor_select_syntax_highlight()`,
`src/bufmgr.c:1913`).

Mode table in one place, next to `localvars_kind()`:
`java-mode,java-ts-mode` → `KG_MODE_JAVA`;
`c-mode,c-ts-mode` → `KG_MODE_C`;
`c++-mode,c++-ts-mode` → `KG_MODE_C`. Unknown selectors skip the whole
entry (current behaviour, now with one test per alias).

Within one file apply `nil` pairs first, then the matching mode's pairs
over them (last-wins per variable, mirroring
`test_dl_duplicate_last_wins_dl`). Non-matching modes are skipped
untouched. Keep every safety property: non-evaluating reader,
`DL_MAX_FILESIZE` (65536), `DL_MAX_NESTING=64`, `DL_MAX_TOKENS=4096`,
`eval` → ignored, malformed → `malformed_entries` without aborting the
file.

### Phase 2 — three variables, one functional

Extend `struct local_settings` (`src/localvars.h:20`): `tab_width` +
`tab_width_set` (1..`KG_TAB_WIDTH_MAX`), `indent_tabs_mode` tristate
(`LOCAL_BOOL_*` reuse), `c_basic_offset` + `c_basic_offset_set`
(bounded, e.g. 1..32 — measure, then pin).

`localvars_kind()`: add `LOCAL_VAR_INT` / `LOCAL_VAR_BOOL` rows for the
three names. Reuse `init_config_parse()`'s integer validation
(`is_integer_token`, range check in `src/localvars.c:1052-1110`) for
`tab-width` / `c-basic-offset` so init.el and dir-locals cannot disagree;
`indent-tabs-mode` reuses `localvars_apply_bool()`.

Envelope parity: modeline/footer route through the same appliers (extend
the `via_*` matrix, `test/test_localvars.c:988-1084`). This is what makes
"one row, not three edits" true.

Buffer state: `tab-width` applies via the existing
`editor_set_tab_width()`; `indent-tabs-mode` / `c-basic-offset` get
per-buffer fields (next to `display` on `struct editor_buffer`,
`src/def.h:363`) defaulting to unset, plus Lisp `defvar`s so
`(setq-local …)` / describe can see them. No editor consumer reads the
latter two yet — that is the documented line.

### Phase 3 — apply at visit, with correct precedence

`buf_apply_local_settings()` (`src/bufmgr.c:1835`) already merges
`dir → modeline → footer` with file-local winning. Keep that order and
pass the resolved mode into the dir-locals parse. Deeper
`.dir-locals.el` wins by construction (only the nearest file is read —
`dirlocals_find` returns the first hit walking up).

Lisp interaction: dir-locals run at visit; a later
`(setq-local tab-width N)` overrides them (same shape as
`compile-command`'s `compile_command_user_override`). Revisit/revert
re-applies. `WITH_LISP=0` behaves identically for `tab-width` (display
is not Lisp-gated; precedent: the `fill-column` fallback in
`src/word.c`).

### Phase 4 — tests, docs, ratchets

- **Native** (`test/test_localvars.c`): mode block accepted /
  non-matching mode skipped / each `-ts-` alias / `nil`+mode precedence
  / int bounds + string-rejected / `t`/`nil` case-insensitive / unknown
  var still ignored / malformed file still `-1` with prior state
  untouched.
- **Traversal proof**: extend the `rmtree_dl` suite (`:833`) with a
  4-deep tree asserting `dirlocals_find` returns the nearest file, plus
  one root-only case found from depth 4.
- **PTY** (`test/pty/`, `workspace_files:`): `dir-locals-tab-width-java`
  — root `.dir-locals.el` carrying the reported `java-mode` block, open
  `a/b/c/d/Foo.java` containing a TAB, assert width-2 rendering vs the
  default 8; `dir-locals-mode-mismatch` — same tree under a `c-mode`-only
  file keeps width 8; `dir-locals-c-mode` — a `.c` file gets its block.
  PTY asserts *display*, never parser internals.
- **Fuzz**: add mode/alias seeds to `test/fuzz-seeds/dirlocals/`;
  `make fuzz-dirlocals-smoke` green.
- **Docs**: `doc/kg.1` dir-locals section (`:4713-4745`) gains the mode
  table + the three variables + the inert-but-stored note for offset /
  `indent-tabs-mode`; `README.md` local-variables lines (`:234-236,253`)
  updated; `make docs-check` green.
- **Gates**: `make check`, `make complexity-check`,
  `make pmccabe-check`, `make coverage` (`coverage-baseline` only to
  bank improvement), `format-check`; final
  `JOBS=8 .ci/run-ci-steps.sh --parallel`.

## Does not do

- No `eval`, no subdirectory selectors, no `dir-locals-2.el`, no new
  indenter behaviour, no `c-basic-offset`-driven reindent, no
  `indent-tabs-mode`-driven `TAB` / newline changes — stored state only,
  consumed solely by `tab-width` display.
- No change to `dirlocals_find()` semantics (depth already works); no
  signature change to the fuzz entry point beyond the additive wrapper.

## Price

At writing `src/localvars.c` is small and far from the 520 scc file cap;
the new branches are bounded integer/symbol appliers plus one selector
dispatch — expect low-double-digit scc, pmccabe-neutral (each helper ≤ 10
against the standing caps). Re-measure at slice start; any ceiling move
carries its bisect + before/after proof in the commit message per repo
rule, rationale in the message, never beside the knob.
