# Forecast audit

**Generated file.  Do not edit.**  `make forecast-audit` rewrites it;
`make forecast-check` (part of `make check`) fails when it and the tree
disagree.  The tool is `utils/forecast_audit.py`, the corpus is
`utils/forecast/` plus kg's shipped `lisp/*.el`, and
`utils/forecast/README.md` says what each kind of corpus file is for.

MISSING is the implementation order for the Lisp surface: a name the
corpus calls that fe's primitives, fe's maths natives, kg's
`native_bindings[]` and kg's own `lisp/*.el` between them do not define.
COVERED is the other half of the same partition, kept in the report so
that a name which *stops* being covered is a diff and not a silence.


## Corpus

| File | References | Definitions |
| --- | ---: | ---: |
| `lisp/auto-fill.el` | 47 | 5 |
| `lisp/grep-buffer.el` | 102 | 10 |
| `lisp/help-fns.el` | 160 | 10 |
| `lisp/pipeline-text.el` | 57 | 6 |
| `lisp/pipeline.el` | 51 | 13 |
| `lisp/prelude.el` | 1913 | 157 |
| `utils/forecast/target-init.el` | 75 | 7 |
| `utils/forecast/forecast-snippet.el` | 112 | 13 |
| `utils/forecast/forecast-wordcount.el` | 118 | 10 |
| **total** | **2635** | **231** |

## Implemented-name set

| Source | Names |
| --- | ---: |
| kg-native | 138 |
| kg-lisp | 201 |
| fe-primitive | 81 |
| fe-native | 14 |
| reader | 6 |
| corpus (defined by the corpus itself) | 231 |

## MISSING (4 names, 4 references)

| Refs | Name | Wanted by |
| ---: | --- | --- |
| 1 | `gethash` | forecast-wordcount.el |
| 1 | `make-hash-table` | forecast-wordcount.el |
| 1 | `maphash` | forecast-wordcount.el |
| 1 | `puthash` | forecast-wordcount.el |

## Watch item: hash tables, vectors, records

Measured demand for the three families the plan's Declined section keeps off the roadmap.  Reopening any of them needs its own phase; this table is the evidence either way.

| Family | References | Names seen |
| --- | ---: | --- |
| hash-tables | 4 | `gethash` x1, `make-hash-table` x1, `maphash` x1, `puthash` x1 |
| vectors | 9 | `vectorp` x4, `aref` x3, `vconcat` x2 |
| records | 0 | -- |

## COVERED (265 names, 2631 references)

| Refs | Name | Source |
| ---: | --- | --- |
| 184 | `if` | fe-primitive |
| 182 | `setq` | fe-primitive |
| 170 | `list` | fe-primitive |
| 156 | `defalias` | kg-lisp |
| 149 | `cdr` | fe-primitive |
| 149 | `lambda` | fe-primitive |
| 146 | `car` | fe-primitive |
| 83 | `cons` | fe-primitive |
| 71 | `defun` | kg-lisp |
| 64 | `while` | fe-primitive |
| 59 | `internal--let` | kg-lisp |
| 59 | `let` | kg-lisp |
| 56 | `and` | fe-primitive |
| 38 | `eq` | fe-primitive |
| 34 | `length` | kg-lisp |
| 34 | `macro` | fe-primitive |
| 31 | `<` | fe-primitive |
| 30 | `not` | fe-primitive |
| 26 | `=` | fe-primitive |
| 25 | `-` | fe-primitive |
| 25 | `null` | kg-lisp |
| 23 | `format` | kg-native |
| 22 | `defvar` | kg-lisp |
| 22 | `substring` | kg-native |
| 21 | `concat` | kg-native |
| 21 | `stringp` | kg-native |
| 20 | `interactive` | kg-lisp |
| 19 | `+` | fe-primitive |
| 18 | `progn` | kg-lisp |
| 17 | `or` | fe-primitive |
| 17 | `reverse` | kg-native |
| 14 | `funcall` | fe-primitive |
| 13 | `consp` | kg-native |
| 13 | `insert` | kg-native |
| 12 | `error` | fe-primitive |
| 11 | `mapcar` | kg-lisp |
| 11 | `message` | kg-native |
| 10 | `<=` | fe-primitive |
| 10 | `append` | kg-lisp |
| 10 | `point-max` | kg-native |
| 10 | `point-min` | kg-native |
| 10 | `signal` | fe-primitive |
| 10 | `string=` | kg-native |
| 10 | `symbol-function` | fe-primitive |
| 9 | `goto-char` | kg-native |
| 9 | `provide` | kg-native |
| 9 | `string-match` | kg-native |
| 8 | `downcase` | kg-native |
| 8 | `internal--seq-to-list` | kg-lisp |
| 8 | `regexp-quote` | kg-native |
| 7 | `dolist` | kg-lisp |
| 7 | `match-end` | kg-native |
| 7 | `push` | kg-lisp |
| 7 | `symbol-name` | fe-primitive |
| 7 | `when` | kg-lisp |
| 6 | `1+` | kg-lisp |
| 6 | `cond` | kg-lisp |
| 6 | `equal` | kg-lisp |
| 6 | `let*` | kg-lisp |
| 6 | `match-beginning` | kg-native |
| 6 | `match-string` | kg-lisp |
| 6 | `setcdr` | fe-primitive |
| 6 | `sort` | kg-lisp |
| 6 | `string-join` | kg-lisp |
| 5 | `*` | fe-primitive |
| 5 | `buffer-substring` | kg-native |
| 5 | `gensym` | fe-primitive |
| 5 | `internal--qq` | kg-lisp |
| 5 | `mapconcat` | kg-lisp |
| 5 | `nreverse` | kg-lisp |
| 5 | `point` | kg-native |
| 4 | `>` | fe-primitive |
| 4 | `apply` | fe-primitive |
| 4 | `atom` | fe-primitive |
| 4 | `commandp` | kg-native |
| 4 | `eval` | fe-primitive |
| 4 | `fboundp` | fe-primitive |
| 4 | `intern` | fe-primitive |
| 4 | `numberp` | kg-native |
| 4 | `pipeline-adder` | kg-lisp |
| 4 | `split-string` | kg-lisp |
| 4 | `string-length` | kg-native |
| 4 | `string-to-char` | kg-native |
| 4 | `vectorp` | fe-primitive |
| 3 | `alist-get` | kg-lisp |
| 3 | `aref` | kg-lisp |
| 3 | `assoc` | kg-lisp |
| 3 | `boundp` | fe-primitive |
| 3 | `bounds-of-thing-at-point` | kg-native |
| 3 | `char-to-string` | kg-native |
| 3 | `condition-case` | fe-primitive |
| 3 | `defmacro` | kg-lisp |
| 3 | `erase-buffer` | kg-native |
| 3 | `forecast-snippet--fields` | corpus |
| 3 | `forecast-snippet--template` | corpus |
| 3 | `forecast-wordcount--words` | corpus |
| 3 | `goto-line` | kg-native |
| 3 | `help-fns--show` | kg-lisp |
| 3 | `integerp` | fe-primitive |
| 3 | `make-string` | kg-native |
| 3 | `number-to-string` | kg-lisp |
| 3 | `pipeline-run` | kg-lisp |
| 3 | `replace-regexp-in-string` | kg-lisp |
| 3 | `save-excursion` | kg-lisp |
| 3 | `seq-filter` | kg-lisp |
| 3 | `string<` | fe-primitive |
| 3 | `switch-to-buffer` | kg-native |
| 3 | `symbolp` | kg-native |
| 3 | `with-current-buffer` | kg-lisp |
| 2 | `add-hook` | kg-native |
| 2 | `assq` | kg-lisp |
| 2 | `assq-delete-all` | kg-lisp |
| 2 | `auto-fill--column-at` | kg-lisp |
| 2 | `auto-fill--maybe-break` | kg-lisp |
| 2 | `beginning-of-line` | kg-native |
| 2 | `buffer-name` | kg-native |
| 2 | `char-after` | kg-native |
| 2 | `current-column` | kg-native |
| 2 | `documentation` | kg-lisp |
| 2 | `documentation-property` | kg-lisp |
| 2 | `expt` | fe-native |
| 2 | `floor` | fe-native |
| 2 | `forecast-snippet--default` | corpus |
| 2 | `forecast-snippet--indent-to-point` | corpus |
| 2 | `forecast-snippet--read-name` | corpus |
| 2 | `forecast-snippet--value` | corpus |
| 2 | `forecast-snippet-expand` | corpus |
| 2 | `forecast-wordcount--interesting-p` | corpus |
| 2 | `forecast-wordcount--tally-alist` | corpus |
| 2 | `get-buffer-create` | kg-native |
| 2 | `global-set-key` | kg-native |
| 2 | `grep-buffer--fold` | kg-lisp |
| 2 | `internal--append2` | kg-lisp |
| 2 | `internal--bind-name` | kg-native |
| 2 | `internal--bind-value` | kg-native |
| 2 | `internal--declare-p` | kg-lisp |
| 2 | `internal--docstring-p` | kg-lisp |
| 2 | `internal--load-loop` | kg-lisp |
| 2 | `internal--qq-dotted` | kg-lisp |
| 2 | `internal--read-form` | kg-native |
| 2 | `internal--replace-expand` | kg-lisp |
| 2 | `internal--require-resolve` | kg-native |
| 2 | `internal--setq-local-forms` | kg-lisp |
| 2 | `internal--string-designator` | kg-lisp |
| 2 | `internal--thread` | kg-lisp |
| 2 | `internal--trim-char-p` | kg-lisp |
| 2 | `internal--trim-reject` | kg-lisp |
| 2 | `kbd` | kg-lisp |
| 2 | `line-number-at-pos` | kg-native |
| 2 | `macroexpand-1` | fe-primitive |
| 2 | `max` | kg-lisp |
| 2 | `member` | kg-lisp |
| 2 | `min` | kg-lisp |
| 2 | `my-open-notes` | corpus |
| 2 | `nconc` | kg-native |
| 2 | `pipeline-text-steps` | kg-lisp |
| 2 | `put` | fe-primitive |
| 2 | `setcar` | fe-primitive |
| 2 | `string-empty-p` | kg-lisp |
| 2 | `string-prefix-p` | kg-lisp |
| 2 | `string-suffix-p` | kg-lisp |
| 2 | `string-to-list` | kg-lisp |
| 2 | `string-trim` | kg-lisp |
| 2 | `symbol-value` | fe-primitive |
| 2 | `unless` | kg-lisp |
| 2 | `unwind-protect` | fe-primitive |
| 2 | `vconcat` | fe-primitive |
| 1 | `/` | fe-primitive |
| 1 | `1-` | kg-lisp |
| 1 | `apropos--candidates` | kg-lisp |
| 1 | `apropos--describe` | kg-lisp |
| 1 | `apropos--unique-sorted` | kg-lisp |
| 1 | `auto-fill--break` | kg-lisp |
| 1 | `auto-fill--find-break` | kg-lisp |
| 1 | `buffer-local-value` | kg-native |
| 1 | `capitalize` | kg-native |
| 1 | `catch` | fe-primitive |
| 1 | `compare-strings` | kg-native |
| 1 | `completing-read` | kg-native |
| 1 | `copy-sequence` | kg-lisp |
| 1 | `current-buffer` | kg-native |
| 1 | `default-value` | kg-native |
| 1 | `defconst` | kg-lisp |
| 1 | `define-key` | kg-native |
| 1 | `elt` | kg-lisp |
| 1 | `end-of-line` | kg-native |
| 1 | `env` | fe-primitive |
| 1 | `eql` | fe-primitive |
| 1 | `forecast-snippet--placeholder-re` | corpus |
| 1 | `forecast-snippet-define` | corpus |
| 1 | `forecast-snippet-names` | corpus |
| 1 | `forecast-wordcount--bar` | corpus |
| 1 | `forecast-wordcount--pairs` | corpus |
| 1 | `forecast-wordcount--row` | corpus |
| 1 | `forecast-wordcount--tally` | corpus |
| 1 | `forward-char` | kg-native |
| 1 | `fset` | fe-primitive |
| 1 | `get` | fe-primitive |
| 1 | `grep-buffer` | kg-lisp |
| 1 | `grep-buffer--highlight` | kg-lisp |
| 1 | `grep-buffer--hits` | kg-lisp |
| 1 | `grep-buffer--lines` | kg-lisp |
| 1 | `grep-buffer--report` | kg-lisp |
| 1 | `grep-buffer--row` | kg-lisp |
| 1 | `grep-buffer-literal` | kg-lisp |
| 1 | `help-fns--function-kind` | kg-lisp |
| 1 | `help-fns--interactive-line` | kg-lisp |
| 1 | `help-fns--macro-p` | kg-lisp |
| 1 | `identity` | kg-lisp |
| 1 | `interactive-form` | kg-native |
| 1 | `internal--cl-case-test` | kg-lisp |
| 1 | `internal--command-documentation` | kg-native |
| 1 | `internal--command-names` | kg-native |
| 1 | `internal--custom-presentation-keyword-p` | kg-lisp |
| 1 | `internal--custom-semantics-keyword-p` | kg-lisp |
| 1 | `internal--equal-vectors` | kg-lisp |
| 1 | `internal--fe-aref` | kg-lisp |
| 1 | `internal--fe-elt` | kg-lisp |
| 1 | `internal--fe-length` | kg-lisp |
| 1 | `internal--has-interactive` | kg-lisp |
| 1 | `internal--interactive-p` | kg-lisp |
| 1 | `internal--load-begin` | kg-native |
| 1 | `internal--load-end` | kg-native |
| 1 | `internal--merge` | kg-lisp |
| 1 | `internal--merge-pairs` | kg-lisp |
| 1 | `internal--qq-list` | kg-lisp |
| 1 | `internal--require-check` | kg-native |
| 1 | `internal--require-pop` | kg-native |
| 1 | `internal--require-push` | kg-native |
| 1 | `internal--resolve-load` | kg-native |
| 1 | `keywordp` | fe-primitive |
| 1 | `listp` | kg-native |
| 1 | `local-variable-p` | kg-native |
| 1 | `macroexpand` | fe-primitive |
| 1 | `memq` | kg-lisp |
| 1 | `my-announce-save` | corpus |
| 1 | `my-boring-name-p` | corpus |
| 1 | `my-insert-rule` | corpus |
| 1 | `my-title-case` | corpus |
| 1 | `nthcdr` | kg-lisp |
| 1 | `pipeline-explain` | kg-lisp |
| 1 | `pipeline-explain-step` | kg-lisp |
| 1 | `pipeline-run-safely` | kg-lisp |
| 1 | `pipeline-run-until` | kg-lisp |
| 1 | `pipeline-scaler` | kg-lisp |
| 1 | `plist-get` | kg-lisp |
| 1 | `re-search-forward` | kg-native |
| 1 | `read-string` | kg-native |
| 1 | `remove-hook` | kg-native |
| 1 | `replace-region` | kg-native |
| 1 | `require` | kg-lisp |
| 1 | `save-match-data` | kg-lisp |
| 1 | `seq-find` | kg-lisp |
| 1 | `seq-remove` | kg-lisp |
| 1 | `seq-take` | kg-lisp |
| 1 | `set` | fe-primitive |
| 1 | `special-variable-p` | fe-primitive |
| 1 | `string-to-number` | kg-native |
| 1 | `string-trim-left` | kg-lisp |
| 1 | `string-trim-right` | kg-lisp |
| 1 | `thing-at-point` | kg-lisp |
| 1 | `throw` | fe-primitive |
| 1 | `truncate` | fe-native |
| 1 | `y-or-n-p` | kg-native |
| 1 | `zerop` | kg-lisp |
