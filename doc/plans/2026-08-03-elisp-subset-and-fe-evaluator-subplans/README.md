# Sub-plans — Emacs Lisp subset and Fe evaluator evolution

Parent plan:
[../2026-08-03-elisp-subset-and-fe-evaluator.md](../2026-08-03-elisp-subset-and-fe-evaluator.md),
reviewed and corrected 2026-08-04.  Read the parent's §0 (verified
baseline), §0.1 (the two complexity ratchets) and §0.3 (scope honesty)
before any of these; they are the facts these documents assume.

This first set covers **Phase 0** and the extraction half of **Phase 1** —
everything that has to exist before a single line of Fe's evaluator,
symbol table or numeric tower changes.  Nothing here changes language
behaviour.  That is the point: Phase 0 exists to make the difference
between "compatibility work", "a regression" and "a documented divergence"
mechanically decidable, and Phase 1's extraction exists so that the
migration in Phase 2 is a Lisp diff rather than a diff of escaped C string
literals.

## Grouping

| Sub-plan | Phase | Focus | Prerequisites |
|----------|-------|-------|---------------|
| [00A](00a-budget-and-fe-structure.md) | 0 | Price every phase; decide both complexity caps and whether `fe.c` splits into more than one translation unit | none — **this is first** |
| [00B](00b-oracle-and-differential-corpus.md) | 0 | Fe's `compat/` corpus, the `emacs -Q --batch` runner, versioned snapshots | 00A (for its own budget row) |
| [00C](00c-feature-inventory.md) | 0 | The manifest: 31 Fe primitives, 78 kg natives, 54 prelude definitions, each with a status | 00B (needs the record format) |
| [00D](00d-baselines-and-arena-observability.md) | 0 | Baselines through the counters that already exist, plus the minimal Fe arena statistics needed to take them at all | 00A; may land beside 00B |
| [01A](01a-prelude-extraction.md) | 1 | `lisp/prelude.fe` becomes the canonical source; generator, drift check, stale-comment cleanup | none technically, but land after 00C so the inventory is written against the file |

**00A is genuinely first.**  Fe measures 210 against a cap of 210 and
`fe.c` measures 102 against a file cap of 105.  Every other sub-plan here
adds code to one tree or the other, and `.ci/ci-12-subprojects.sh` runs
fe's complexity gate from kg's tree, so the first commit that adds a
function to `fe.c` turns kg's CI red.  This is not a theoretical
constraint; it is the state of the tree today.

00B and 00C are the substance of Phase 0.  00D is smaller than it looks
because kg's performance apparatus already exists and only needs Lisp
cases — except for its one real deliverable, the Fe-side arena counters,
without which four of the eight baseline items cannot be measured at all.

01A is included in this first set because it is genuinely independent of
the language work, it is the last cheap moment to do it, and every later
phase edits the prelude.  Doing it after Phase 2 would mean migrating 54
definitions inside C string literals.

## Compatibility direction

The parent's §0.4 is binding for every sub-plan: there are no known external
users of this Fe fork and no known user-written kg `init.fe` files.  Here,
"compatibility" means agreement with the pinned Emacs oracle, not preservation
of the old Fe/kg dialect.

Price and implement only the hard cutovers the two repositories need.  Do not
add legacy aliases, C-API wrappers, dual-evaluator modes, source-file lint for
hypothetical configs, or `.fe` filename fallbacks.  Version numbers still move
because they make the Fe↔kg contract checkable.

## Sequencing

```text
00A  budget + TU decision ──┬──> 00B oracle/corpus ──> 00C inventory ──> 01A prelude extraction
                            └──> 00D baselines + arena counters
```

00A gates everything because it produces the numbers.  00C depends on 00B
only for the record format — the inventory can be *drafted* in parallel.
01A is placed last in this set so the inventory it feeds is written against
`lisp/prelude.fe` rather than against a C array that is about to be
deleted, but if 00C slips, 01A can go first with no loss.

## What this set deliberately does not do

- **No language behaviour changes.**  Not `setq`, not the `=` migration,
  not a single new primitive.  Phase 0's whole value is that it is
  measured against an unchanged language.
- **No evaluator work.**  The frame machine is Phase 3 and is the largest
  single risk in the program; it does not start before its budget row
  exists and the translation-unit question is answered.
- **No new test harness.**  kg has a PTY harness, a native harness, a perf
  counter build, a bench driver, five fuzz targets, twelve CI stages and
  four ratchets.  Fe has eight CI steps and three fuzz targets.  Every
  sub-plan below extends one of those.
- **No `.el` filenames yet.**  Phase 2 gives `=` its numeric meaning and makes
  the dialect/filename cut at the same time.  There `.el` replaces `.fe`; it
  is not a preference with a legacy fallback.  See the parent's §5 and §6.

## Rules

The follow-up program's rules (`../2026-07-31-follow-ups/README.md`) apply
unchanged, and three of them do most of the work here:

- **Rule 6** — no ratchet is raised for routine work; record scc before and
  after; bank a decrease when one lands.  00A is where the non-routine
  exception is written down.
- **Rule 9** — each commit runs its focused suite; each phase ends with
  `make check` and `make WITH_LISP=0 clean all check`; a completed
  workstream ends with `.ci/run-ci-steps.sh --parallel`.
- **Rule 10** — Fe changes land and pass in the submodule first, then the
  parent pin and all kg adaptations move together in a separate, green kg
  commit.  A deliberate API/language break must not create a pin-only commit
  that cannot build.

Two additions specific to this program:

- **Run both complexity commands, in both trees.**  `make
  complexity-check` and `make pmccabe-check` measure different things;
  sub-plan 07B was caught by the second after passing the first.  Fe has
  its own pair, and a cap can sit silently red for a whole plan.  Run them
  at the *start* of a slice as well as the end.
- **Every phase touches `doc/fe-upstream.md`'s divergence table.**  That
  table currently documents, as deliberate decisions, most of what this
  program reverses.  A phase that changes a row and does not rewrite it
  leaves the authoritative document lying.

## Status

Not started (2026-08-04).  This directory was created with the parent
plan's review.
