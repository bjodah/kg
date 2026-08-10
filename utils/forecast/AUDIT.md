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
| `lisp/auto-fill.el` | 48 | 5 |
| `lisp/pipeline-text.el` | 57 | 6 |
| `lisp/pipeline.el` | 51 | 13 |
| `lisp/prelude.el` | 1568 | 122 |
| `utils/forecast/target-init.el` | 75 | 7 |
| `utils/forecast/forecast-grep.el` | 91 | 11 |
| `utils/forecast/forecast-snippet.el` | 112 | 13 |
| `utils/forecast/forecast-wordcount.el` | 118 | 10 |
| **total** | **2120** | **187** |

## Implemented-name set

| Source | Names |
| --- | ---: |
| kg-native | 101 |
| kg-lisp | 146 |
| fe-primitive | 70 |
| fe-native | 14 |
| reader | 6 |
| corpus (defined by the corpus itself) | 187 |

## MISSING (6 names, 7 references)

| Refs | Name | Wanted by |
| ---: | --- | --- |
| 2 | `erase-buffer` | forecast-grep.el, forecast-wordcount.el |
| 1 | `beginning-of-line` | forecast-grep.el |
| 1 | `gethash` | forecast-wordcount.el |
| 1 | `make-hash-table` | forecast-wordcount.el |
| 1 | `maphash` | forecast-wordcount.el |
| 1 | `puthash` | forecast-wordcount.el |

## Watch item: hash tables, vectors, records

Measured demand for the three families the plan's Declined section keeps off the roadmap.  Reopening any of them needs its own phase; this table is the evidence either way.

| Family | References | Names seen |
| --- | ---: | --- |
| hash-tables | 4 | `gethash` x1, `make-hash-table` x1, `maphash` x1, `puthash` x1 |
| vectors | 0 | -- |
| records | 0 | -- |

## COVERED (218 names, 2113 references)

| Refs | Name | Source |
| ---: | --- | --- |
| 171 | `setq` | fe-primitive |
| 144 | `cdr` | fe-primitive |
| 134 | `car` | fe-primitive |
| 129 | `if` | fe-primitive |
| 122 | `defalias` | fe-primitive |
| 112 | `list` | fe-primitive |
| 109 | `lambda` | fe-primitive |
| 69 | `cons` | fe-primitive |
| 63 | `let` | kg-lisp |
| 62 | `defun` | kg-lisp |
| 60 | `while` | fe-primitive |
| 49 | `internal--let` | kg-lisp |
| 48 | `and` | fe-primitive |
| 32 | `eq` | fe-primitive |
| 28 | `not` | fe-primitive |
| 26 | `<` | fe-primitive |
| 22 | `macro` | fe-primitive |
| 21 | `-` | fe-primitive |
| 21 | `=` | fe-primitive |
| 20 | `length` | kg-lisp |
| 19 | `+` | fe-primitive |
| 19 | `null` | kg-lisp |
| 17 | `defvar` | kg-lisp |
| 17 | `interactive` | kg-lisp |
| 17 | `substring` | kg-native |
| 16 | `concat` | kg-native |
| 16 | `consp` | kg-native |
| 14 | `progn` | kg-lisp |
| 14 | `reverse` | kg-lisp |
| 13 | `funcall` | fe-primitive |
| 13 | `or` | fe-primitive |
| 12 | `format` | kg-native |
| 12 | `insert` | kg-native |
| 12 | `stringp` | kg-native |
| 11 | `when` | kg-lisp |
| 10 | `mapcar` | kg-lisp |
| 10 | `string=` | kg-native |
| 9 | `<=` | fe-primitive |
| 9 | `error` | fe-primitive |
| 9 | `signal` | fe-primitive |
| 8 | `downcase` | kg-native |
| 8 | `message` | kg-native |
| 8 | `point-min` | kg-native |
| 7 | `point-max` | kg-native |
| 7 | `setcdr` | fe-primitive |
| 6 | `provide` | kg-native |
| 6 | `regexp-quote` | kg-native |
| 5 | `*` | fe-primitive |
| 5 | `1+` | kg-lisp |
| 5 | `dolist` | kg-lisp |
| 5 | `equal` | kg-lisp |
| 5 | `goto-char` | kg-native |
| 5 | `internal--qq` | kg-lisp |
| 5 | `match-end` | kg-native |
| 5 | `match-string` | kg-lisp |
| 5 | `push` | kg-lisp |
| 5 | `string-match` | kg-native |
| 5 | `symbol-function` | fe-primitive |
| 5 | `symbol-name` | fe-primitive |
| 5 | `with-current-buffer` | kg-lisp |
| 4 | `apply` | fe-primitive |
| 4 | `atom` | fe-primitive |
| 4 | `buffer-substring` | kg-native |
| 4 | `cond` | kg-lisp |
| 4 | `let*` | kg-lisp |
| 4 | `match-beginning` | kg-native |
| 4 | `nreverse` | kg-lisp |
| 4 | `numberp` | kg-native |
| 4 | `pipeline-adder` | kg-lisp |
| 4 | `sort` | kg-lisp |
| 4 | `split-string` | kg-lisp |
| 4 | `string-join` | kg-lisp |
| 4 | `string-length` | kg-native |
| 4 | `string-to-char` | kg-native |
| 3 | `>` | fe-primitive |
| 3 | `alist-get` | kg-lisp |
| 3 | `append` | kg-lisp |
| 3 | `assoc` | kg-lisp |
| 3 | `bounds-of-thing-at-point` | kg-native |
| 3 | `buffer-name` | kg-native |
| 3 | `defmacro` | kg-lisp |
| 3 | `forecast-snippet--fields` | corpus |
| 3 | `forecast-snippet--template` | corpus |
| 3 | `forecast-wordcount--words` | corpus |
| 3 | `get-buffer-create` | kg-native |
| 3 | `goto-line` | kg-native |
| 3 | `integerp` | fe-primitive |
| 3 | `mapconcat` | kg-lisp |
| 3 | `number-to-string` | kg-lisp |
| 3 | `pipeline-run` | kg-lisp |
| 3 | `seq-filter` | kg-lisp |
| 3 | `string-to-list` | kg-lisp |
| 3 | `symbolp` | kg-native |
| 2 | `add-hook` | kg-native |
| 2 | `assq` | kg-lisp |
| 2 | `assq-delete-all` | kg-lisp |
| 2 | `auto-fill--column-at` | kg-lisp |
| 2 | `auto-fill--maybe-break` | kg-lisp |
| 2 | `char-after` | kg-native |
| 2 | `char-to-string` | kg-native |
| 2 | `condition-case` | fe-primitive |
| 2 | `current-column` | kg-native |
| 2 | `expt` | fe-native |
| 2 | `floor` | fe-native |
| 2 | `forecast-grep--fold` | corpus |
| 2 | `forecast-snippet--default` | corpus |
| 2 | `forecast-snippet--indent-to-point` | corpus |
| 2 | `forecast-snippet--read-name` | corpus |
| 2 | `forecast-snippet--value` | corpus |
| 2 | `forecast-snippet-expand` | corpus |
| 2 | `forecast-wordcount--interesting-p` | corpus |
| 2 | `forecast-wordcount--tally-alist` | corpus |
| 2 | `gensym` | fe-primitive |
| 2 | `global-set-key` | kg-native |
| 2 | `intern` | fe-primitive |
| 2 | `internal--append2` | kg-lisp |
| 2 | `internal--bind-name` | kg-lisp |
| 2 | `internal--bind-value` | kg-lisp |
| 2 | `internal--declare-p` | kg-lisp |
| 2 | `internal--docstring-p` | kg-lisp |
| 2 | `internal--load-loop` | kg-lisp |
| 2 | `internal--qq-dotted` | kg-lisp |
| 2 | `internal--read-form` | kg-native |
| 2 | `internal--trim-char-p` | kg-lisp |
| 2 | `internal--trim-reject` | kg-lisp |
| 2 | `kbd` | kg-lisp |
| 2 | `keywordp` | fe-primitive |
| 2 | `line-number-at-pos` | kg-native |
| 2 | `listp` | kg-lisp |
| 2 | `macroexpand-1` | fe-primitive |
| 2 | `make-string` | kg-native |
| 2 | `member` | kg-lisp |
| 2 | `my-open-notes` | corpus |
| 2 | `pipeline-text-steps` | kg-lisp |
| 2 | `replace-regexp-in-string` | kg-lisp |
| 2 | `save-excursion` | kg-lisp |
| 2 | `setcar` | fe-primitive |
| 2 | `string-empty-p` | kg-lisp |
| 2 | `string<` | kg-lisp |
| 2 | `thing-at-point` | kg-lisp |
| 2 | `unless` | kg-lisp |
| 2 | `unwind-protect` | fe-primitive |
| 1 | `/` | fe-primitive |
| 1 | `1-` | kg-lisp |
| 1 | `auto-fill--break` | kg-lisp |
| 1 | `auto-fill--find-break` | kg-lisp |
| 1 | `capitalize` | kg-native |
| 1 | `catch` | fe-primitive |
| 1 | `completing-read` | kg-native |
| 1 | `define-key` | kg-native |
| 1 | `eql` | fe-primitive |
| 1 | `eval` | fe-primitive |
| 1 | `forecast-grep` | corpus |
| 1 | `forecast-grep--format-hit` | corpus |
| 1 | `forecast-grep--highlight` | corpus |
| 1 | `forecast-grep--hit-p` | corpus |
| 1 | `forecast-grep--hits` | corpus |
| 1 | `forecast-grep--lines` | corpus |
| 1 | `forecast-grep--report` | corpus |
| 1 | `forecast-grep-literal` | corpus |
| 1 | `forecast-snippet--placeholder-re` | corpus |
| 1 | `forecast-snippet-define` | corpus |
| 1 | `forecast-snippet-names` | corpus |
| 1 | `forecast-wordcount--bar` | corpus |
| 1 | `forecast-wordcount--pairs` | corpus |
| 1 | `forecast-wordcount--row` | corpus |
| 1 | `forecast-wordcount--tally` | corpus |
| 1 | `get-buffer` | kg-native |
| 1 | `identity` | kg-lisp |
| 1 | `internal--custom-presentation-keyword-p` | kg-lisp |
| 1 | `internal--custom-semantics-keyword-p` | kg-lisp |
| 1 | `internal--has-interactive` | kg-lisp |
| 1 | `internal--interactive-p` | kg-lisp |
| 1 | `internal--load-begin` | kg-native |
| 1 | `internal--load-end` | kg-native |
| 1 | `internal--merge` | kg-lisp |
| 1 | `internal--merge-pairs` | kg-lisp |
| 1 | `internal--qq-list` | kg-lisp |
| 1 | `internal--replace-expand` | kg-lisp |
| 1 | `internal--require-check` | kg-native |
| 1 | `internal--require-pop` | kg-native |
| 1 | `internal--require-push` | kg-native |
| 1 | `internal--require-resolve` | kg-native |
| 1 | `internal--resolve-load` | kg-native |
| 1 | `macroexpand` | fe-primitive |
| 1 | `max` | kg-lisp |
| 1 | `memq` | kg-lisp |
| 1 | `min` | kg-lisp |
| 1 | `my-announce-save` | corpus |
| 1 | `my-boring-name-p` | corpus |
| 1 | `my-insert-rule` | corpus |
| 1 | `my-title-case` | corpus |
| 1 | `nconc` | kg-lisp |
| 1 | `nth` | kg-lisp |
| 1 | `nthcdr` | kg-lisp |
| 1 | `pipeline-explain` | kg-lisp |
| 1 | `pipeline-explain-step` | kg-lisp |
| 1 | `pipeline-run-safely` | kg-lisp |
| 1 | `pipeline-run-until` | kg-lisp |
| 1 | `pipeline-scaler` | kg-lisp |
| 1 | `plist-get` | kg-lisp |
| 1 | `read-string` | kg-native |
| 1 | `remove-hook` | kg-native |
| 1 | `replace-region` | kg-native |
| 1 | `require` | kg-lisp |
| 1 | `seq-remove` | kg-lisp |
| 1 | `seq-take` | kg-lisp |
| 1 | `set` | fe-primitive |
| 1 | `string-suffix-p` | kg-lisp |
| 1 | `string-to-number` | kg-native |
| 1 | `string-trim` | kg-lisp |
| 1 | `string-trim-left` | kg-lisp |
| 1 | `string-trim-right` | kg-lisp |
| 1 | `symbol-value` | fe-primitive |
| 1 | `throw` | fe-primitive |
| 1 | `truncate` | fe-native |
| 1 | `y-or-n-p` | kg-native |
| 1 | `zerop` | kg-lisp |
