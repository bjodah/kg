# TODO In Priority Order

The dependency-ordered implementation program for the next architecture and
feature work is [doc/plans/2026-07-31-follow-ups](plans/2026-07-31-follow-ups/README.md).
This file remains the broader feature and technical-debt inventory.

## LSP follow-ups (v1 landed 2026-08-09)

The `WITH_LSP=1` client is complete per
[doc/plans/2026-08-08-lsp.md](plans/2026-08-08-lsp.md)'s definition of
done: `M-.`, `M-?` and `M-,` over a JSON-RPC stack of its own
(`lsp_transport`, `lsp_json`, `lsp_client`, `lsp_server`, `lsp_sync`,
`lsp_uri`, `xref`), tested against a scripted fake server and against the
real `clangd` and `ty`.  Known follow-ups, none blocking:

- **Result previews in `*xref*`.**  The listing ships as
  `path:line:col:`; the plan's `path:line:col: preview` needs the target
  line, which means reading it from an open buffer when there is one and
  a bounded lazy read otherwise (200 files opened to paint one screen,
  and read from disk is the wrong text for a buffer with unsaved edits).
- **A per-request timeout.**  A server that is alive but stuck answers
  nothing and kg waits forever; only death is detected today.
- **An `*lsp-log*` buffer.**  Server stderr goes to `/dev/null`
  (`kg_process_spawn_bidi()`), so a server that explains its refusal
  explains it to nobody.
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
  `*xref*` is excluded from that predicate by name; a real mode registry
  owning the table is the answer, not a third exclusion.

## Tree-sitter follow-ups (v1 landed 2026-08-08)

The `WITH_TREE_SITTER=1` backend is complete per
[doc/plans/kg-tree-sitter-plan.md](plans/kg-tree-sitter-plan.md)'s
definition of done: 13 registry rows over 11 grammars, incremental
TSInputEdit parsing with damage-limited repainting, differential-tested
against full rebuilds.  Known follow-ups, none blocking:

- **Shell**: `/opt-9/tree-sitter-grammar-bash` is v0.6.0 (grammar
  ABI 6, below tree-sitter 0.26's floor); the loader refuses it and
  `test_bash_grammar_abi_is_rejected()` fails loudly when the pin
  moves.  Adding Shell then is one registry row + one query + tests.
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

### High value, straightforward

- [x] **M-% query-replace**: Interactive find-and-replace.  Prompt for
      search string then replacement, step through matches with y/n/!
      (replace all)/q.  The incremental search machinery in search.c is
      a natural starting point.

- [x] **C-x C-w write-file**: Save buffer to a different filename (Save
      As).  Prompts for a new name, writes, and updates the buffer's
      filename.

- [x] **C-x i insert-file**: Insert the contents of a file at point.

- [x] **M-; comment-dwim**: Toggle or insert a line comment using the
      current buffer's syntax.  Requires plumbing comment-start strings
      from the syntax table through to an editing command.

- [x] **C-x C-x exchange-point-and-mark**: Swap cursor and mark.  Lets
      you visually inspect the other end of a region or bounce between two
      positions.  Trivial to implement.

- [x] **C-o open-line**: Insert a newline at point without advancing the
      cursor.  The classic "make room above the next line" command.  One
      liner.

- [x] Visual mark mode

- [x] Adjust filename in bar, gets cut off if the path is too long.
      Same issue applies to long buffer names; need a sensible ellipsis
      strategy (e.g. shorten leading path components like Emacs does).

- [x] Crash when opening doc/TODO.md

        ~/src/kg(master)$ kg doc/TODO.md 
        malloc(): invalid next size (unsorted)
        Aborted (core dumped)
- [x] Let C-x C-f use the directory of the current buffer as starting point.


### Stability / safety (high priority)

- [x] **`M-!` shell-command** and **`M-|` shell-command-on-region**:
      Run an external shell command, optionally piping the current region
      through it and replacing the region with the output.  For a remote-shell
      editor this is the highest-leverage missing feature — pipe through
      `sort`, `fmt`, `column -t`, `jq`, `sed`, etc.  Mg has it.

- [x] **Tab completion in the minibuffer**: For `C-x C-f`, `C-x i`,
      `C-x C-w` and any other path-prompting command.  Single biggest
      day-to-day friction once everything else works.

- [x] **External-modification detection on save**: When editing
      `/etc/*` something else may have rewritten the file.  Stat the
      file before save; if mtime/size changed since we read it, prompt
      "file changed on disk, save anyway?".  Also: live `(changed)`
      marker in the mode line, plus optional `M-x auto-revert-mode` /
      `global-auto-revert-mode` that silently reload clean buffers.

- [ ] **Backup-on-save / simple autosave**: Write a `file~` (or `#file#`)
      safety net on first save in a session.  Cheap to add, saves grief
      when ssh drops mid-edit.

- [x] **`C-q` quoted-insert**: Insert the next keystroke literally.
      Currently there is no way to insert a literal Tab, Esc, or other
      control byte — matters for terminfo, sendmail.cf, Makefiles.

### Medium value

- [x] **M-x named commands**: Execute a command by name.  Opens the door
      to toggles (auto-fill, overwrite-mode) without burning key bindings.
      Significant infrastructure but makes kg extensible.

- [x] **Keyboard macros  C-x ( / C-x ) / C-x e**: Record and replay a
      sequence of keystrokes.  Even a single-level macro (no nesting)
      covers the vast majority of real use.  F3 and F4 added as aliases
      for start/stop/execute.

- [x] **M-x revert-buffer**: Re-read the current file from disk, discarding
      all unsaved changes (with confirmation prompt if dirty).  Useful after
      an external tool modifies the file.

- [x] **M-^ join-line**: Join current line with the previous one
      (complement to the C-k-at-EOL join-with-next that kg already has).
      Also add `join-line` as an M-x command.

- [x] **M-u / M-l / M-c  upcase / downcase / capitalize word**: Operate
      on the word from point forward.  word.c already walks words; just
      add tolower/toupper passes.  Also add `upcase-word`, `downcase-word`,
      and `capitalize-word` as M-x commands.

- [x] **Git commit mode (and the wider EDITOR-server crowd)**: When kg
      runs as `GIT_EDITOR` — filename matches `COMMIT_EDITMSG`,
      `MERGE_MSG`, `TAG_EDITMSG`, etc. — enter a small dedicated mode
      with Emacs-style `C-c C-c` to finalize (save and exit 0) and
      `C-c C-k` to abort (exit non-zero so git cancels the commit
      with an empty message).  `C-x #` does the same as `C-c C-c`
      for the broader EDITOR-server flow that crontab, sudoedit, hg,
      and friends share.  Pair with syntax highlighting that dims
      comment lines (`#` prefix), warns past column 50 on the
      subject, and a `M-q` that respects column 72 in the body but
      leaves the subject alone.  Lands kg in the "what's your
      $EDITOR" conversation alongside `emacs -nw` and `vim`.
      Implemented as a syntax-flag-detected mode (`SHL_GITCOMMIT`), with no
      Lisp package.

### Lower priority / larger scope

- [x] **C-u universal-argument**: Numeric prefix for repeating commands
      (e.g. C-u 8 C-f moves forward 8 chars).  Moderate plumbing work
      but unlocks power-user workflows.

- [x] **M-z zap-to-char**: Kill from point up to and including a
      prompted character.

- [ ] **M-y yank-pop**: After C-y, cycle backwards through the kill ring
      with repeated M-y presses.  Requires expanding the kill ring from a
      single slot to a small ring (Emacs default is 60; even 8 would cover
      most use).

- [ ] **Multi-entry kill ring**: Prerequisite for M-y yank-pop.  Keep
      the ring bounded (8–16 entries) to avoid unbounded memory growth.

- [x] **M-SPC just-one-space**: Collapse whitespace around point to a
      single space.  Also add `just-one-space` as an M-x command.

- [x] **C-t transpose-chars**: Swap the character before point with the
      one at point.  Indispensable for fixing the most common typing errors.
      Also add `transpose-chars` as an M-x command.

- [ ] **M-t transpose-words**: Companion to `transpose-chars`.

- [x] **Rectangle operations**: `C-x r k` kill-rectangle, `C-x r y`
      yank-rectangle, plus `C-x r d` delete and `C-x r c` clear.
      `C-x r t` string-rectangle is still TODO.  Surprisingly useful
      for config tables, fstab columns, CSV-ish data.

- [x] **Registers**: `C-x r SPC` (point-to-register), `C-x r j`
      (jump-to-register), `C-x r s` (copy-to-register), `C-x r i`
      (insert-register).  32 slots, marker-backed positions, bounded text
      (1 MiB per register, 4 MiB total).  Persistent bookmarks deferred.

- [ ] **Regex search**, or at least a case-sensitivity toggle in
      isearch.  Right now isearch is literal-only and case-sensitive.

- [x] **Verify `M-d` kill-word forward and `M-DEL` kill-word backward**:
      Pair with `M-f`/`M-b` that already exist; add if missing.

- [ ] **Toggle line numbers**: `M-x linum-mode` or similar.  Frequent
      ask, low cost.

- [x] **Minimal config file**: `~/.config/kg/init.el` — done; see the Lisp
      section in README.md (init files, `(load ...)`, `define-command`,
      `global-set-key`). What exists now:
      - an Emacs-shaped position and mark API (`point`, `goto-char`,
        `goto-line`, `point-min`/`point-max`, `line-number-at-pos`,
        `current-column`, `mark`, `set-mark`, `deactivate-mark`,
        `region-beginning`/`region-end`,
        `buffer-substring`, `char-after`, `forward-word`/`backward-word`,
        `bounds-of-thing-at-point`), addressing the buffer by 1-based
        codepoint offsets
      - string natives (`string-length`, `substring`, `concat`, `string=`,
        `char-to-string`, `string-to-char`, `format`), which upstream fe
        lacks; `format` covers `%s`, `%S`, `%d`, `%e`, `%f`, `%g` and
        `%%`, and since Phase 8 also `%c`, `%o`, `%x`, `%X` and the
        `-`/`0` flag, field width and precision that go with them; and
        `message` is a format function like its Emacs namesake
      - an Emacs Lisp prelude evaluated at startup: `defun`, `defmacro`,
        `defvar`, `defconst`, `defcustom`, `custom-set-variables`,
        `declare`, `interactive`, `let`/`let*` with elisp binding
        lists, `progn`, `cond`, `when`, `unless`, `prog1`, `dolist`,
        `dotimes`, `quasiquote`, the list library (`length`, `nth`,
        `nthcdr`, `last`, `reverse`, `append`, `mapcar`, `mapc`,
        `mapconcat`, `assoc`, `assq`, `member`, `memq`, `push`, `pop`,
        `nreverse`, `delq`, `delete`, `add-to-list`), `equal`,
        `string-empty-p`, `thing-at-point`, `identity`, `prog2`, `max`,
        `min`, `documentation`, `number-to-string`, `string-to-list`,
        `setq-default`, `setq-local` and `kbd`;
        `&optional` and `&rest` in argument lists
      - type natives `type-of`, `stringp`, `symbolp`, `numberp`, `consp`,
        `functionp`
      - Emacs names throughout the editor bridge (`insert`, `message`,
        `buffer-name`, `load`, `global-set-key`, `global-unset-key`,
        `command-execute`); the only invented names are `define-command`
        and `remove-command`, which Emacs has no analogue for — and
        `(interactive)` inside a `defun` writes to that registry, so they
        rarely need to be typed
      - the fe submodule carries elisp `if`, `lambda` as the primitive's
        name, the `` ` ``/`,`/`,@`/`#'` reader macros (`#'f` reads as
        `(function f)`, and the writer prints one back as `#'f`),
        Lisp-2 namespaces — a separate value and function cell per
        symbol, with call position resolving the function cell only, and
        the nine namespace forms `function`, `funcall`, `apply`,
        `fboundp`, `symbol-function`, `symbol-value`, `fset`, `defalias`
        and `fmakunbound` as core forms rather than prelude
        definitions — `void-function` errors, `unwind-protect`, core
        `setq` (lexical-aware) and `set` (always the global cell),
        left-to-right chained numeric `=`, and a 4096-slot GC stack; and
        from Phase 6, condition objects `(SYMBOL . DATA)` with a static
        hierarchy plus the forms that raise and catch them — `catch`,
        `throw`, `condition-case`, `signal` and `error` as core forms,
        with `ignore-errors` the prelude's macro over the last of them —
        and the host-side completion surface kg's seams are built on:
        `FeGetCompletion`/`FeGetCondition`/`FeGetCompletionMessage`, the
        protected call `FeTryCallWithOptions` and `FeResignal`; and from
        Phase 9, exhaustion as an ordinary catchable condition
        (`arena-exhaustion`, `evaluation-stack-exhaustion`, both under
        `error`, pre-built at context open so signalling one allocates
        nothing) and a mark phase that uses no C stack at all — the
        collector's `car` recursion is replaced by pointer reversal, so
        the last unbounded data recursion in the interpreter is gone and
        collecting a deep structure no longer depends on the host's stack
        limit; see `doc/fe-upstream.md`

      Remaining Lisp follow-ups:
      - no call-trace exposure through the host error callback
        (the `call_trace` parameter to `handle_error` is discarded;
        exposing structured call traces is a debugger-shaped feature)
      - re-classification of kg's 112 prose raise sites into named
        condition objects (deferred per 06A Decision 2; most raise
        `(error "text")` which is Emacs' own shape for prose errors).
        Until it lands, `(condition-case e (goto-char "x")
        (wrong-type-argument …))` does *not* match, which
        `test/lisp-compat/features.json`'s
        `condition-case-native-errors` row records as a divergence
      - ~~`save-excursion`/`with-current-buffer` expanded to Lisp
        `unwind-protect`~~ — **done in Phase 11 (11A Decision 6).**  Both
        are prelude macros over `unwind-protect` around two capture/restore
        natives, so no native frame stands between a `throw` and its
        `catch`.  The remaining native re-entry walls are the callbacks kg
        invokes from its own C — hooks, process filters and sentinels, a
        nested `command-execute` — and they stay walls: there is no prelude
        expansion that removes those frames
      - ~~errors raised while evaluating a file through `(load ...)`~~ —
        **done in Phase 11 (11A Decision 5, Shape A).**  `lisp_eval_file`
        uses fe's `FeTryEvaluateStringWithOptions`, unwinds the loader
        bookkeeping the frame owns and `FeResignal`s, so a
        `condition-case` around the loader catches with the original
        condition.  `load` also answers `t` now.  Two pieces did not
        close then; Phase 12 closed one of them (`file-missing`) and
        measured the other (`throw`) into a different, larger shape than
        11A had assumed.  Both have their own items below
      - ~~A `throw` out of a loaded file (Shape B)~~ — **done in Phase
        12's fix cycle**, by exactly the design (c) this item measured
        out, once fe's side of the blocker was built: the fix cycle
        added the input-unit trio
        `FeEnterInputUnit`/`FeReadInputForm`/`FeLeaveInputUnit`
        (`FE_API_VERSION` 8, the fix-cycle pin move), and `load` and
        `require` became prelude loops `eval`ing each form in the
        current run — per-form `path:LINE` latched by the incremental
        reader, input-unit defvar scoping intact, incremental
        read-eval timing pinned as an oracle case
        (`load-error-timing`).  The flipped `load-throw-reachability`
        row and the `load-dynamic-extent` cases carry the evidence.
      - ~~a cleanup that raises AND HANDLES its own error overwrites the
        in-flight condition~~ — **done in Phase 12** (12A Decision 2, fe
        `95965f0`), and the defect was broader and simpler than this item
        said.  It was not about the drain at all: a `condition-case` or
        `ignore-errors` established inside an `unwind-protect` CLEANUP
        never handled anything, with or without an unwind in flight, and
        `(unwind-protect 'body (ignore-errors (car 6)))` escaped to the
        host where Emacs answers `body`.  The seam was one ordering —
        `RaiseCompletionCore` tested `ctx->cleanup_catch` and replayed to
        `RunOneCleanupEntry`'s `setjmp` before it ever called
        `FindConditionHandler` — and the fix accepts a found handler only
        at or above the frame floor the cleanup entry already saved, so a
        **native** cleanup is bit-identical.  06A Decision 4 measured
        correct and is preserved: an *unhandled* cleanup raise still
        replaces the completion being unwound.  Six oracle cases under
        `phase12-cleanup-handler-visibility`, including two guards for
        Decision 4
      - ~~`file-missing` as a condition subtype~~ — **done in Phase 12**
        (12A Decision 4, fe `b030d0a`), and the blocker this item
        recorded was **stale when it was written**: `file-error` has been
        a row of fe's `condition_parents[]` since Phase 6, the
        inheritance test in the handler search has always existed, and
        `file-missing` under it was one data line.  kg's two sites — the
        loader's cannot-open and `require`'s cannot-find — raise it with
        Emacs' `(OPERATION STRERROR PATH)` data, and the diagnostic is
        Emacs' `error-message-string` rendering of it.  Recorded as
        `phase12-file-conditions`
      - token/cancel cleanup registry — **not Phase 9's, after all**.
        09A Decision 4 measured it and left it where it was: it is
        designed but unbuilt (`fe/doc/unwind-design.md` item 2), and its
        one concrete defect (`fex_io.c`'s `MakeFile` leaking a `FILE*`
        when `FeMakePtr` raises) is in the `fex_*.c` files kg does not
        link.  It stays **fe-standalone debt**, owned by the submodule
        and priced there; pulling it into a kg phase would double the
        phase's fe price for no kg benefit
      - Phase 8 deferrals, each measured absent at the phase close and
        each an init-file compatibility gap rather than a nicety:
        - no `string-match`/`match-string` over a string; kg's regexp
          surface is buffer-search only (`re-search-forward` and friends)
        - no `symbol-name`, `intern` or `intern-soft`, so a program
          cannot compute a name and a keyword cannot be turned back into
          text
        - no vectors, and no `#:` uninterned-symbol syntax: both are
          named read errors rather than misreadings, recorded as
          `phase8-reader-unsupported-syntax`.  A vector needs an object
          type the fe core does not have
        - `load-path` is still a C array; `(add-to-list 'load-path ...)`
          cannot reach it, and `add-to-load-path` is kg's spelling
        - source positions are per top-level form only.  A read error
          reports a byte offset rather than a line, because
          `FeReadString` has no label to count lines against, and an
          error inside a function reports the top-level form that called
          it rather than the sub-form that raised
        - no `read`, `read-string`, `read-from-string` or any other
          `read-*` entry point; `(interactive "...")` codes are the only
          way to prompt
        - Customize is a declaration, not a system: no `defgroup`,
          `setopt`, `custom-file`, saved state or UI.  `defcustom` is a
          `defvar` that records a docstring and accepts inert
          presentation keywords, and `custom-set-variables` is a
          `setq` over quoted `(SYMBOL VALUE)` pairs
        - no `format-message` (Emacs' curved-quote translation), and
          `%S` has no print-depth or print-length control -- fe's writer
          truncates with `#<deep>`/`#<truncated>` on its own budgets
        - no `*Messages*` ring: `message` writes the status line and the
          text is gone when the next message replaces it
      - editor option variables (`tab-width`, `auto-fill-column`, ...) exposed
        to Lisp; still only commands/bindings and the editing bridge exist
      - grow the `command-execute` allow-list deliberately (policy per
        command).  The list is gone: it is the `CMD_LISP_CALLABLE` flag
        on `cmdtable` in `src/cmd.c`, still the same eleven commands,
        and `cmd_invoke()` is the one place that reads it
      - **word constituents disagree between the two layers**: the global
        `is_word_char()` in `src/word.c` is ASCII-only (`isalnum() || '_'`),
        so `M-f`, `M-b`, `M-@`, `M-d` and the Lisp `forward-word` /
        `backward-word` that drive them stop inside "héllo", while the Lisp
        `bounds-of-thing-at-point` treats every codepoint from U+0080 up as
        a word constituent and returns the whole word. Deliberate for now —
        making interactive word motion codepoint-aware is part of the
        "Proper UTF-8 handling" item below, not a Lisp change
      - extend the keypress fuzz harness to drive `eval-expression` once it
        can run without filesystem side effects
      - upstream Fe: `FeCallWithOptions` (landed) lets hosts budget a bare
        `FeCall`; per-context custom types remain deferred

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

- [x] Refactor code to Linux style, variable decl. at top of context sorted
      in reverse chrismas tree style, lines can be up to 110 chars long
- [x] Add hash-bang fallback for detection of syntax highlighting, e.g., #!/bin/sh
- [x] Fix delete key
- [x] Add auto-indent à la Mg (consider M-x auto-indent-mode toggle or
      C-j vs Enter distinction)
- [x] Add built-in help, similar in style to whay my fork of Mg has, see
      ~/src/mg/ for details
- [x] Add markdown-mode with syntax highlighting
- [x] Add support for multiple buffers, supporting copy-paste between
      them, i.e., shared kill ring
- [x] Add support for split windows, both horizontal and vertical
- [x] Change the mode line to be more similar to Emacs, the current
      active window marker '**' is so easily mistaken for "aha a modifed
      buffer", we could instead use ansi escape sequences to set all the
      non-selected windows as "dim"
- [x] With two buffers open, we should only need to ask "are you sure"
      if any buffer is modified and not saved when exiting
- [x] Testing and stability to reach "usable" level
- [x] Add support for Emacs' M-q to "reflow" a paragraph

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

- [x] **Horizontal-scroll + tab units mismatch in display.c**.  Fixed:
      `coloff` is a chars-byte offset (`coloff + cx == filecol`), which
      is what every other module already assumed, and the two places
      that read it as a render offset now convert.  The row-drawing
      loop slices `r->render` at `chars_to_render_col(r, coloff)`, and
      the cursor column is the difference of two `editor_visual_col()`
      readings instead of a second, subtly different, walk of the row.
      A scrolled window used to be drawn one tab expansion off and not
      contain point at all
      (`test/pty/horizontal-scroll-tab-slices-render.yaml`).
      See `doc/coordinates.md` row 11b.

- [ ] **mandoc -T lint nits in doc/kg.1**.  Three pre-existing
      "new sentence, new line" warnings (e.g. around `M-a/M-e`'s
      `Mr.\&` and `e.g.\&`).  Cosmetic; mdoc convention says each
      sentence should begin on its own line.

- [x] **Multi-byte input in the minibuffer is broken**.  Fixed: the
      prompt reader dropped every byte above ASCII (`isprint()` rejects
      them), so the search string stayed empty and the following keys
      fell through to the buffer.  Prompts now pull in the rest of the
      UTF-8 sequence (`editor_read_utf8_seq`) and insert it whole, and
      backspace, C-d, C-b/C-f and the echo-area cursor column all work
      in glyphs and display cells instead of bytes.

- [x] **Typing a multi-byte character into the buffer still does
      nothing**.  Fixed: the self-insert arm accepted only TAB and
      32..126, so both bytes of a typed `å` were dropped before they
      reached `editor_self_insert_char`.  A byte above ASCII now
      collects its continuation bytes (`editor_read_utf8_seq`, so
      keyboard macros still replay) and inserts the glyph as one unit
      through `editor_self_insert_glyph`; a malformed sequence is
      dropped rather than half inserted, and read-only buffers refuse
      the lead byte like any other editing key.  The two open decisions
      resolved as: **undo** follows yank, the other command that
      inserts multi-byte text — one `UNDO_YANK_TEXT` record whose
      reverse deletes the glyph's bytes forward, so `C-_` removes the
      whole character and leaves valid UTF-8 (overwrite mode reuses
      `editor_overwrite_char`'s `UNDO_REPLACE_TEXT` record, now with a
      replacement longer than one byte); **`editor.cx`** stays a byte
      offset into `row->chars` and advances by the glyph's byte length,
      which is exactly where `C-f` over the same glyph lands.

- [x] **Regex follow-ups from the Emacs-fidelity work.**
      Both infrastructure halves are done: the differential fuzzer
      against the Emacs oracle is now `make check-regex-differential`
      (`utils/regex_differential.py`, `utils/regex_oracle.el`,
      `test/regex_differential.c`), run in CI by
      `.ci/ci-10-regex-differential.sh`; and the regex fuzz corpus has
      a tracked home in `test/fuzz-seeds/regex`, copied into the
      gitignored working corpus by `make fuzz-regex-seed`.  The one
      non-deliberate divergence they turned up is fixed too, below.
      Known deliberate divergences that remain: `\w` `\d` `\s`, case
      folding and the POSIX classes are ASCII-only (Emacs' `\w`
      matches `å`); a quantifier on a quantifier (`a\{2\}\{2\}`) is
      valid in Emacs but never matches here.
      **Fixed: the capture register after an empty repetition of a
      quantified group.**  `\(x*\|a\)\{2\}b` against `ab` used to give
      group 1 as `1 1` where Emacs gives `0 1`.  The first write-up
      here read it as "Emacs keeps the last iteration that consumed
      anything", and scoped it to `\{n\}`; probing the oracle over the
      whole `\{n,m\}` grid said otherwise on both counts.  Emacs
      compiles a repeat with a non-zero minimum as a counted loop with
      *no* empty-match check, so an empty repetition does not end the
      loop until the minimum is behind it — repetition 1 of
      `\(x*\|a\)\{2\}` matches the empty branch at 0 and repetition 2
      then matches `a`.  Past the minimum the check is back, which is
      why `*`, `\{0,m\}` and `\{n,\}` always agreed, and why enough
      slack over the minimum (`\{2,4\}b` on `ab`) reaches the trailing
      empty repetition again.  `\{n,n+1\}` diverged too and was never
      in the original scope.  `match_rep()` in `fe/tiny-regex-c/re.c`
      now repeats while `done <= min` even on an empty body; kg and
      Emacs agree over 32k hand-built cases spanning the grid, and
      `--cases 200000` agrees on seeds 12 (which used to fail), 13, 14,
      15 and 20260729.  Span 0 was never affected, so only `\1`-style
      references saw it.
