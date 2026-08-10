## Lisp in kg

Use `M-x eval-expression` (bound to `M-:`) to evaluate Lisp entered in the minibuffer,
`M-x eval-buffer` to evaluate the current buffer, or `C-j` to evaluate the
s-expression before point (in `*scratch*` and Lisp Interaction/Lisp buffers
`C-j` inserts the result; elsewhere it behaves as a plain newline). Results and
labelled errors are shown in the status area. A build made with `WITH_LISP=0`
keeps all commands available and reports that Lisp was not compiled in.

Lisp runs in a fixed 1 MiB arena that never grows, so a program can exhaust
it — `out of memory` in the status area is that, not the host running out.
Exhaustion is survivable and catchable: `(condition-case e BIG (error
'caught))` catches it, and so does a handler naming `arena-exhaustion`
(`evaluation-stack-exhaustion` for a full GC root stack). `M-x
lisp-arena-stats` reports what is left — slots total/free/peak,
collections, peak GC roots, peak frames against capacity, and failed
allocations — and allocates nothing itself. An exhaustion whose data is
reachable from a *global* keeps the arena pinned: the session stays alive
and keeps reporting truthfully, but the space does not come back until kg
is restarted.

On startup kg loads `$XDG_CONFIG_HOME/kg/init.el` (falling back to
`~/.config/kg/init.el`); a missing file is normal, and `-Q` skips loading
entirely so a broken configuration can be repaired. Load errors show the
labelled diagnostic in the status area; forms evaluated before the error
remain applied.

The smallest useful `init.el` is one line. This turns off the startup
screen — the centred logo an empty buffer shows:

```elisp
(setq inhibit-startup-message t)
```

`inhibit-startup-screen` is Emacs' other name for the same switch, and
either spelling suppresses the screen; kg reads them once, after `init.el`
has run. In Emacs the two names are one variable; in kg they are two, so
setting one does not change what the other reads back. Neither touches the
`Press Ctrl-h for help` greeting in the status area — that is Emacs'
separate `inhibit-startup-echo-area-message`, which kg does not have.
`doc/lisp-api.md` lists every variable the editor itself reads.

Interactive Lisp commands use the declaration immediately after an optional
docstring to construct arguments. Supported codes are `p`, `P`, `r`, `s`,
`n`, `N`, `f`, `F`, `b`, and `B`: strings, decimal numbers, paths, and buffer
names are read through the existing minibuffer pickers without visiting or
selecting anything. `N` uses the numeric prefix when supplied. Prompts are
literal, available only from a real key or M-x command context, and capped at
16 arguments. `C-g` quits and input overflow is an error before the body runs;
kg does not provide public `read-*` functions or a completion framework.

A complete `init.el` command, in ordinary Emacs Lisp — the docstring and
`(interactive)` make it available as `M-x my/select-current-line`, and
the region it sets is live, so `C-w` kills the selected line:

```elisp
(defun my/select-current-line ()
   "Select the current line."
   (interactive)
   (move-beginning-of-line 1)
   (set-mark (point))
   (move-end-of-line 1))
```

Extension packages load explicitly with `(load "name")`, which resolves a
bare name to `<config>/kg/lisp/name.el` and treats names containing `/` as
literal paths. A bare name may be written with or without the `.el` suffix;
both spellings resolve to the same file, as in Emacs. Packages may load other packages; loading a file twice with
`load` evaluates it twice, and `load` answers `t` when it succeeds. An
error raised by a loaded file — at read time or run time, and including
the nesting-depth limit and a missing file — is catchable by a
`condition-case` written around the `(load ...)`, with its original
condition symbol. A file that is not there raises Emacs' own
`file-missing` — `(file-missing "Cannot open load file" "No such file or
directory" PATH)`, and the same condition and data from `require` when
nothing in the load-path matches — so a handler may name `file-missing`,
its parent `file-error`, or `error`. A permission failure raises the
parent class `file-error`, where Emacs raises its third leaf
`permission-denied` — recorded rather than implemented. A `throw` out of a loaded file
reaches a `catch` around the `load`, as it does in Emacs: loaded forms
are read and evaluated one at a time in the caller's own run, so
errors, throws and quits cross the `load` as if the forms were written
in place. Init files and packages are trusted code with the
full privileges of the editor process, bounded only by the evaluation step
budget and `C-g` cancellation — **kg's Lisp is not a sandbox.**
`doc/lisp-api.md` is the full reference (object lifetimes, position units,
callback ordering, error handling); this section is the narrative tour.

`defcustom` is the declaration subset: it initializes an unbound global like
`defvar`, records its docstring, returns the variable symbol, and accepts inert
presentation keywords (`:type`, `:options`, `:group`, `:tag`, `:link`,
`:version`, and `:package-version`). `custom-set-variables` accepts only
quoted `(SYMBOL VALUE)` entries. Customize state and semantic keywords are not
implemented. `declare` immediately after a definition's optional docstring is
an accepted no-op. Load errors report `FILE:LINE: CONDITION` in the status
line; missing files name the resolved path.

`require`/`provide`/`featurep` give a package a way to load at most once:

| Form | Result |
| ---- | ------ |
| `(provide FEATURE)` | Register `FEATURE` (a symbol or string) as loaded; returns `FEATURE` |
| `(require FEATURE &optional FILENAME)` | No-op if `FEATURE` is already provided; else resolves `FILENAME` (or `FEATURE`'s own name) through `load-path` — with or without the `.el` suffix, exactly as `load` resolves a bare name — and evaluates it, erroring if the feature is still not provided afterward |
| `(featurep FEATURE)` | `t`/`nil`, without loading anything |
| `(add-to-load-path DIR)` | Prepend `DIR` to the bounded load-path (searched before every directory already in it) |

A `FILENAME` containing `/` is a literal path for `require` too: it is
neither suffixed nor searched.

`load-path` defaults to one entry, `<config>/kg/lisp/`, and is a bounded
C-side array rather than a Fe list a package could `(push ...)` onto directly
— `add-to-load-path` is the mutator. `require` re-entered for a feature
already mid-load is a "cyclic require" error naming it, independent of
`load`'s own nesting depth limit. `lisp/auto-fill.el` is a worked package
using all three: `(require 'auto-fill)` then `(auto-fill-mode)` breaks lines
at `fill-column` as they are typed past it, using `after-change-functions`
and one `replace-region` call per break (one undo step). It also shows how a
package handles its own errors: the hook entry point is a `condition-case`
that stores the condition in `auto-fill--error`, removes itself from the hook
and reports once, so a `fill-column` set to something that is not a number
disables the mode instead of erroring on every keystroke.

`make install` puts it in `$(prefix)/share/kg/lisp/` — `/usr/local/share/kg/lisp`
by default. That directory is **not** on the default `load-path`, which is the
per-user `<config>/kg/lisp/` alone, so reach it from `init.el` with:

```elisp
(add-to-load-path "/usr/local/share/kg/lisp")
(require 'auto-fill)
(auto-fill-mode)
```

or copy the file into `<config>/kg/lisp/` and require it with no extra line.
`lisp/prelude.el` is deliberately not installed: it is compiled into the
binary, so it can never be missing or a version behind the editor that
evaluates it.

`lisp/pipeline.el` and `lisp/pipeline-text.el` ship beside it as a worked
two-file package: `pipeline.el` is pure Lisp — higher-order steps folded over
a value, with closures, `funcall`/`apply`, `catch`/`throw`, `condition-case`,
macros and reflective `macroexpand-1`/`macroexpand` — and `pipeline-text.el`
`(require)`s it and adds the interactive commands that run a pipeline over the
current buffer. `pipeline.el` touches no buffer, window, key or process, and
runs unchanged under GNU Emacs 31: `make lisp-oracle-check` loads that exact
file on both sides and compares kg's answers with the pinned Emacs' own.

Buffer positions use Emacs' convention: a position is a 1-based codepoint
offset, so `(point-min)` is 1, `(point-max)` is one past the last character,
and every line break counts as one character. Offsets count characters, not
bytes, so multi-byte text addresses the same way it reads.

| Form | Result |
| ---- | ------ |
| `(point)` | Position of point |
| `(point-min)` / `(point-max)` | Buffer bounds |
| `(goto-char N)` | Move point to `N`, clamped to the buffer |
| `(goto-line N)` | Move point to the beginning of line `N`, clamped to the buffer |
| `(line-number-at-pos)` | 1-based line of point |
| `(current-column)` | Display column of point (tabs expand, wide characters count two) |
| `(mark)` | Position of the mark, or `nil` |
| `(set-mark N)` | Set the mark at `N` and activate the region |
| `(deactivate-mark)` | Drop the region highlight, keep the mark |
| `(region-beginning)` / `(region-end)` | Region bounds; an error with no mark |
| `(buffer-substring BEG END)` | Text between two positions, in either order |
| `(char-after)` / `(char-after N)` | Codepoint as a number, `nil` at end of buffer |
| `(forward-word)` / `(forward-word N)` | Move point over `N` words |
| `(backward-word)` / `(backward-word N)` | Move point back over `N` words |
| `(forward-char &optional N)` / `(backward-char &optional N)` | Move point over `N` characters; a line break is one |
| `(forward-line &optional N)` | `N` lines on, to the beginning of a line; answers how many of `N` were not travelled |
| `(beginning-of-line &optional N)` / `(end-of-line &optional N)` | Start/end of the line `N - 1` lines on from this one |
| `(beginning-of-buffer)` / `(end-of-buffer)` | `(goto-char (point-min))` / `(goto-char (point-max))` |
| `(skip-chars-forward SPEC &optional LIM)` / `(skip-chars-backward SPEC ...)` | Move over characters in `SPEC`; answers the signed distance |
| `(buffer-file-name &optional BUF)` | The file `BUF` visits, or `nil` |
| `(buffer-modified-p &optional BUF)` / `(set-buffer-modified-p FLAG)` | Read and write the unsaved-changes flag |
| `(bounds-of-thing-at-point THING)` | Cons `(START . END)` for `'word` or `'line`, or `nil` |

`goto-line` counts lines from 1 and, like its Emacs namesake, takes no
column: reach one by moving on from the beginning of the line with
`(goto-char (+ (point) N))`.

`bounds-of-thing-at-point` returns a cons cell, so `car` and `cdr` read the
two positions just as they do in Emacs, and both things are bounded the way
Emacs bounds them. `'word` names the word containing point: point immediately
after a word still belongs to that word, and point between two words yields
`nil`. `'line` runs from the start of the line to the start of the next one,
so it includes the line break; on the last line it ends at `(point-max)`.
Any other symbol is an error rather than a silent `nil`.

Editing and search go through the same gateway every command does, so each
call below is one undo step:

| Form | Result |
| ---- | ------ |
| `(delete-region START END)` | Delete the region; positions in either order |
| `(replace-region START END TEXT)` | The region becomes `TEXT`, as one edit |
| `(search-forward STRING &optional BOUND)` | Literal search to `BOUND` (default `point-max`); moves point past the match, or `nil` |
| `(search-backward STRING &optional BOUND)` | Literal search to `BOUND` (default `point-min`); moves point to the match's start, or `nil` |
| `(re-search-forward PATTERN &optional BOUND)` | Regexp search forward; an error on a bad or too-complex pattern |
| `(re-search-backward PATTERN &optional BOUND)` | Regexp search backward; see the caveat below |
| `(match-beginning N)` / `(match-end N)` | Group `N`'s bounds from the last search, or `nil` |
| `(delete-char &optional N)` | Delete `N` characters after point, or `-N` before it |
| `(erase-buffer)` | The whole buffer becomes empty |
| `(looking-at REGEXP)` | `t` when `REGEXP` matches at point; sets the match data, moves point nowhere |
| `(make-marker)` | A marker that points nowhere until something sets it |
| `(point-marker)` | A marker at point in the current buffer |
| `(copy-marker &optional POSITION TYPE)` | A marker at `POSITION`, or nowhere when it is omitted |
| `(set-marker MARKER POS &optional BUF)` | Move `MARKER`; `POS` nil detaches it |
| `(marker-position MARKER)` / `(marker-buffer MARKER)` | Where `MARKER` points, or `nil` if nowhere |

None of the search natives wrap around the buffer, unlike `C-s`/`C-r`, and
none of them fold case. A match cannot span two lines, the same limit
incremental search has. `re-search-backward`'s notion of "the match" in a
line that holds more than one is not Emacs' bounded backward search — see
`src/lisp_search.c` for the exact rule. Search and `match-beginning`/
`match-end` are separate top-level operations only in the sense that match
data outlives the search that set it, the same way point outlives a
`goto-char`. `set-buffer`, by contrast, lasts only for the top-level form
that calls it: the next command starts again in the window's buffer. Use
`with-current-buffer` to scope it explicitly.

Point is a property of the buffer rather than of the running form, so a
buffer remembers where Lisp left point in it even while no window shows
it. Only the buffer the form started in is synchronised back to the
window, and only when the form returns without an error — work in a
buffer no window displays never moves a displayed cursor.

These forms restore what they saved on every exit, including an error or
`C-g`, because Fe now has real cleanup records behind them:

| Form | Result |
| ---- | ------ |
| `(save-excursion BODY...)` | Restores point and the current buffer afterwards |
| `(with-current-buffer BUF BODY...)` | Evaluates `BODY` with `BUF` current, then restores; never selects a window |

Hooks let Lisp run when the editor does something. Callbacks run at a safe
point after the event, never in the middle of the edit that caused it, and
one failing hook neither stops the others nor disturbs the form that was
running:

| Form | Result |
| ---- | ------ |
| `(add-hook 'HOOK FN &optional LOCAL)` | Add `FN`; `LOCAL` restricts it to the current buffer |
| `(remove-hook 'HOOK FN)` | Remove `FN` from `HOOK` |
| `(run-hooks 'HOOK)` | Run `HOOK`'s functions now |

The hooks that exist are `after-change-functions` (called with buffer,
start, end and the replaced length), `find-file-hook`, `before-save-hook`
and `after-save-hook`. There is deliberately no `post-command-hook`: its
per-keystroke cost has not been measured. A hook that is added twice runs
twice, and a hook list holds at most 16 functions.

`FN` may be a function value or a quoted symbol naming one: `(add-hook
'my-hook 'my-fn)` works, as it does in Emacs. A symbol is resolved when the
hook runs, not when it is added, so redefining `my-fn` afterwards takes
effect. An unbound symbol is reported as an ordinary hook error naming it.

kg tracks up to 8 child processes at once, started from Lisp:

| Form | Result |
| ---- | ------ |
| `(start-process NAME BUFFER PROGRAM &rest ARGS)` | Exec `PROGRAM` with `ARGS` directly — no shell, so no word splitting, globbing or redirection touches an argument |
| `(start-shell-command NAME BUFFER COMMAND)` | Run `COMMAND` through `/bin/sh -c`, the way `M-!` does |
| `(process-live-p PROC)` | `t` while `PROC` is running, else `nil` |
| `(delete-process PROC)` | `SIGTERM`, a bounded wait, then `SIGKILL` if it is still alive; releases the slot |
| `(process-buffer PROC)` | `PROC`'s target buffer, or `nil` once it is gone |
| `(set-process-filter PROC FN)` | `FN` is called `(FN PROC STRING)` with each chunk of output; `nil` restores the default of appending to `(process-buffer PROC)` |
| `(set-process-sentinel PROC FN)` | `FN` is called `(FN PROC EVENT)` once the process exits, with `EVENT` a string such as `"finished\n"` |
| `(process-status PROC)` | `'run`, `'exit` or `'signal` |

`BUFFER` is a buffer object, a name (matching `get-buffer-create`'s own
resolution: an existing buffer of that name, else a fresh one), or `nil` to
discard the process's output. `NAME` is validated as a string but not
otherwise used — there is no `process-name` yet. A process object is not a
PID: it is a generation-checked handle into a bounded table the same way a
buffer or marker is, so a handle to a process whose slot has since been
reclaimed by a later one never resolves to that later process.

Setting a filter stops output from landing in `process-buffer` — the filter
owns it, exactly as in Emacs — and clearing it (`(set-process-filter PROC
nil)`) resumes appending. Both callbacks run only at a safe point, after the
process's own output and exit have been decided, and a filter always sees
everything a process wrote before its sentinel runs, even when the exit
itself was detected before the last chunk of output was delivered. A filter
or sentinel that errors is reported in the status area and does not stop
the other one, another process's callbacks, or later deliveries.

Only pipes, not a pty: a child that behaves differently when it is not
talking to a terminal behaves that way here too. Children get `/dev/null`
on stdin; there is no `process-send-string`. Output is capped at 256 KiB
per process — a child that produces more has its oldest bytes dropped, with
a `kg: output truncated` line marking the gap — and every tracked process is
killed and reaped when kg exits, so none outlives the editor.

Key bindings reach the same layered keymaps the built-in keys use:

| Form | Result |
| ---- | ------ |
| `(define-key MAP KEY COMMAND)` | Bind `KEY` in `MAP`; a `nil` `COMMAND` unbinds |
| `(lookup-key MAP KEY)` | What `MAP` alone says `KEY` means, or `nil` |
| `(current-local-map)` | The active major-mode map, or `nil` |

Map names are kg's own — `global`, `dired`, `compilation` — and the Emacs
spellings `global-map` and `dired-mode-map` resolve to them. `lookup-key`
reports what the named map says whether or not that map is currently
active, which is not the same question as what the next keystroke will do.
Unlike `global-set-key`, which only accepts `C-c <key>`, `define-key`
takes any sequence the built-in maps could hold — so it can shadow a
built-in binding, and `C-g` and `C-x C-c` are the keys to leave alone.

Its word constituents include every codepoint from U+0080 up, so `héllo` and
`漢字` come back whole. This is the one place that is true: the interactive
word commands (`M-f`, `M-b`, `M-@`, `M-d`, `M-t`) are ASCII-only and stop at
the first accented character, and so are the Lisp `forward-word` and
`backward-word`, which drive the same editor primitives.

Upstream fe has no string operations, so kg registers its own. They index by
codepoint like the position API, so no result is ever cut mid-glyph. Almost
all of the Emacs Lisp surface is bought this way — as kg natives and as the
prelude below — rather than in the `fe/` submodule; `doc/fe-upstream.md`
lists the few changes that did have to be made there, and why.

| Form | Result |
| ---- | ------ |
| `(string-length S)` | Length of `S` in characters, not bytes |
| `(substring S FROM)` / `(substring S FROM TO)` | 0-based character indices; negative counts from the end |
| `(concat A B ...)` | Joins any number of strings; `(concat)` is `""` |
| `(string= A B)` | `t` when the strings are equal, else `nil` |
| `(char-to-string N)` | One-character string for codepoint `N` |
| `(string-to-char S)` | First codepoint of `S` as a number, `nil` for `""` |
| `(format FORMAT ARG ...)` | Substitutes `%s`, `%S`, `%d`, `%o`, `%x`, `%X`, `%c`, `%e`, `%f` and `%g`, each with an optional `-`/`0` flag, field width and precision; `%%` is a literal per cent |

`substring` clamps out-of-range indices instead of signalling, and a `TO`
before `FROM` yields `""`. `char-to-string` rejects 0, surrogates and values
above `U+10FFFF` so the result is always well-formed text; it is the inverse
of `char-after`, which returns a number.

`format` takes the specifiers Emacs Lisp reaches for most. `%s` prints
an object the way the interpreter prints it — a string bare, a list as a
list, `nil` as `nil` — and `%S` is the same with strings quoted, and with
a quoted form abbreviated the way Emacs abbreviates it, so
`(format "%S" ''x)` is `"'x"`; `%d`
accepts either number type, printing an integer exactly and truncating a
float toward zero; and `%e`, `%f` and `%g` are the C floating-point
conversions, accepting either number type as they do in Emacs. `%o`, `%x`
and `%X` print an integer in octal or hexadecimal, with an explicit sign
in front of the magnitude rather than C's two's-complement rendering, and
`%c` writes one codepoint as UTF-8. Every specifier takes an optional
`-` (left-align) or `0` (zero-fill) flag, a field width and a precision,
with the same meanings as in Emacs: a precision is a digit count for `%d`,
`%o` and `%x`, decimal places for `%e`, `%f` and `%g`, and a maximum
character count for `%s`, `%S` and `%c`. Emacs' remaining flags — `+`,
a space, `#` — and its `N$` field numbers are not accepted, and
`(format "%c" 0)` is an error here where Emacs writes a NUL byte, because
nothing kg stores in a string may contain one. Extra
arguments are ignored, as in Emacs, while a missing argument, an unknown
specifier and a format string ending inside one are all errors. NaN and
the infinities have no integer to print, so `%d` refuses them (where
Emacs writes `nan` and `inf`); `%e`/`%f`/`%g` print them the way C does,
which is the spelling Emacs uses for those specifiers too; and `%s`/`%S`
print them in the interpreter's own readable float syntax — `1.0e+INF`
and `-0.0e+NaN` — exactly as Emacs does.

kg also evaluates a prelude at startup, written in Fe, so the Emacs Lisp
surface is available before any init file runs. It is what makes kg's
`init.el` read like Emacs'.

| Group | Forms |
| ---- | ------ |
| Definitions | `defun` `defmacro` `defvar` `defconst` `defcustom` `custom-set-variables` `declare` `interactive` `lambda` |
| Binding | `(let ((VAR VALUE) ...) BODY...)` `let*` `(setq VAR VALUE ...)` `(set 'VAR VALUE)` `progn` `(special-variable-p 'VAR)` — a `let` binding is lexical unless `defvar`/`defconst` marked the name special |
| Control | `cond` `when` `unless` `prog1` `(dolist (VAR LIST [RESULT]) BODY...)` `(dotimes (VAR COUNT [RESULT]) BODY...)` |
| Non-local exits | `(catch TAG BODY...)` `(throw TAG VALUE)` `(condition-case VAR BODY (CONDITION HANDLER...) ...)` `(signal 'SYMBOL DATA)` `(error "FORMAT" ARG...)` `(unwind-protect BODY CLEANUP...)` `(ignore-errors BODY...)` — core Fe forms except `ignore-errors`, which is the prelude's macro over `condition-case` |
| Lists | `length` `nth` `nthcdr` `last` `reverse` `append` `mapcar` `mapc` `mapconcat` `assoc` `assq` `member` `memq` `push` `pop` `nreverse` `delq` `delete` `add-to-list` `caar` `cadr` `cddr` `1+` `1-` |
| Predicates | `null` `eq` `eql` `equal` `zerop` `integerp` `floatp` `listp` `type-of` `stringp` `symbolp` `numberp` `consp` `functionp` `commandp` `keywordp` `boundp` `fboundp` — only `null`, `equal`, `zerop` and `listp` are prelude definitions; `eq`, `eql`, `integerp`, `floatp`, `keywordp`, `boundp` and `fboundp` are core Fe primitives and the rest kg natives |
| Functions | `(funcall F ARG ...)` `(apply F ARG ... LIST)` `(eval FORM)` `(function F)` written `#'F` `fboundp` `symbol-function` `symbol-value` `(fset 'NAME FN)` `(defalias 'NAME FN)` `fmakunbound` — core Fe forms, not prelude definitions |
| Numbers | `+` `-` `*` `/` and the comparators `(= N ...)` `<` `<=` `>` `>=` `/=` |
| Quoting | `quasiquote`, written `` ` `` with `,` and `,@`; `#'f` is `(function f)` |
| Editor | `(string-empty-p S)` and `(thing-at-point THING)` — the text of `(bounds-of-thing-at-point THING)`, or `nil` when there are no bounds |
| Small library | `identity` `prog2` `max` `min` `documentation` `number-to-string` `string-to-list` `setq-default` `setq-local` `kbd` |

Argument lists are strict: too few or too many arguments raise
`wrong-number-of-arguments`. `&optional` parameters bind `nil` when omitted,
and `&rest` collects remaining arguments into a fresh list. `length` also counts
the codepoints of a string. `equal`
is structural on lists and compares the leaves with `eql` — strings by
content, numbers only when they are the same type *and* the same value, so
`(equal 1 1.0)` is `nil`, and everything else by identity.

A name that has never been assigned is an error rather than `nil`, so a typo
says `void-variable NAME` instead of quietly being false. `(boundp 'NAME)` asks
whether a name has a value, `(makunbound 'NAME)` takes it away, and a variable
holding `nil` is bound — which is what `defvar` tests before initialising.
Values and functions live in separate namespaces, as in Emacs: `defun`,
`defalias` and `fset` write the function cell, `setq`/`defvar` the value cell,
so `(setq f 7)` then `(defun f () 9)` coexist — `(list f (f))` is `(7 9)` —
and calling a name whose function cell is empty is `void-function NAME` even
when the name has a value.

```lisp
(defun initialise (words)
  "Insert the initial of every non-empty word in WORDS."
  (dolist (word words)
    (unless (string-empty-p word)
      (insert (concat (substring word 0 1) ". ")))))

(initialise (list "alpha" "beta"))
```

Where it differs from Emacs Lisp, and these are worth knowing before the
first surprise:

- `=` is numeric equality, chained over any number of arguments as in
  Emacs — it used to be Fe's assignment operator, and any Lisp written
  against that older kg needs `setq` instead.
- `eq` is Emacs' `eq`: `(eq 3 3)` is `t` because integers compare by
  value, while two separately-read equal strings and two float objects
  are `nil`. Fe's own broad comparator remains available as `is`, but
  `is` is fe-native, not an Emacs form.
- Values and functions live in separate namespaces, as in Emacs: call position
  reads only the function cell, so a function held in a variable is called
  with `(funcall f ...)` and `#'f` is `(function f)`.
- Numbers are signed 64-bit integers or doubles — there are no bignums —
  and character literals such as `?a` read as their codepoint numbers.
  Integer arithmetic that overflows, and integer division by zero, raise
  an `arith-error` message instead of promoting or wrapping.
- Variables are lexical by default; `defvar` and `defconst` mark a symbol
  *special*, and a `let` over a marked name binds it dynamically — which
  makes the ordinary Emacs temporary-setting idiom work:
  `(defvar case-sensitive-search nil)` then
  `(let ((case-sensitive-search t)) (do-the-search))` is seen by
  `do-the-search`, as it is in Emacs. The binding swaps the symbol's
  global value and restores it on every way out, including an error, a
  `throw` and `C-g`. `(special-variable-p 'v)` answers whether `v` was
  marked. A `lambda` or `defun` **parameter** named after a special stays
  lexical, which is Emacs' own behaviour under `lexical-binding: t`.
  A one-argument `(defvar v)` is scoped to the file — strictly, the
  reader/evaluation unit — it appears in, as in Emacs: a later file's
  `let` over the same name is lexical again. The one residual is a
  `defun` written after such a `defvar` and *called* from another file,
  which Emacs keeps dynamic and kg answers lexically. What kg does
  *not* have is buffer-local variables or whole-file
  `lexical-binding: nil` semantics.
- The printer abbreviates `(quote X)` back to `'X`, as Emacs' does, so
  `M-:` and `%S` show `'x` and `(a 'b c)`. Backquote is the exception:
  kg's reader expands `` ` ``/`,`/`,@` to the ordinary symbols
  `quasiquote`/`unquote`/`unquote-splicing` where Emacs uses distinct
  symbols it also abbreviates, so an unevaluated backquote form prints
  the long way. That is recorded rather than fixed. Its named
  prerequisite -- symbol escapes, without which the prelude could not
  spell the macro it defines after renaming what the reader produces --
  is met since Phase 14; what remains is the rename itself and Emacs'
  context-sensitive comma abbreviation, which the printer has no place
  to track.
- Symbols are first class: `intern`, `intern-soft`, `symbol-name`,
  `make-symbol`, `gensym`, and `put`/`get`/`symbol-plist` for property
  lists. `intern-soft` is a probe -- it answers `nil` for a name nothing
  has interned and interns nothing while asking -- and `make-symbol`
  and `gensym` hand out uninterned symbols, which is how a macro gets a
  temporary nothing can capture by name. A symbol name may carry
  backslash escapes, as in Emacs: `a\ b` is the one symbol named `a b`,
  `\1` is the symbol named `1`, an escaped `\.` in a list is an ordinary
  element, and `##` is the symbol with the empty name. The printer emits
  those escapes wherever a name would otherwise read back as something
  else, so a symbol always reads back as itself. kg has one obarray, so
  `intern`'s optional OBARRAY argument is refused rather than ignored.

- `t`, `nil` and keyword symbols are protected constants: `setq`, `set`, a
  `let` or `let*` binding name, `fset` and `defalias` all refuse them with
  the `setting-constant` condition. Keywords are self-evaluating, so
  `:foo` is `:foo` and `(keywordp :foo)` is `t` (`:` alone is a keyword
  too). A lambda parameter may still shadow `t`, as in Emacs; unlike
  Emacs, one named `nil` or a keyword is refused rather than bound.
- `condition-case`, `catch`/`throw`, `signal`, and `error` exist; `quit`
  (from `C-g`) is catchable by name but not by `(error …)`, and budget
  exhaustion is catchable by nothing. Conditions use a static
  hierarchy: `wrong-type-argument`, `void-function`, `arith-error`,
  `setting-constant` and others are under `error`; `quit` is a separate
  branch.
  `unwind-protect` runs its cleanup forms on any non-local exit — a
  normal return, an error, a `throw`, `C-g`, or budget exhaustion — and
  a `condition-case` written *inside* a cleanup handles what that
  cleanup raises, leaving whatever was being unwound in flight. A
  cleanup error nothing in the cleanup handles still replaces that
  completion, which is Emacs' rule too — though *where* the replacement
  is delivered can diverge in two corner shapes (a handler in the
  abandoned body; a catch the exit had already left), recorded as
  `cleanup-raise-residuals` in the compat manifest.
  `save-excursion` and `with-current-buffer` are transparent to the
  enclosing evaluation, `throw` included: an error inside either body
  reaches a `condition-case` written around the form with its original
  condition, a `throw` reaches a `catch` written around the form with the
  value it threw, and the restore has already run by the time either
  does. A hook or process-callback boundary *contains* instead: an error
  there, and a `throw` naming a `catch` outside the callback, are
  reported and swallowed so one broken hook cannot take the editor's
  evaluation down — but a `C-g` or a budget exhaustion is never
  contained, and cancels the whole evaluation. Those callbacks, and a
  nested `command-execute`, are still kg's native-reentry wall for
  `throw`: it becomes `(no-catch TAG VALUE)` there, which an enclosing
  `condition-case` handles, and that is a recorded divergence from
  Emacs. `(eval FORM)`, by contrast, evaluates FORM in the caller's own
  run — a condition, a `throw` or a `C-g` out of it reaches the
  enclosing handler, and its environment is the global one, which is
  Emacs' answer for a nil LEXICAL argument.
  kg's own editor natives still signal a plain `error` whose message
  happens to read like a condition name, so a handler naming the
  specific symbol does not match one of them; classifying them is the
  follow-up this phase deferred. `ignore-errors` is a one-line macro
  over `condition-case`.
- A macro's function cell holds fe's own macro object, not Emacs'
  `(macro . FUNCTION)` cons — visible only through `(symbol-function 'a-macro)`.
- Lisp nesting (recursive calls, nested special forms, self-expanding
  macros) is bounded by the interpreter's frame stack, not by C or
  garbage-collector recursion — kg's default arena holds about 540
  levels of ordinary self-recursion (measured 544 at the Phase 12 fix
  cycle), so walk long lists with `while`, not recursion. A native re-entering the evaluator synchronously (as a hook
  function, a process filter or sentinel, and a nested
  `command-execute` do) has its own, much smaller bound (32 nested
  re-entries), since each level there is a real C stack frame.
  `save-excursion` and `with-current-buffer` were the example here until
  Phase 11 made them prelude macros over `unwind-protect`; they no
  longer re-enter, which is why a `throw` out of either now reaches a
  `catch` outside it.
- A structure that refers to itself prints as far as the cycle and then
  `#<cycle>`, rather than being printed forever.

The editor bridge uses the Emacs names throughout: `insert`, `message`,
`buffer-name`, `load`, `global-set-key` and `global-unset-key`. `message`
formats, so `(message "%s at %d" name (point))` reaches the status area with
its arguments substituted, and a literal per cent in a message has to be
written `%%`.

The init file can also toggle editor options by running named commands,
e.g. enabling electric bracket pairing (off by default):

```lisp
(command-execute 'electric-pair-mode)
```

`command-execute` runs a built-in or Lisp-defined command named by a quoted
symbol as in Emacs or equivalently by a string. Nested calls use the same
evaluator and inherit the active command's prefix; outside dispatch they use
an empty prefix.

Which commands those are, and which of them a read-only buffer refuses, is
one table in the editor — the same one M-x and every key binding consult, so
a command cannot be refused by one route and allowed by another. Commands
defined in Lisp count as commands that edit the buffer, so a read-only buffer
refuses them too; there is no way yet for a `defun` to say otherwise.

Packages define interactive commands the way Emacs does, with `defun` plus
`(interactive)`, and bind them by name:

```lisp
(defun insert-date ()
  "Insert today's date."
  (interactive)
  (insert "2026-07-04"))
(global-set-key "C-c d" "insert-date")
```

Only an `(interactive ...)` form immediately after an optional docstring is a
declaration. It is removed from the body and registers the closure under its
own symbol; a later form is ordinary code. `(commandp NAME)` answers whether a
name is a command — kg has no interactive-form reflection, so unlike Emacs it
says `nil` for an anonymous lambda carrying an interactive form.

The declaration supplies command arguments: `p` is the numeric prefix, `P` the
raw prefix, and `r` the sorted region bounds. `s` reads literal text; `n` and
`N` read numbers, with `N` using a supplied prefix; `f`/`F` read paths and
`b`/`B` read buffer names without visiting or selecting them. Instead of a
specification string the declaration may carry a single **form**, as in
`(interactive (list 1 2))`: it is evaluated in the command's own lexical
environment at invocation time, once, and must return a proper list of
arguments. Emacs' additional `interactive` MODES arguments are accepted and
ignored, which is a recorded divergence.

A number must be one decimal token — an optional sign, digits, an optional
fraction, an optional exponent — with nothing but ASCII whitespace around it.
That is fe's own reader grammar minus its `1e+INF`/`1e+NaN` spellings, so
`inf`, `nan`, `0x10`, `1e`, trailing junk and an empty answer all re-prompt,
and no Lisp is evaluated to decide. An integer past `int64` becomes a float,
as an integer literal does.

Prompts are literal: a `%` in prompt text is not a format directive, where
Emacs would pass it through `format` with the earlier arguments — a recorded
divergence. `C-g` is quit, and an answer too long for the prompt buffer is
refused rather than truncated (an error for the Lisp codes; for `C-x C-f` and
the other interactive path prompts, a dismissal with `Path too long` in the
echo area). Neither runs the command body. Prompting is available only from
key/M-x dispatch, not init, hooks, process callbacks, eval-expression, or an
active prompt.

A valid Emacs code kg has not implemented, and the deferred modifiers `*`, `@`
and `^`, report `unsupported interactive code`; a byte outside Emacs' set
reports `invalid interactive code`. Arguments are strict and capped at 16, with
no nil padding; the cap is a recorded divergence rather than a silent
truncation. The raw `current-prefix-arg` binding is temporary, and
`(prefix-numeric-value X)` converts its nil, integer, universal-list, or `-`
forms — a malformed form raises a real `wrong-type-argument` condition carrying
the value. The binding is made and unmade at the command boundary by kg's C rather
than by `let` over a `defvar`'d name, and `current-prefix-arg` is not
marked special, so a `let` over that name is an ordinary lexical binding:
the forms lexically inside the `let` read it, and a function *called*
from inside still reads the command-boundary value —
`(let ((current-prefix-arg 7)) (list current-prefix-arg (f)))` measures
`(7 nil)`. The registry is also
reachable as `(define-command NAME FUNCTION &optional SPEC DOCUMENTATION)`;
the spec is nil, a string, or a zero-argument function, and documentation is
nil or a string. `remove-command` undoes the registration.

### Asking the user something

A command does not have to say everything it needs in its `(interactive
...)` declaration. These seven forms read the minibuffer from anywhere in
a command's body:

```lisp
(defun insert-heading ()
  "Ask for a heading and insert it."
  (interactive)
  (let ((text (read-string "Heading: " nil nil "Untitled"))
        (level (read-number "Level: " 1)))
    (when (y-or-n-p (concat "Insert \"" text "\"? "))
      (insert (concat (make-string level ?#) " " text)))))
(global-set-key "C-c h" "insert-heading")
```

* `(read-string PROMPT &optional INITIAL-INPUT HISTORY DEFAULT-VALUE)`
* `(read-number PROMPT &optional DEFAULT HISTORY)` — the prompt shows
  `(default N)` and an empty answer takes it
* `(read-file-name PROMPT &optional DIR DEFAULT-FILENAME MUSTMATCH INITIAL PREDICATE)`
  and `(read-buffer PROMPT &optional DEFAULT REQUIRE-MATCH PREDICATE)` —
  the same file and buffer pickers `C-x C-f` and `C-x b` use
* `(y-or-n-p PROMPT)` — one key: `y` is yes, any other key is no
* `(yes-or-no-p PROMPT)` — a typed `yes` or `no`, re-asked until one of
  them arrives
* `(completing-read PROMPT COLLECTION &optional PREDICATE REQUIRE-MATCH INITIAL-INPUT HISTORY DEFAULT)`
  — kg's pick-list over a list of strings: typing filters, `Left`/`Right`
  cycle, `Tab` completes to the highlighted candidate, `Enter` takes it,
  and a name typed in full wins over a longer candidate that sorts first

They follow the interactive codes' rules, because they are the same
readers: prompting works only from key/`M-x` dispatch and not while
another prompt is up, prompts are literal, an over-long answer is refused
rather than truncated, and `C-g` is a quit a `condition-case` can catch:

```lisp
(defun ask-politely ()
  (interactive)
  (message "%s" (condition-case nil (read-string "Say: ") (quit "never mind"))))
```

Four things differ from Emacs, and are recorded as such: a `HISTORY`
argument is accepted and ignored (kg's minibuffer histories are not
values a symbol names); a non-nil `PREDICATE` is refused with an error
rather than silently dropped; `read-buffer`'s `DEFAULT` is ignored,
because kg's buffer picker supplies its own default for a blank answer;
and `y-or-n-p` answers no for any key that is not `y`, where Emacs
re-asks — which is what every `(y/n)` question in kg already does.
`completing-read`'s `COLLECTION` is a list of strings and nothing else,
at most 64 of them.

A worked `init.el` — select the word under the cursor, the way you would
write it in Emacs:

```lisp
(defun select-current-word ()
  "Select the word under point, or say there is none."
  (interactive)
  (let ((bounds (bounds-of-thing-at-point 'word)))
    (if bounds
        (progn (goto-char (car bounds))
               (set-mark (cdr bounds)))
      (message "No word found at point."))))
(global-set-key "C-c w" "select-current-word")
```

`C-c w` now selects the word under point, so `C-w` kills it, `M-w` copies it
and `C-x C-x` bounces between its ends. One detail differs from Emacs:
`set-mark` already activates the region, so there is no separate
`activate-mark` to call.

Lisp-defined commands appear in `M-x` completion and run under the same
step budget and error recovery as `eval-expression`; `remove-command`
and `global-unset-key` undo the registrations. Only `C-c <key>` sequences
are bindable — `C-c` is reserved for user bindings, so they can never
shadow built-in keys. A mode that defines its own `C-c` keys (git commit
and rebase buffers, `*compilation*`) shadows the user's binding of that
same sequence while it is current, and every other `C-c` key still
reaches the user's.