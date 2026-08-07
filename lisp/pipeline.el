;;; pipeline.el --- Proof 3's pure half: higher-order steps over a value.  -*- lexical-binding: t; -*-
;;;
;;; The parent plan's §14 asks for "a small higher-order package" whose
;;; pure-language portions also run unchanged under Emacs 31.  This file
;;; is that portion, and "unchanged" is measured rather than claimed:
;;; test/lisp-compat/ carries `comparison: emacs' cases whose setup loads
;;; THIS FILE on both sides and whose snapshots are the pinned Emacs'
;;; own answers (see the pipeline-* case ids).  Nothing here touches a
;;; buffer, a window, a key or a process; `pipeline-text.el' is the half
;;; that does, and it (require ...)s this one.
;;;
;;; A pipeline is an ordinary list of one-argument functions, applied
;;; left to right to a starting value.  That is enough to exercise every
;;; bullet §14 lists -- Lisp-2 name separation, funcall/apply, closures,
;;; macro expansion including reflective use, catch/throw,
;;; condition-case, provide/require and more than one file -- without
;;; inventing machinery to hang them on.
;;;
;;; Three measured constraints shaped it, and are worth knowing before
;;; editing it:
;;;
;;;   * a `throw' cannot cross a native re-entry boundary in kg (the
;;;     recorded catch-throw-reachability divergence), so every catch
;;;     and its throws live inside this file, with no `save-excursion'
;;;     or `with-current-buffer' in between.  `pipeline-text.el' never
;;;     throws across one either.
;;;   * `load' resolves a bare name only in the config directory, so the
;;;     two files reach each other with `require', which searches
;;;     `load-path'.
;;;   * Emacs' `load' of a file with no `lexical-binding' cookie binds
;;;     dynamically, and the closures below then capture nothing --
;;;     measured, as `void-variable (n)', before the cookie on line 1 was
;;;     added.  kg has only lexical binding and reads the cookie as the
;;;     comment it is, so the line costs kg nothing and is what makes
;;;     "runs unchanged under Emacs 31" true rather than nearly true.
;;;   * two of kg's recorded silent divergences must not be load-bearing
;;;     here, or "runs unchanged under Emacs" would be a coincidence:
;;;     nothing in this file rebinds a `defvar'd variable around a call,
;;;     and no returned value contains a `(quote X)' form.  Both are why
;;;     the macros below expand to plain `if'/`+' forms and why the
;;;     reflective helpers are handed their form as data built with
;;;     `list'.  BOTH DIVERGENCES ARE CLOSED as of Phase 11 -- `defvar'
;;;     marks, and the writer abbreviates -- so the constraint no longer
;;;     binds; this file is left as written because what it took to make
;;;     the agreement a property rather than a coincidence is the record
;;;     worth keeping.

(provide 'pipeline)

;; --- Lisp-2 name separation -----------------------------------------
;; One symbol, two independent cells.  The value cell says where the
;; pipeline came from; the function cell computes something.  Reading
;; `pipeline-origin' never reaches the function, and calling
;; `(pipeline-origin)' never reaches the value.
(setq pipeline-origin "value-cell")

(defun pipeline-origin ()
  "function-cell")

;; --- closures --------------------------------------------------------
;; Each of these returns a function that has captured its argument.  The
;; capture is lexical in both dialects, which is exactly why the package
;; may rely on it.
(defun pipeline-adder (n)
  (lambda (x) (+ x n)))

(defun pipeline-scaler (n)
  (lambda (x) (* x n)))

;; --- funcall / apply -------------------------------------------------
;; `pipeline-run' is the fold: funcall each step on the accumulated
;; value.  `pipeline-call' is `apply' over a ready-made argument list,
;; which is the other half of §14's bullet.
(defun pipeline-run (steps value)
  (let ((rest steps) (acc value))
    (while rest
      (setq acc (funcall (car rest) acc))
      (setq rest (cdr rest)))
    acc))

(defun pipeline-call (fn args)
  (apply fn args))

;; --- catch / throw ---------------------------------------------------
;; Early exit: stop at the first step whose result passes LIMIT and hand
;; that result straight out of the loop.  The catch and the throw are in
;; one function on purpose -- see the header's first constraint.
(defun pipeline-run-until (steps value limit)
  (catch 'pipeline-limit
    (let ((rest steps) (acc value))
      (while rest
        (setq acc (funcall (car rest) acc))
        (if (< limit acc)
            (throw 'pipeline-limit acc))
        (setq rest (cdr rest)))
      acc)))

;; --- condition-case --------------------------------------------------
;; A step that signals stops the pipeline and is reported as data rather
;; than escaping.  The report is (failed CONDITION), a list of two
;; symbols, so it prints identically in both dialects.
(defun pipeline-run-safely (steps value)
  (condition-case err
      (pipeline-run steps value)
    (error (list 'failed (car err)))))

;; --- macros, and reflection on them ----------------------------------
;; These expand to plain arithmetic and `if' forms: no implicit body
;; (kg's is `do' where Emacs' is `progn') and no quoted data, so an
;; expansion is comparable between the two dialects character for
;; character.  The quoted-data half stopped mattering in Phase 11, when
;; kg's writer learnt Emacs' `(quote x)' -> 'x abbreviation; the
;; implicit-body half still does.
;;
;; `pipeline-boost' expands to a form whose own head is a macro call.
;; That is deliberate: it is what makes one expansion step and the
;; expansion fixpoint different answers, which is the whole difference
;; between `macroexpand-1' and `macroexpand'.
(defmacro pipeline-twice (form)
  (list '+ form form))

(defmacro pipeline-boost (form)
  (list 'pipeline-twice form))

(defmacro pipeline-guard (n form)
  (list 'if (list '< 0 n) form 0))

;; The reflective half §14's "macro expansion" bullet means (10A
;; Decision 2): a package that can tell you what its own macros expand
;; to.  FORM is data, not syntax, so callers build it with `list'.
;; `pipeline-explain-step' takes exactly one expansion step and
;; `pipeline-explain' runs to the fixpoint, which is the difference
;; between `macroexpand-1' and `macroexpand' and the reason a debugging
;; helper wants both.
(defun pipeline-explain-step (form)
  (macroexpand-1 form))

(defun pipeline-explain (form)
  (macroexpand form))

;; Whether FORM's head names one of this package's macros, answered by
;; asking the expander rather than by keeping a list: a form that is not
;; a macro call comes back from `macroexpand-1' unchanged.
(defun pipeline-macro-call-p (form)
  (not (equal form (macroexpand-1 form))))
