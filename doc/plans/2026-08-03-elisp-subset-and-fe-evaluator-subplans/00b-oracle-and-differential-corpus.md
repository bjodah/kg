# 00B — The Emacs oracle and the differential corpus

Parent: [Phase 0](../2026-08-03-elisp-subset-and-fe-evaluator.md#4-phase-0--freeze-the-contract-and-establish-baselines),
Fe work.

**Prerequisites:** [00A](00a-budget-and-fe-structure.md) for its budget row.

## Why this exists

Every later phase's gate is "matches the pinned Emacs oracle".  Without a
mechanism, that phrase means "somebody ran Emacs once and wrote the answer
in a test comment" — which is exactly what kg has today.  `test/test_lisp.c`
and the 64 Lisp PTY cases contain many individually measured Emacs
expectations and no way to re-derive any of them.

This sub-plan builds the mechanism.  It changes no language behaviour, and
it must be able to say, of a construct that is not yet implemented, "this
differs and that is intended" as distinctly as it says "this differs and
that is a bug".

## The oracle is already resolvable — do not add a second way

kg's PTY harness already resolves Emacs as: `--emacs`, else
`$KG_PTY_EMACS`, else `emacs` on `PATH`, else the `/opt-3` developer-box
pin; it SKIPs with a printed reason when absent, and `--require-tools`
turns that into an upfront failure naming the tool.  `make
check-regex-differential` uses the same convention and skips itself with a
message when `emacs` is missing.  Reuse both behaviours exactly.

The box has GNU Emacs **31.0.90**, a development build from the `emacs-31`
branch dated 2026-07-09, at `/opt-3/emacs-31-lucid/bin/emacs`.

**"Emacs 31" is not a pin.**  A branch build's behaviour can move.  Every
snapshot must record the exact version string of the binary that produced
it, and regeneration against a different version must fail loudly rather
than rewrite the file.  An oracle that silently re-records is an echo.

## Layout

In `fe/`:

```text
compat/
  README.md
  features.json          # see 00C; this sub-plan defines the schema
  cases/*.json
  oracle/*.json          # checked-in snapshots, version-stamped
tools/
  run-emacs-oracle.py
  run-fe-compat.py
```

**JSON, not TOML.**  Both trees are JSON throughout —
`.ci/coverage-baseline.json`, `.ci/mutation-gateway.json`,
`.ci/pmccabe-baseline.json`, `test/.results/{unit,pty,bench}.json`,
`.ci/.run/quality.json` — and every checker is a Python script under
`utils/`.  A second format costs a parser in each consumer and buys
nothing.  `tomllib` exists in this box's Python 3.13, so the argument for
TOML would have to be ergonomic, and hand-writing case files is not the
common path: they are generated and diffed.

## The record protocol

The parent plan is right that this must not go through the human-readable
printer, and the reason is worth stating: Fe's writer is bounded and emits
`#<cycle>`, `#<deep>` and `#<truncated>` — stable output by design, and
useless as a comparison key.  One JSON record per case, from each side:

```json
{"kind": "value", "type": "integer", "printed": "3"}
{"kind": "condition", "condition": "wrong-type-argument", "data": "..."}
{"kind": "quit"}
{"kind": "unsupported", "feature": "hash-tables"}
```

`kind` is the classifier and is never derived by parsing a message string.
Note the ordering constraint the parent plan's §6 records: until Phase 6
there is no condition system, so `"condition"` on the **Fe** side is
carried in the existing `FeHandleError()` message, and the comparator must
treat that as a weaker claim than the oracle's structured signal.  Encode
that weakness in the schema now — a `"condition_source": "message"` field
— rather than discovering in Phase 6 that every stored record needs
rewriting.

Each case records: setup forms, the expression, the expected result
representation, observable side effects, the condition symbol and optional
data, and whether execution should terminate normally.

## The Emacs runner

- `emacs -Q --batch`, lexical binding **on** (`-l` a shim that sets
  `lexical-binding`, or a `;;; -*- lexical-binding: t -*-` header per
  case — pick one and say which).
- Catches ordinary errors via `condition-case`, and distinguishes `quit`
  from them.  In batch Emacs `quit` is reachable and is not an `error`;
  the corpus must prove the runner tells them apart before any Phase 6
  case relies on it.
- Emits canonical records on stdout, one per line, nothing else.  Anything
  Emacs prints incidentally goes to stderr and is not parsed.
- Stamps the version it ran under into the snapshot.

## The Fe runner

Drives `fe` — the standalone binary, not kg — over the same cases and
emits the same records.  Fe's script suite already has this shape
(`scripts/*.fe` against `tests/*.out` and `*.err`, run three times, the
third under `fe -a`); this is the structured sibling of it, not a
replacement, and `make -C fe check` keeps running both.

## The split the parent plan under-specified

Roughly half of the taxonomy in Phase 0's kg section — buffers, markers,
keymaps, interactive commands, editor primitives — **cannot run under
`emacs -Q --batch` against kg's semantics at all**, because the primitives
are kg's, not Emacs'.  One manifest cannot serve both.

- Fe owns `compat/features.json`: pure-language constructs, oracle-
  comparable, snapshot-backed.
- kg owns `test/lisp-compat/features.json` (00C): its 78 natives and its
  prelude forms, referencing Fe feature ids by name, with **no** oracle
  result claimed.
- A check asserts the id spaces do not collide and that no kg entry claims
  an oracle answer.

Putting kg's half inside a branch-pinned submodule would make every kg
inventory edit a pin move, which rule 10 makes a two-commit dance.

## Wiring

- `make -C fe compat` runs the corpus against Fe and the checked-in
  snapshots.  No Emacs required — that is what the snapshots are for.
- `make -C fe compat-oracle` regenerates the snapshots against the
  resolved Emacs, and fails when the version differs from what is
  recorded.
- A new numbered step in `fe/.ci/` runs `compat`; the runner discovers
  steps by glob, so it joins with no runner change.  Do **not** add it to
  kg's `.ci` — kg reaches it through `ci-12-subprojects.sh`, which already
  runs the submodules' fast suites.

## Gates

- A case can be written, run against both sides, and disagree in a way
  that names the feature id.
- Snapshots regenerate reproducibly and carry the oracle version; a
  mismatched version fails.
- With Emacs absent, `make -C fe check` still passes; with
  `--require-tools`, the oracle target fails naming the tool.
- The record protocol survives an unimplemented construct, a raised error,
  a quit, and a non-terminating case, and none of those is classified by
  reading a message string.
- Both complexity gates green in both trees, at 00A's numbers.

## What this does not do

- It does not populate the corpus.  That is 00C, and the two are separated
  precisely so the schema is not designed around whichever twenty cases
  got written first.
- It does not test kg.  kg's half is 00C's manifest plus the PTY harness
  that already exists.
- It does not compare backward regex matching, printing of cyclic
  structures, or anything else where kg's policy is deliberately not
  Emacs'.  `CLAUDE.md`'s note on `kg_regex_match_backward()` is the
  precedent: where kg encodes its own policy, an oracle has to encode that
  policy too, which makes it a test, not an oracle.

## Status

Not started.
