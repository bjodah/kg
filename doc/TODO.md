# TODO In Priority Order

The dependency-ordered implementation program for the next architecture and
feature work is [doc/plans/2026-07-31-follow-ups](plans/2026-07-31-follow-ups/README.md).
This file remains the broader feature and technical-debt inventory.

## Convenience

- [x] mouse-mode by default if support detected. Left click to move
      cursor (and select the window under it), scroll wheel up/down (5
      lines), left drag marks a region.  `src/mouse.c`, SGR 1006
      reporting, on unless `TERM` is unset/dumb/unknown; `M-x
      xterm-mouse-mode` toggles (terminal-native selection needs
      Shift-drag while reporting is on, see kg(1) MOUSE).  Out of v1:
      middle/right buttons, double-click selection, modifier bits,
      mouse in keyboard macros.
- [x] "C-x C-f C-a C-k /tmp/newfile.txt RET foobar C-x C-s" no longer asks
      "Cannot verify /tmp/newfile.txt on disk.  Save anyway? (y/n)": a visit
      that ends in ENOENT records the file as absent instead of leaving its
      state unsampled, so the first save of a new file is silent as in Emacs.
      The question stays for a file that was there and is not, or that
      cannot be examined.
- [x] Create new scratch buffers: done as the full Emacs rule rather
      than asterisk-gated -- "C-x b name RET" for any non-matching name
      creates a buffer visiting no file (`no_file` +
      `buf_visits_file()`), and "C-x k" confirms unsaved changes only
      for file-visiting buffers.  "C-x C-s" in such a buffer prompts
      "Write file:" and adopts the answer.  Known consequence, accepted
      for now: killing a modified lone non-file buffer exits silently
      (kill-last-buffer-quits composing with the new kill rule).
- [ ] Currently the minibuffer behavior is patched in an ad-hoc manner:
      I would expect all (almost all?) manipulation that work in the
      ordinary buffer to work in the minibuffer too.  The quick wins
      landed 2026-08-10: isearch accepts "C-q" (isearch-quote-char, so
      "C-q C-j" searches for word-then-newline, with the cross-row
      literal matching that requires), and the prompt editor gained
      M-u/M-l/M-c beside its existing M-f/M-b/M-d/M-DEL/C-q.  Still
      open, deliberately deferred (minibuffer-as-real-buffer work):
      prompt word boundary is whitespace rather than word syntax,
      regexp isearch and query-replace are still per-row, the prompt
      is a fixed char[] (no undo, no transpose, no embedded newline).
- [x] "M-/" to run dabbrev-expand (equivalent).  `src/dabbrev.c`, with
      Emacs' candidate order, cycling, exhaustion wording and
      restore-the-abbreviation behaviour pinned by `oracle: emacs` cases.
      Left undone on purpose: case-fold matching and case-pattern fitting,
      the major mode's syntax table as the word definition (kg uses its own
      ASCII one), and continuing into other buffers.
- [ ] "M-/" should have an atomic undo operation.
- [ ] M-x has no exact-match-wins rule: RET runs the first prefix match
      in table order, and static commands sort ahead of Lisp ones, so
      the exact name of a user command can be shadowed by a longer
      built-in (found when `show-paren-mode` shadowed a test's `show-p`;
      Emacs' completing-read accepts the exact match instead).

## Maintainability

- [x] **CI rebalance** (2026-08-10): `ci-03-gcc-analyzer-valgrind` split
      into `ci-03-gcc-analyzer` (fast, default tier) and `ci-15-valgrind`,
      the first member of the runner's *expensive* tier — reported as SKIP
      by default, run with `--expensive` or `CI_EXPENSIVE=1`, which
      `.woodpecker.yaml` sets so hosted CI runs everything.  The `.ci`
      scripts no longer `set -x`; run one under `bash -x` when a trace is
      wanted.  The failure-frequency ledger idea was considered and
      REJECTED: raw failure counts conflate gates that catch bugs with
      gates that flake, and cost-of-late-discovery is asymmetric (a
      coverage-ratchet failure at end of campaign is a five-minute fix; a
      sanitizer failure there is a bisect across every sub-plan).  Pick a
      subagent's step subset by cost instead: fast static gates per slice,
      PTY-heavy sanitizer lanes at end of campaign — except that a slice
      touching memory-lifetime code runs its sanitizer lane early.
- [x] **Over-splitting audit** (2026-08-10): a CCN<3 floor was considered
      and REJECTED — CCN 1 is also every accessor and named predicate, and
      a low-complexity ratchet would fight the scc/pmccabe ratchets from
      the other side.  The real smell is a single-caller `static` helper
      threading many of its caller's locals through the boundary, which is
      what `utils/single_caller_audit.py` ranks (manual tool, no gate, no
      Makefile target — by decision, not omission).
- [ ] A first inlining pass over that audit's top rows: the
      `splice_alloc`/`splice_head_alloc`/`splice_rows_alloc` chain in
      `src/buffer.c` (10/8/6 parameters, one caller each) and the
      `path_*`/`buf_name_*` prompt families in `src/bufmgr.c` (8–12
      parameters each).
- [x] **Comment creep** (2026-08-10): the Makefile's ~1000-line ratchet
      changelog block is gone (2244 → 1258 lines) and the policy that
      replaces it is in AGENTS.md: a ratchet raise/lower/re-baseline
      carries its rationale and measured proof in the COMMIT MESSAGE; the
      comment beside a knob says what it means today, nothing about how it
      got there.  A comment-percentage CI guard was considered and
      REJECTED (gameable by adding code; punishes headers whose comments
      earn their keep).  `utils/comment_ratio.py` ranks files by comment
      share for one-off verbosity hunts instead.


## LSP follow-ups (v1 landed 2026-08-09)

The `WITH_LSP=1` client is complete per
[doc/plans/2026-08-08-lsp.md](plans/2026-08-08-lsp.md)'s definition of
done: `M-.`, `M-?` and `M-,` over a JSON-RPC stack of its own
(`lsp_transport`, `lsp_json`, `lsp_client`, `lsp_server`, `lsp_sync`,
`lsp_uri`, `xref`), tested against a scripted fake server and against the
real `clangd` and `ty`.  Known follow-ups, none blocking:

- ~~**Result previews in `*xref*`.**~~  Done: the listing ships as
  `path:line:col: preview`, the preview coming from `src/fileline.h`'s
  `kg_file_line_preview()` -- an open buffer's own row when one visits the
  file, so an unsaved edit is what is shown, and a bounded line-by-line
  read otherwise (no buffer is opened to paint a listing, and no more than
  a mebibyte is walked to reach one line).  Trimmed, capped at 200 bytes
  and stripped of control bytes, because a listing row is what RET indexes
  results by.  The seam is a module of its own for the occur- and
  grep-style listings that want the same text.
- [x] **A per-request timeout.**  Done 2026-08-10: every request carries a
      deadline (30 s, `KG_LSP_TIMEOUT_MS`, `0` disables), and one that
      passes abandons the request with an error naming the method and the
      wait, a line in `*lsp-log*`, and the server left running.  A late
      reply lands on no pending entry and is dropped.
- [x] **An `*lsp-log*` buffer.**  Done 2026-08-10: server stderr has a pipe
      of its own (`kg_process_spawn_bidi()`'s fifth argument), and its
      lines, the timeouts and the reason a client died are appended to
      `*lsp-log*` with the server's name in front.  Created lazily, never
      selected, last 64 KiB kept.
- **A fuzz target for the frame parser.**  `lsp_transport.c`'s
  Content-Length framing is the one place kg parses bytes from a process
  it did not write; the five existing targets are the pattern
  (`test/fuzz_*.c`, tracked seeds under `test/fuzz-seeds/`).
- **More server specs.**  `gopls` and `rust-analyzer` are one row each in
  `server_specs[]` plus a marker predicate (`go.mod`, `Cargo.toml`) and
  the `KG_LSP_SERVER_<MODE>` name that follows from the mode.
- **The rest of the protocol**, in the order it would be worth having:
  diagnostics (which need a decoration channel and an error list, not
  just a request), hover, rename, completion.  All were out of scope by
  the plan, not by accident.
- **Lisp bindings for the xref commands.**  They are deliberately not in
  the `CMD_LISP_CALLABLE` set while the command set settles.
- **The mode-map overlap cleanup.**  `*compilation*` is read-only, so the
  buffer-list map is live there too and its `RET` resolves to
  `ibuffer-visit-buffer`, which does nothing only because the command
  checks the buffer's syntax first (src/kbd.c, above `enum mode_map`).
  The by-name exclusion that used to keep `*xref*` out of that predicate
  is now one table, `name_keyed_maps[]`, which says which buffers own a
  map keyed on their name and which of those bind `RET` themselves --
  added when `*Occur*` became the third such buffer.  That is the shape
  of the answer, not the answer: a real mode registry owning the table,
  with the predicates and the buffer names on the modes rather than in
  kbd.c, is still what this wants.

## Tree-sitter follow-ups (v1 landed 2026-08-08)

The `WITH_TREE_SITTER=1` backend is complete per
[doc/plans/kg-tree-sitter-plan.md](plans/kg-tree-sitter-plan.md)'s
definition of done: 13 registry rows over 11 grammars, incremental
TSInputEdit parsing with damage-limited repainting, differential-tested
against full rebuilds.  Known follow-ups, none blocking:

- **Shell**: unblocked.  The grammar the loader used to refuse (a
  tree-sitter-bash of grammar ABI 6) is gone; the installed
  `libtree-sitter-bash.so` is ABI 15 and loads.  What is missing is
  one registry row + one query + tests.
- **Git-mode diagnostics under the TS backend**: the overlong-subject
  and bad-rebase-action HL_WARNING spans are legacy-scanner output, so
  a TS build shows git buffers plain.  The plan's Phase 9 answer is to
  re-express them as decorations, which both backends display.
- **TSX tag colouring**: TS and TSX share one query text restricted to
  the common node inventory, so `.tsx` tag names are unpainted; a
  second query literal for the tsx row fixes it if wanted.
- **Hosted CI promotion**: `.ci/ci-13-with-tree-sitter.sh` SKIPs off
  the developer box; promoting it means building the pinned core (and
  grammars) in a cached step (Refinement, "Hosted CI").
- **Bench cases**: slice 7 measured edit latency ad hoc (1.3-5.6 ms
  net per keystroke on 6.7k-38.8k-line files); `utils/bench.py` cases
  for large-file open + edit under the TS build would make that a
  tracked number.  Parse cancellation stays deferred until a
  measurement says otherwise (Refinement, "Latency policy").
- **Query embedding generator** (`utils/embed_tree_sitter_queries.py`):
  deliberately declined twice; revisit only if queries outgrow
  string-literal form.

## Lisp Interactive Follow-up

- [x] **07E interactive prompting**: `n`/`N`, `s`, `f`/`F`, and `b`/`B` use
      the existing pickers without side effects.
- [ ] **Deferred interactive codes and modifiers**: every valid Emacs code
      kg does not implement (`a b c C d D e G i k K m M R S U v x X z Z`)
      and the three modifiers `*`, `@` and `^` report *unsupported*
      interactive code, not invalid, and do not run the command body.
      Keyboard-macro strings and `CMD_REPEATS` for Lisp commands are
      deferred with them.
- [ ] **Interactive reflection**: `interactive-form` and documentation
      reflection need a metadata decision first — 07D stores the spec
      thunk but not the raw form beside it, so honest reflection has
      nothing to return. `commandp` therefore answers about a *name*
      (the command registry) rather than about a function object, which
      is a recorded divergence: Emacs also calls an anonymous lambda
      carrying an interactive form a command.
- [ ] **Prompt interpolation and `interactive` MODES**: Emacs passes a
      prompt containing `%` through `format` with the earlier interactive
      arguments, and filters commands by mode; kg's prompts are literal
      and the MODES arguments are accepted and ignored. Both are recorded
      divergences in `README.md` and `doc/lisp-api.md`.
- [ ] **Phase 8 read functions**: public `read-string`, `read-number`,
      `read-file-name`, and `read-buffer`, including optional arguments,
      defaults, history, and non-command re-entry semantics.
- [ ] **An arena reset command**, deferred by Phase 9's 09A Decision 3 and
      the one piece of exhaustion recovery kg does not have. Phase 9 made
      exhaustion catchable (`arena-exhaustion` under `error`), survivable
      (a `let`-local chain is collectable the moment the handler runs) and
      *visible* (`M-x lisp-arena-stats`), and stopped there deliberately.
      What it does not fix: an init file or a command that exhausts into a
      **global** leaves the arena pinned for the life of the session —
      alive, correctly reporting, and useless, with restarting kg as the
      only recovery. `test/pty/lisp-exhaustion-mid-init-visible.yaml` and
      `test/test_lisp.c:test_arena_exhaustion_conditions` pin that state
      rather than paper over it. A reset command is editor UI, not
      robustness: it has to decide what happens to Lisp-defined commands,
      hooks, key bindings, process filters and `provide`d features that
      live in the arena it would throw away, which is a design question
      Phase 9 had no reason to answer. When it lands it needs its own PTY
      case; it must not be bolted onto the two above.

- [x] **Dynamic binding, and `defvar` marking a symbol special.** Done by
      Phase 11 (11A Decisions 2–3), the subset it names: two symbol flags
      set by `defvar`/`defconst` through fe's `internal--mark-special`,
      shallow binding at fe's binding-list paths with restore on all five
      completion kinds, `special-variable-p`, and kg's prelude `let` moved
      off its lambda-application expansion onto fe's core bindings-list
      form so the flag can be consulted at all.  Parameters stay lexical
      unconditionally, which is Emacs 31's own measured answer.  The
      21-probe grid is `test/lisp-compat/cases/phase11-dynamic-*.json`.
- [x] **A one-argument `(defvar v)` is scoped to its file in Emacs and
      global in kg.**  Done by Phase 12 (12A Decision 6, fe `38caa63`),
      and the recorded blocker was wrong: the carrier this item said
      "neither fe's evaluator nor kg's loader has" is `EvaluateInput`,
      entered exactly once per `load`, `require`, batch file or prelude
      install.  It takes the next number of a monotone counter on the way
      in and hands the enclosing one back on the way out, so nested units
      stack; a let-dynamic-only mark carries the unit that made it, and
      the predicate compares for **equality** — measured in both nesting
      directions, so neither an outer unit's mark nor an inner one's
      leaks.  A full mark (two-argument `defvar`, `defconst`) stays
      global, as in Emacs.  Outside any input unit — kg's hooks, command
      dispatch and process callbacks — every mark is visible, since there
      is no unit there for one to be foreign to.
      `test_phase12_one_arg_defvar_file_scope` is the two-file probe, and
      it answers what the pinned Emacs answers for the same two files.
- [ ] **A `defun` written after a one-argument `defvar` in the same file
      stays dynamic in Emacs when it is called from another file.**  The
      one residual of the item above, and the direction its old text
      warned about: kg is now NARROWER there.  Emacs' mechanism is not a
      per-unit registry at all — a one-argument `(defvar v)` adds `v` to
      the lexical environment threaded through the file, and a `lambda`
      captures it — while fe consults the flag where the `let` RUNS and
      has nowhere to record a closure's unit.  Against the five-probe
      grid fe went 2/5 to 4/5 and lost this one.  Closing it means the
      marking becoming a lexical-environment entry, which is a different
      and larger design, in `fe_eval.c` rather than `fe.c`.  fe's
      `one-arg-defvar-scope-carrier` manifest row carries the grid; kg's
      `phase11-one-arg-defvar-file-scope` row carries what its own case
      actually pins, which is the oracle shim's per-form scoping and not
      the leak.
- [ ] **The prelude's `internal--let` temporaries are dynamically
      capturable.**  A consequence of the same phase, recorded rather than
      defended (11A Decision 3): the ~53 generically named temporaries
      `lisp/prelude.el` binds (`result`, `res`, `doc`, `name`, …) become
      dynamic the moment a user `defvar`s such a name.  This is exactly
      Emacs' own exposure for `lexical-binding` libraries, so "fixing" it
      means gensyms or an obarray-style renaming pass over the prelude,
      which is a cost with no measured demand behind it.  Left here so a
      future report of a strange interaction has somewhere to land.
      `internal--excursion` is a 54th name and the one with the sharpest
      consequence, added after the Phase 11 acceptance review found it:
      it is what `save-excursion`/`with-current-buffer` hold their saved
      state in, so a body that assigns it makes the `unwind-protect`
      cleanup raise — and a raising cleanup REPLACES the completion it is
      unwinding (fe's 06A Decision 4, which is Emacs' rule too:
      `(condition-case e (unwind-protect (error "MY") (error "CLEANUP"))
      (error e))` is `(error "CLEANUP")` on both), so the user's own
      error is lost.  Measured:
      `(condition-case e (save-excursion (setq internal--excursion nil)
      (error "MY-ERROR")) (error (format "%S" e)))` answers
      `"(error \"internal--excursion-restore: expected a marker or a
      buffer\")"`.  Pathological trigger, no gensyms to fix it with, and
      the tolerant restore cannot help: the value is not a spent
      excursion, it is a value of the wrong type.
- [ ] **Buffer-local variables.** `setq-local` and `setq-default` are
      aliases of `setq` and write the one global binding; both manifest
      rows are `divergent` for that reason since 10C.  A real
      implementation needs a per-buffer binding table, a lookup that
      consults it before the global cell, and a lifetime rule for buffer
      kill and switch.  `add-hook`'s LOCAL argument is unaffected — it is
      real already.
- [x] **The printer's `(quote X)` → `'X` abbreviation.**  Done by Phase 11
      (11A Decision 4) in fe's `WriteObject`, as the symmetric copy of the
      `(function f)` → `#'f` block that has been there since Phase 4, with
      Emacs' measured discrimination: exactly one element after the head
      and the form proper, so `(quote x y)`, `(quote)` and `(quote . x)`
      keep printing as pairs.  Recursive, so `(a 'b c)` comes out as Emacs
      prints it.
- [ ] **Backquote printing, and the reader symbols under it — BLOCKED,
      not merely expensive.**  Phase 12 measured it and put it out of
      scope with a basis (12A Decision 6), which is the shape the next
      reader phase should price against.  Emacs abbreviates over
      reader-produced symbols that ARE `` ` ``/`,`/`,@`; kg's reader
      expands them to the ordinary symbols
      `quasiquote`/`unquote`/`unquote-splicing`.  Renaming the reader's
      three expansion strings is a three-string edit — and then
      `lisp/prelude.el` **cannot spell the macro it defines**:
      `` ` `` and `,` are reader *delimiters* (`fe/fe.c`'s `ReadAtom`)
      and a backslash anywhere in a token is a named read error, so
      neither `` (defalias '\` …) `` nor any of `internal--qq`'s six
      pattern-match sites reads.  That rejection is not incidental — it
      is the **supported** `phase8-reader-rejection-policy` row, pinned
      by `test/test_lisp.c` and `fe/test_api.c`.  So symbol-escape reader
      support (or an `intern` primitive, which neither tree has) is a
      *prerequisite item*, not part of this one.  On top of that, Emacs'
      comma abbreviation is context-sensitive — measured, `(list '\, 'x)`
      standalone prints `(\, x)` and only inside a backquote does it
      print `,x` — which needs an inside-backquote depth field fe's
      `Writer` struct does not have.  Blast radius re-counted at the
      Phase 12 fix cycle (`git grep -lE "quasiquote|unquote"`, tracked
      files, `doc/plans` excluded): 75 sites in 22 files across the kg
      and fe trees, plus a guaranteed XPASS on
      `phase8-reader-nested-backquote` and a PTY `expected_saved` edit.
      The honest shape is two phases: reader escapes first, spelling
      second.  Recorded as `phase8-reader-backquote-symbol-names`.
- [ ] **Nesting is bounded by fe's evaluation frame limit, and Emacs has
      no such bound.**  Recorded by Phase 12, which removed the *other*
      bound: the adapter object pool went 64 -> 256 records, so nested
      `save-excursion` runs to 218 and the 219th raises `evaluation frame
      limit exceeded` — the same 1095-frame arena partition that bounds
      `with-current-buffer` (at the fix cycle, over `(current-buffer)`:
      156 runs, the 157th raises).  Emacs has no pool and
      no frame limit; its first ceiling is the C stack, at about 50000
      nested `save-excursion`s.  Raising this means a larger frame
      partition or a growable one, which is fe arena work and has no
      measured demand behind it: nothing kg ships nests past single
      digits.  Collector back-pressure is measured **useless** for it —
      at the threshold every record is live, so a forced full collection
      frees zero.
- [ ] **`permission-denied`, Emacs' third `file-error` leaf.**  Recorded
      by Phase 12 rather than asserted (12A Decision 1), because this box
      cannot measure it: `chmod 000` is defeated by running as root, and
      the suite runs as root.  Measured once, unprivileged, through
      `setpriv --reuid=65534`: Emacs 31.0.90 answers
      `(permission-denied "Cannot open load file" "Permission denied"
      PATH)`, conditions `(permission-denied file-error error)`.  kg
      answers the PARENT class, `file-error`, with the same data — a
      handler spelling `file-error` catches both dialects, and only one
      spelling `permission-denied` exactly does not.  Closing it is one
      more line in fe's `condition_parents[]` and one more arm in
      `src/lisp_io.c`; what it also needs is a way for the suite to
      measure it, since a test that drops privileges is a new kind of
      test here.
- [ ] **`eval`'s LEXICAL argument.**  fe gained Emacs' one-argument
      `eval` at the Phase 12 pin, evaluating its form in the caller's own
      run.  A non-nil LEXICAL is rejected by name, as `macroexpand`'s
      ENVIRONMENT is.  The measured reason it is not simply "inherit the
      caller's scope": Emacs' LEXICAL *selects* an environment and never
      inherits — `(let ((qq 1)) (eval 'qq))` is `(void-variable qq)` on
      31.0.90 — so an implementation would need first-class lexical
      environments as values, which neither tree has.
- [ ] **fe has no `error-message` property, so a `signal`'s message is
      the bare condition name.**  Newly recorded by Phase 12; example
      corrected by its review, which measured the original headline
      example rendering fine — `render_file_condition()` keys on the
      condition SYMBOL, not on who raised it, so a plain Lisp
      `(signal 'file-missing ...)` of the string triple renders
      byte-identically to Emacs.  The true examples: measured,
      `(signal 'error (list "custom msg"))` reports `error` where
      Emacs reports `custom msg`, and
      `(signal 'wrong-type-argument (list 'listp 6))` reports
      `wrong-type-argument` where Emacs reports
      `Wrong type argument: listp, 6`.  kg's renderer is the narrow
      version for the two file classes; the general one is a
      per-symbol message property in fe's hierarchy table and an
      `error-message-string` to read it.
- [ ] **`lisp_raise_wrong_type()`'s route does not reach an enclosing
      `condition-case`.**  Found while measuring Phase 12's file
      conditions, pre-existing, and deliberately not fixed there.
      `(condition-case e (prefix-numeric-value "x") (wrong-type-argument
      ...))` escapes to the host: the helper raises by evaluating a
      `(signal ...)` form through `FeEvaluateWithOptions`, and
      `FeEvaluate`/`FeCall` start a *nested run* whose completion
      transfers to the outermost barrier, past every handler lexically
      between the native and the raise.  It is the same defect 11D
      Part 3 fixed for the loader, and the fix is the same shape —
      `FeTryCall...` plus `FeResignal`, which is what
      `lisp_raise_file_condition()` does.  Visible as part of the
      `condition-case-native-errors` divergence.
- [ ] **`macroexpand-all`**, which Phase 10's 10B left as a
      reject-by-name stub (`unsupported feature: macroexpand-all`, a
      catchable condition, deliberately not `void-function`).  It needs a
      code walker, and with it the `ENVIRONMENT` alist both `macroexpand`
      and `macroexpand-1` currently refuse the same way.  fe's own
      `TODO.md` carries the submodule half.
- [ ] **Bytecode — measured and declined, 2026-08-07.**  Phase 10 answered
      the parent plan's §15 from counters rather than acting on it: none
      of its five triggers fires.  Prelude 0.82 ms, user init 0.59 ms,
      package load 0.44 ms (inside the init), 0 collections, 14.9% of the
      arena live after a representative init that requires two packages.
      The trigger that is *unmeasured* rather than negative is the fifth
      (evaluator dispatch as a dominant cost); its instrumentation is fe
      work and 10A Decision 9 declined to build it for a decision the
      other four settle.  The full trigger table, the three uninstrumented
      §15 measurements and the counters behind them are in
      `test/lisp-compat/README.md`.  A phase that re-opens this funds that
      instrumentation first.
- [ ] **A missing-function channel.** 10A Decision 5: reader syntax kg
      does not implement is rejected *by name* (`unsupported read syntax:
      vector brackets`), and functions it does not implement are not —
      an unimplemented Emacs function is byte-identical to a typo
      (`void-function`).  A curated known-name channel would need an
      unbounded name list and new language machinery, so Phase 10
      re-worded the milestone gate item against what the tree does
      instead of building one.  The three `unsupported` manifest entries
      say so in their rationales.

## Missing Mg features

Features and keybindings present in Mg but missing from kg, roughly
ordered by value vs implementation effort.

### Stability / safety (high priority)

- [ ] **Backup-on-save / simple autosave**: Write a `file~` (or `#file#`)
      safety net on first save in a session.  Cheap to add, saves grief
      when ssh drops mid-edit.

- [x] **`C-q` quoted-insert**: Insert the next keystroke literally.
      Currently there is no way to insert a literal Tab, Esc, or other
      control byte — matters for terminfo, sendmail.cf, Makefiles.

### Lower priority / larger scope

- [x] **M-z zap-to-char**: Kill from point up to and including a
      prompted character.

- [ ] **M-y yank-pop**: After C-y, cycle backwards through the kill ring
      with repeated M-y presses.  Requires expanding the kill ring from a
      single slot to a small ring (Emacs default is 60; even 8 would cover
      most use).

- [ ] **Multi-entry kill ring**: Prerequisite for M-y yank-pop.  Keep
      the ring bounded (8–16 entries) to avoid unbounded memory growth.

- [ ] **M-t transpose-words**: Companion to `transpose-chars`.

- [ ] **Toggle line numbers**: `M-x linum-mode` or similar.  Frequent
      ask, low cost.


- [ ] **Dired follow-ups**: dired mode shipped as a C mode (`src/dired.c`)
      with listing, `RET`/`^`/`g`/`n`/`p`, the `*`/`D` markers and `x`
      delete-flagged.  Deliberately left out of that first version:
      - `C` copy and `R` rename the entry (or the marked entries) at point
      - `+` mkdir in the listed directory
      - `U` unmark all, and capital `D` (`dired-do-delete`) acting on the
        `*` marks, which are inert until something consumes them
      - mark-preserving revert: `g` rebuilds the buffer and the markers are
        buffer text, so they are dropped today
      - `ls -l`-style detail columns (size, mode, mtime) and the sort
        toggles that go with them
      Recursive deletion stays out on purpose: `rmdir` failing on a
      non-empty directory is the safety property, not a limitation.

- [ ] **`draw_window_rows()` wants a render context, not an extraction**:
      at pmccabe 67 (14 parameters) it is the worst symbol left in
      `src/*.c`, and the 2026-08-07 complexity campaign deliberately did
      not touch it.  It is a hot repaint loop with coupled terminal
      state, so the right fix is a render-context struct plus a real
      structural decomposition, designed against `make bench` and
      `test/perfobj/` counter evidence rather than eyeballed -- not the
      "lift some ifs into helpers" shape that would satisfy the ratchet
      and leave the loop no easier to reason about.


## Important (DONE)

- [x] Add hash-bang fallback for detection of syntax highlighting, e.g., #!/bin/sh
- [x] Add markdown-mode with syntax highlighting

## Maybe Later

- [ ] Send alt screen sequences if TERM=xterm: "\033[?1049h" and "\033[?1049l"
- [ ] Add support for modes.  E.g., c-mode with bindings for
      compile/make which opens a compile buffer in a window below
- [ ] **Proper UTF-8 handling**.  `editor.cx` and `mark_col` are byte
      indices, so the cursor can sit inside a multi-byte glyph and
      Right/Left advances one byte at a time through box-drawing or
      accented characters.  The visual-mark renderer now defers its
      reverse-video toggle to glyph boundaries (display-time fix), and
      display columns now go through the width table in `src/width.c`,
      but region extraction, word motion, and the syntax classifier
      (`syntax.c`, where `!isprint()` on high-bit bytes tags every byte
      of a UTF-8 glyph as `HL_NONPRINT` and substitutes `?`, so CJK in a
      file with a syntax renders as a row of `?`) still treat each byte
      on its own.  A real fix means walking by `utf8_glyph_span_at()`
      everywhere we step through chars.

- [ ] **Partial combining-mark coverage in the width table**.
      `src/width.c` encodes the full Unicode 15.1 East_Asian_Width W/F
      set but only a curated slice of the nonspacing marks: the generic
      combining blocks plus Hebrew, Arabic, Syriac, Thaana, NKo,
      Samaritan, Mandaic, Thai, Lao, conjoining Hangul jamo, Mongolian
      and the variation selectors.  The Brahmic scripts (Devanagari and
      the rest of the Indic blocks, Tibetan, Myanmar, Khmer, Balinese)
      and the musical/mathematical marks above U+FE2F still measure one
      column each.  Grapheme clustering is also out of scope: an emoji
      ZWJ sequence or a regional-indicator flag pair measures as the sum
      of its parts, so a flag reports four columns where the terminal
      draws two.

- [ ] **mandoc -T lint nits in doc/kg.1**.  Three pre-existing
      "new sentence, new line" warnings (e.g. around `M-a/M-e`'s
      `Mr.\&` and `e.g.\&`).  Cosmetic; mdoc convention says each
      sentence should begin on its own line.
