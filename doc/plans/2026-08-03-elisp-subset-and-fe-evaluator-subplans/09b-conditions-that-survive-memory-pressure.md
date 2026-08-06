# Sub-plan 09B — Conditions that survive memory pressure (fe-only)

Prerequisite: 09A. fe-only; no kg edits, no pin move.

## The defect, precisely

When the arena cannot allocate, `FeHandleError` (`fe_eval.c:631-641`) and
`RaiseCondition` (`fe_eval.c:762-765`) set `ctx->condition = &nil` instead
of building the `(NAME DATA…)` structure, and `ConditionMatches`
(`fe_eval.c:187`) answers `false` for any non-pair condition once the `t`
and `quit` kind checks have passed. Consequences, all measured in 09A's
Table X:

- `(condition-case e FORM (error …))` does not catch `out of memory`; only
  `(t …)` does. Same for `GC stack overflow`, and for *any named condition*
  that happens to be raised while the arena is full.
- A `(t …)` handler that itself allocates while the exhausting data is
  still rooted re-raises and escapes — currently unspecified behaviour.
- An escaping raise prints the entire call trace to stderr unbounded
  (thousands of forms on one line for an `(apply 'list BIG)` OOM).

## Mechanics

1. **Pre-built exhaustion conditions.** At `FeOpen` time, intern and
   permanently root two condition objects — `(out-of-memory)` and
   `(gc-stack-overflow)`-shaped pairs with their symbol names — so raising
   them allocates nothing. (Shape and names: measure what the existing
   structured-error renderer expects; the point is the *pair* exists, so
   `ConditionMatches` can walk it. Emacs' nearest names are
   `memory-full`/`error`; fe's condition names are fe's own here — there is
   no Emacs oracle for arena behaviour — but they must sit under `error` in
   the hierarchy so `(error …)` handlers catch them.)
2. **`RaiseCondition` under pressure**: when a *named* condition cannot be
   built because the arena is full, fall back to the pre-built
   out-of-memory condition rather than nil — the handler sees a truthful
   "this became an OOM" rather than nothing.
3. **Handler re-entry rule**: define and test what happens when the handler
   itself hits OOM — the second raise unwinds to the *next* enclosing
   handler (ordinary nesting), and to the host when none remains. No new
   mechanism; just pin it.
4. **Bounded escape trace** (09A Decision 5): the host-facing trace print
   truncates per the printer's existing depth/node budget instead of
   dumping every form.
5. The GC-stack-overflow `abort()` past the reserve
   (`fe_eval.c:686-694`) stays — it is the can't-even-unwind backstop; the
   reserve exists so the *raise* path never needs it. Assert the reserve is
   still sufficient with the new pre-built conditions (they are pre-rooted,
   so the raise path allocates strictly less than today).

## Tests owned by this slice

- `test_api.c`: `(condition-case e BIG (error 'caught))` catches; the
  specific name catches; `(t …)` still catches; session-consistency after
  a caught OOM (evaluate more forms, collect, check `FeGetArenaStats`);
  handler-re-OOM unwinds to the next handler and then the host; named
  condition raised while full arrives as the OOM fallback, not nil;
  Budget kinds (step/frame/re-entry) remain uncatchable (09A Decision 2);
  quit still catchable with a nil condition (kind check precedes shape).
- The 09A "before" pins flip to the new contract in the same commit that
  changes behaviour — no window where neither is asserted.
- Script suite: an exhaustion script under `scripts/` with checked-in
  golden, runnable by `fe` directly with a small `-s`.
- Fuzz: a tracked seed exercising condition-case-around-OOM
  (09A Decision 6); reachability counts in the commit body.
- Compat: none — Emacs has no arena; record that explicitly in
  `compat/features.json` only if a feature row is touched at all.

## Gates

- Full nine-stage fe runner green.
- Opening commit raises `SCC_COMPLEXITY_MAX` 760→820 and
  `PMCCABE_TOTAL_MAX` 1056→1120 with temporary-lowering proof in the body
  (09A Decision 7); the close slice re-sets to actuals.

## Price

fe +15..30 scc (two pre-built conditions, one fallback arm, one truncation
policy, no new subsystem). Against Phase 6's measured condition work, the
hierarchy already exists; this is plumbing, not architecture.

## Explicitly not this slice

No mark-phase work (09C). No kg edits, no pin (09D). No cleanup-registry
work (09A Decision 4). No new condition *hierarchy* — the names land under
`error` in the existing table.
