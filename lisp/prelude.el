;;; prelude.el --- kg's Lisp prelude: the Emacs Lisp surface built on Fe.
;;;
;;; This is the canonical source.  It is NOT a runtime dependency: kg
;;; embeds a byte-for-byte copy of this file (src/lisp_prelude_generated.inc,
;;; produced by utils/embed_lisp.py) and evaluates that at startup, so a kg
;;; binary built and run with no lisp/ directory installed anywhere behaves
;;; identically to one built next to this file.  Do not "fix" packaging by
;;; installing this file -- it changes nothing at runtime.
;;;
;;; Regenerate the embedded copy after editing this file with
;;; `make lisp-prelude-generate`.  `make lisp-prelude-check` (part of
;;; `make check`) fails if the checked-in copy and this file disagree.
;;;
;;; Forms kg provides that upstream fe does not: the Emacs Lisp surface,
;;; written in Fe and evaluated at startup so it is available before any
;;; init file runs.
;;;
;;; Two rules hold everywhere below.
;;;
;;; 1. Ordering is load-bearing.  An alias of a primitive must be taken
;;;    before anything shadows that name (only `let' is shadowed), and a
;;;    macro must not expand into a name that shadows what it meant.
;;;    test_prelude_source_file (test/test_lisp.c) pins this: it asserts
;;;    `internal--let' is still a primitive rather than the macro that
;;;    later shadows the name it aliased.
;;; 2. Nothing here recurses over a list spine.  Fe's GC stack caps
;;;    recursion at a few hundred frames, so list walks are `while' loops.
;;;    The one deliberate exception is `equal', which is iterative on the
;;;    spine and recursive only on the car.
;;;
;;; A third rule used to sit between them -- "no macro may expand to bare
;;; nil", because Fe spliced the expansion over the caller's cons cell and
;;; compared nil by address, so the copy was a nil-shaped truthy object.
;;; Fe no longer copies the expansion (see the FeTMacro arm of Evaluate in
;;; fe/fe.c, and doc/fe-upstream.md, which lists the fix as shipped), so
;;; the rule is gone, and so are the four `(list 'quote nil)' workarounds
;;; it licensed.  The first three -- in `cond', `defvar' and `interactive'
;;; -- were replaced with bare `nil' on 2026-08-04 (sub-plan 02E), proven
;;; rather than assumed: `make check' at its full discovered count,
;;; `test_prelude_source_file''s ordering pin, `make lisp-prelude-check',
;;; and the named behaviours these three forms guard (`(cond)' is nil,
;;; `defvar' on an already-bound variable does not reassign, a top-level
;;; `(interactive)' is inert and reads as nil to a surrounding `progn')
;;; all held with the replacement in place.  The fourth, in this file's
;;; own `setq' macro, was already gone: the Phase 2 dialect cutover
;;; deleted that macro outright now that core `setq' is a real Fe special
;;; form.
;;;
;;; Macros expand on *every* invocation, and each expansion is charged
;;; against the evaluation step budget.  That is a performance property,
;;; not a correctness one; an earlier version of this comment claimed
;;; expansion happened once per call site, which was never true of Fe.
;;;
;;; The prelude bootstraps itself with the primitives it is about to wrap:
;;; definitions use Fe's core `defalias' primitive, not the `defun' macro
;;; defined further down, so nothing here depends on an expansion
;;; happening before `defun' itself exists.

;; Emacs spellings for Fe primitives.  `internal--let' keeps Fe's
;; one-binding `let' reachable after the Emacs `let' shadows it; the
;; function bodies below use it.  `defalias' stores the captured
;; primitive itself -- not a symbol designator -- so the later
;; redefinition of `let' cannot reach it through the cell it aliased.
(defalias 'internal--let (symbol-function 'let))
(defalias 'progn (symbol-function 'do))
(defalias 'null (symbol-function 'not))
(defalias '1+ (lambda (n) (+ n 1)))
(defalias '1- (lambda (n) (- n 1)))
(defalias 'caar (lambda (x) (car (car x))))
(defalias 'cadr (lambda (x) (car (cdr x))))
(defalias 'cddr (lambda (x) (cdr (cdr x))))
(defalias 'listp (lambda (x) (if (null x) t (consp x))))
;; --- list library, all iterative ---
(defalias 'reverse (lambda (lst)
  (internal--let res nil)
  (while lst
    (setq res (cons (car lst) res))
    (setq lst (cdr lst)))
  res))
(defalias 'internal--append2 (lambda (a b)
  (internal--let res b)
  (internal--let r (reverse a))
  (while r
    (setq res (cons (car r) res))
    (setq r (cdr r)))
  res))
(defalias 'append (lambda lists
  (internal--let r (reverse lists))
  (internal--let res (car r))
  (setq r (cdr r))
  (while r
    (setq res (internal--append2 (car r) res))
    (setq r (cdr r)))
  res))
(defalias 'length (lambda (x)
  (if (stringp x)
      (string-length x)
    (internal--let n 0)
    (while x
      (setq n (+ n 1))
      (setq x (cdr x)))
    n)))
(defalias 'nthcdr (lambda (n lst)
  (while (and (< 0 n) lst)
    (setq n (- n 1))
    (setq lst (cdr lst)))
  lst))
(defalias 'nth (lambda (n lst) (car (nthcdr n lst))))
(defalias 'last (lambda (lst)
  (while (cdr lst) (setq lst (cdr lst)))
  lst))
;; Structural on lists, then Emacs' atom rule: strings compare by
;; content, numbers by eql, everything else by identity.  Only the car
;; descends, so the spine cost is a loop, not a frame.
(defalias 'equal (lambda (a b)
  (internal--let same t)
  (while (and same (consp a) (consp b))
    (if (equal (car a) (car b)) nil (setq same nil))
    (setq a (cdr a))
    (setq b (cdr b)))
  (and same (not (consp a)) (not (consp b))
    (if (and (stringp a) (stringp b))
        (string= a b)
      (if (and (numberp a) (numberp b))
          (eql a b)
        (eq a b))))))
(defalias 'zerop (lambda (n) (= n 0)))
(defalias 'mapcar (lambda (f lst)
  (internal--let res nil)
  (while lst
    (setq res (cons (funcall f (car lst)) res))
    (setq lst (cdr lst)))
  (reverse res)))
(defalias 'member (lambda (elt lst)
  (while (and lst (not (equal elt (car lst))))
    (setq lst (cdr lst)))
  lst))
(defalias 'memq (lambda (elt lst)
  (while (and lst (not (eq elt (car lst))))
    (setq lst (cdr lst)))
  lst))
(defalias 'assoc (lambda (key alist)
  (internal--let hit nil)
  (while (and alist (null hit))
    (if (equal key (car (car alist))) (setq hit (car alist)))
    (setq alist (cdr alist)))
  hit))
;; --- control macros ---
(defalias 'cond (macro clauses
  (if clauses
      (list 'if (car (car clauses))
        (cons 'progn (cdr (car clauses)))
        (cons 'cond (cdr clauses)))
    nil)))
(defalias 'when (macro (test . body)
  (list 'if test (cons 'progn body))))
(defalias 'unless (macro (test . body)
  (cons 'if (cons test (cons nil body)))))
(defalias 'internal--first (lambda args (car args)))
(defalias 'prog1 (macro (first . body)
  (cons 'internal--first (cons first body))))
;; --- binding forms ---
(defalias 'internal--bind-name (lambda (b) (if (atom b) b (car b))))
(defalias 'internal--bind-value (lambda (b)
  (if (atom b) nil (car (cdr b)))))
;; Parallel, via immediate application: the value forms are evaluated
;; as arguments, in the environment outside the new bindings.  One
;; `while' walk calls both helpers in head position, as `let*' does,
;; and builds the two lists reversed for `reverse' to put back.  The
;; obvious spelling -- two `mapcar' passes over #' designators -- pays
;; a funcall and a designator resolution per binding per expansion, and
;; a macro expands on every call: 200k expansions of a two-binding
;; `let', run against the fe interpreter itself, measured 2.92 s that
;; way against 1.92 s for this loop (median of five each).
(defalias 'let (macro (bindings . body)
  (internal--let names nil)
  (internal--let values nil)
  (while bindings
    (setq names (cons (internal--bind-name (car bindings)) names))
    (setq values (cons (internal--bind-value (car bindings)) values))
    (setq bindings (cdr bindings)))
  (cons (cons 'lambda (cons (reverse names) body))
    (reverse values))))
(defalias 'let* (macro (bindings . body)
  (internal--let forms nil)
  (while bindings
    (setq forms (cons (list 'internal--let
                        (internal--bind-name (car bindings))
                        (internal--bind-value (car bindings)))
                  forms))
    (setq bindings (cdr bindings)))
  (cons 'progn (internal--append2 (reverse forms) body))))
;; --- iteration macros ---
(defalias 'internal--dolist (lambda (items body)
  (while items
    (funcall body (car items))
    (setq items (cdr items)))))
(defalias 'dolist (macro (spec . body)
  (list 'progn
    (list 'internal--dolist (car (cdr spec))
      (cons 'lambda (cons (list (car spec)) body)))
    (car (cdr (cdr spec))))))
(defalias 'internal--dotimes (lambda (count body)
  (internal--let i 0)
  (while (< i count)
    (funcall body i)
    (setq i (+ i 1)))))
(defalias 'dotimes (macro (spec . body)
  (list 'progn
    (list 'internal--dotimes (car (cdr spec))
      (cons 'lambda (cons (list (car spec)) body)))
    (car (cdr (cdr spec))))))
(defalias 'push (macro (item place)
  (list 'setq place (list 'cons item place))))
(defalias 'pop (macro (place)
  (list 'prog1 (list 'car place)
    (list 'setq place (list 'cdr place)))))
(defalias 'save-excursion (macro body
  (list 'internal--save-excursion
    (cons 'lambda (cons nil body)))))
(defalias 'with-current-buffer (macro (buf . body)
  (list 'internal--with-current-buffer buf
    (cons 'lambda (cons nil body)))))
;; --- quasiquote: `x , ,@ read as (quasiquote x) etc. ---
(defalias 'internal--qq (lambda (form)
  (if (atom form)
      (list 'quote form)
    (if (eq (car form) 'unquote)
        (car (cdr form))
      (internal--qq-list form)))))
(defalias 'internal--qq-list (lambda (form)
  (internal--let segs nil)
  (while (consp form)
    (internal--let e (car form))
    (if (and (consp e) (eq (car e) 'unquote-splicing))
        (setq segs (cons (car (cdr e)) segs))
      (setq segs (cons (list 'list (internal--qq e)) segs)))
    (setq form (cdr form)))
  (if form (setq segs (cons (internal--qq form) segs)))
  (cons 'append (reverse segs))))
(defalias 'quasiquote (macro (form) (internal--qq form)))
;; --- definition forms ---
;; Argument lists go to Fe unchanged: its binder reads &optional and
;; &rest itself, as well as its own dotted and bare-symbol forms.  kg
;; used to lower "(a &optional b &rest r)" to "(a b . r)" here, which
;; worked only because the binder accepted any argument count.
(defalias 'internal--interactive-p (lambda (form)
  (if (atom form) nil (eq (car form) 'interactive))))
(defalias 'internal--docstring-p (lambda (form) (stringp form)))
(defalias 'internal--has-interactive (lambda (body)
  (if body (internal--interactive-p (car body)) nil)))
;; Only the declaration immediately after the optional docstring is metadata.
;; A non-string descriptor is wrapped as a closure in the command's lexical
;; environment and evaluated at invocation time.
(defalias 'defun (macro (name params . body)
  (internal--let doc nil)
  (internal--let declaration nil)
  (internal--let spec nil)
  (if (and body (internal--docstring-p (car body)))
      (progn (setq doc (car body)) (setq body (cdr body))))
  (if (internal--has-interactive body)
      (progn
        (setq declaration (car body))
        (setq body (cdr body))
        (if (null body) (setq body (list nil)))
        (internal--let f (cons 'lambda (cons params body)))
        (setq spec (car (cdr declaration)))
        (if (and spec (not (stringp spec)))
            (setq spec (cons 'lambda (cons nil (list spec)))))
        (list 'progn
          (list 'defalias (list 'quote name) f)
          (list 'define-command (list 'quote name)
            (list 'symbol-function (list 'quote name)) spec doc)
          (list 'quote name)))
    (progn
      (if (null body) (setq body (list nil)))
      (internal--let f (cons 'lambda (cons params body)))
      (list 'progn (list 'defalias (list 'quote name) f)
        (list 'internal--remove-command-if-present (list 'quote name))
        (list 'quote name))))))
(defalias 'defmacro (macro (name params . body)
  (list 'progn
    (list 'defalias (list 'quote name)
      (cons 'macro (cons params body)))
    (list 'quote name))))
;; Fe distinguishes an unassigned symbol from one holding nil, so
;; defvar asks boundp rather than reading the variable -- which would
;; now raise void-variable -- and a variable holding nil stays nil.
(defalias 'defvar (macro (name . rest)
  (list 'progn
    (list 'if (list 'boundp (list 'quote name))
      nil
      (list 'setq name (car rest)))
    (list 'quote name))))
(defalias 'defconst (macro (name . rest)
  (list 'progn (list 'setq name (car rest)) (list 'quote name))))
;; Inert outside defun: a stray top-level (interactive) is harmless.
(defalias 'interactive (macro args nil))
(defalias 'ignore-errors (macro body
  (cons 'condition-case (cons nil (cons (cons 'progn body) '((error nil)))))))
;; --- editor helpers ---
(defalias 'string-empty-p (lambda (s) (string= s "")))
(defalias 'thing-at-point (lambda (thing)
  (internal--let bounds (bounds-of-thing-at-point thing))
  (if bounds (buffer-substring (car bounds) (cdr bounds)))))
