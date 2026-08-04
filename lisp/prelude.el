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
;;; the rule is obsolete.  Three `(list 'quote nil)' workarounds it
;;; licensed -- in `cond', `defvar' and `interactive' -- are vestigial
;;; rather than wrong and are left in place, since removing them changes
;;; evaluated code.  The fourth, in this file's own `setq' macro, is gone:
;;; the Phase 2 dialect cutover deleted that macro outright now that core
;;; `setq' is a real Fe special form.
;;;
;;; Macros expand on *every* invocation, and each expansion is charged
;;; against the evaluation step budget.  That is a performance property,
;;; not a correctness one; an earlier version of this comment claimed
;;; expansion happened once per call site, which was never true of Fe.
;;;
;;; The prelude bootstraps itself with the primitives it is about to wrap:
;;; definitions use Fe's core `setq' special form, not the `defun' macro
;;; defined further down, so nothing here depends on an expansion
;;; happening before `defun' itself exists.

;; Emacs spellings for Fe primitives.  `internal--let' keeps Fe's
;; one-binding `let' reachable after the Emacs `let' shadows it; the
;; function bodies below use it.
(setq internal--let let)
(setq progn do)
(setq null not)
(setq eq is)
(setq function (lambda (f) f))
(setq 1+ (lambda (n) (+ n 1)))
(setq 1- (lambda (n) (- n 1)))
(setq caar (lambda (x) (car (car x))))
(setq cadr (lambda (x) (car (cdr x))))
(setq cddr (lambda (x) (cdr (cdr x))))
(setq listp (lambda (x) (if (null x) t (consp x))))
;; --- list library, all iterative ---
(setq reverse (lambda (lst)
  (internal--let res nil)
  (while lst
    (setq res (cons (car lst) res))
    (setq lst (cdr lst)))
  res))
(setq internal--append2 (lambda (a b)
  (internal--let res b)
  (internal--let r (reverse a))
  (while r
    (setq res (cons (car r) res))
    (setq r (cdr r)))
  res))
(setq append (lambda lists
  (internal--let r (reverse lists))
  (internal--let res (car r))
  (setq r (cdr r))
  (while r
    (setq res (internal--append2 (car r) res))
    (setq r (cdr r)))
  res))
(setq length (lambda (x)
  (if (stringp x)
      (string-length x)
    (internal--let n 0)
    (while x
      (setq n (+ n 1))
      (setq x (cdr x)))
    n)))
(setq nthcdr (lambda (n lst)
  (while (and (< 0 n) lst)
    (setq n (- n 1))
    (setq lst (cdr lst)))
  lst))
(setq nth (lambda (n lst) (car (nthcdr n lst))))
(setq last (lambda (lst)
  (while (cdr lst) (setq lst (cdr lst)))
  lst))
;; Structural on lists; Fe's `is' compares pairs by identity.  Only the
;; car descends, so the spine cost is a loop, not a frame.
(setq equal (lambda (a b)
  (internal--let same t)
  (while (and same (consp a) (consp b))
    (if (equal (car a) (car b)) nil (setq same nil))
    (setq a (cdr a))
    (setq b (cdr b)))
  (and same (not (consp a)) (not (consp b)) (is a b))))
(setq mapcar (lambda (f lst)
  (internal--let res nil)
  (while lst
    (setq res (cons (f (car lst)) res))
    (setq lst (cdr lst)))
  (reverse res)))
(setq member (lambda (elt lst)
  (while (and lst (not (equal elt (car lst))))
    (setq lst (cdr lst)))
  lst))
(setq memq (lambda (elt lst)
  (while (and lst (not (eq elt (car lst))))
    (setq lst (cdr lst)))
  lst))
(setq assoc (lambda (key alist)
  (internal--let hit nil)
  (while (and alist (null hit))
    (if (equal key (car (car alist))) (setq hit (car alist)))
    (setq alist (cdr alist)))
  hit))
;; --- control macros ---
(setq cond (macro clauses
  (if clauses
      (list 'if (car (car clauses))
        (cons 'progn (cdr (car clauses)))
        (cons 'cond (cdr clauses)))
    (list 'quote nil))))
(setq when (macro (test . body)
  (list 'if test (cons 'progn body))))
(setq unless (macro (test . body)
  (cons 'if (cons test (cons nil body)))))
(setq internal--first (lambda args (car args)))
(setq prog1 (macro (first . body)
  (cons 'internal--first (cons first body))))
;; --- binding forms ---
(setq internal--bind-name (lambda (b) (if (atom b) b (car b))))
(setq internal--bind-value (lambda (b)
  (if (atom b) nil (car (cdr b)))))
;; Parallel, via immediate application: the value forms are evaluated
;; as arguments, in the environment outside the new bindings.
(setq let (macro (bindings . body)
  (cons (cons 'lambda
          (cons (mapcar internal--bind-name bindings) body))
    (mapcar internal--bind-value bindings))))
(setq let* (macro (bindings . body)
  (internal--let forms nil)
  (while bindings
    (setq forms (cons (list 'internal--let
                        (internal--bind-name (car bindings))
                        (internal--bind-value (car bindings)))
                  forms))
    (setq bindings (cdr bindings)))
  (cons 'progn (internal--append2 (reverse forms) body))))
;; --- iteration macros ---
(setq internal--dolist (lambda (items body)
  (while items
    (body (car items))
    (setq items (cdr items)))))
(setq dolist (macro (spec . body)
  (list 'progn
    (list 'internal--dolist (car (cdr spec))
      (cons 'lambda (cons (list (car spec)) body)))
    (car (cdr (cdr spec))))))
(setq internal--dotimes (lambda (count body)
  (internal--let i 0)
  (while (< i count)
    (body i)
    (setq i (+ i 1)))))
(setq dotimes (macro (spec . body)
  (list 'progn
    (list 'internal--dotimes (car (cdr spec))
      (cons 'lambda (cons (list (car spec)) body)))
    (car (cdr (cdr spec))))))
(setq push (macro (item place)
  (list 'setq place (list 'cons item place))))
(setq pop (macro (place)
  (list 'prog1 (list 'car place)
    (list 'setq place (list 'cdr place)))))
(setq save-excursion (macro body
  (list 'internal--save-excursion
    (cons 'lambda (cons nil body)))))
(setq with-current-buffer (macro (buf . body)
  (list 'internal--with-current-buffer buf
    (cons 'lambda (cons nil body)))))
;; --- quasiquote: `x , ,@ read as (quasiquote x) etc. ---
(setq internal--qq (lambda (form)
  (if (atom form)
      (list 'quote form)
    (if (eq (car form) 'unquote)
        (car (cdr form))
      (internal--qq-list form)))))
(setq internal--qq-list (lambda (form)
  (internal--let segs nil)
  (while (consp form)
    (internal--let e (car form))
    (if (and (consp e) (eq (car e) 'unquote-splicing))
        (setq segs (cons (car (cdr e)) segs))
      (setq segs (cons (list 'list (internal--qq e)) segs)))
    (setq form (cdr form)))
  (if form (setq segs (cons (internal--qq form) segs)))
  (cons 'append (reverse segs))))
(setq quasiquote (macro (form) (internal--qq form)))
;; --- definition forms ---
;; Argument lists go to Fe unchanged: its binder reads &optional and
;; &rest itself, as well as its own dotted and bare-symbol forms.  kg
;; used to lower "(a &optional b &rest r)" to "(a b . r)" here, which
;; worked only because the binder accepted any argument count.
(setq internal--interactive-p (lambda (form)
  (if (atom form) nil (eq (car form) 'interactive))))
(setq internal--has-interactive (lambda (body)
  (internal--let hit nil)
  (while body
    (if (internal--interactive-p (car body)) (setq hit t))
    (setq body (cdr body)))
  hit))
(setq internal--strip-interactive (lambda (body)
  (internal--let out nil)
  (while body
    (if (internal--interactive-p (car body))
        nil
      (setq out (cons (car body) out)))
    (setq body (cdr body)))
  (reverse out)))
;; A body form (interactive) registers the function as a command, the
;; way Emacs makes a defun interactive; define-command takes the
;; symbol.  defun returns the name, as Emacs does.
(setq defun (macro (name params . body)
  (internal--let f (cons 'lambda (cons params
                     (internal--strip-interactive body))))
  (if (internal--has-interactive body)
      (list 'progn (list 'setq name f)
        (list 'define-command (list 'quote name) name)
        (list 'quote name))
    (list 'progn (list 'setq name f) (list 'quote name)))))
(setq defmacro (macro (name params . body)
  (list 'progn
    (list 'setq name
      (cons 'macro (cons params body)))
    (list 'quote name))))
;; Fe distinguishes an unassigned symbol from one holding nil, so
;; defvar asks boundp rather than reading the variable -- which would
;; now raise void-variable -- and a variable holding nil stays nil.
(setq defvar (macro (name . rest)
  (list 'progn
    (list 'if (list 'boundp (list 'quote name))
      (list 'quote nil)
      (list 'setq name (car rest)))
    (list 'quote name))))
(setq defconst (macro (name . rest)
  (list 'progn (list 'setq name (car rest)) (list 'quote name))))
;; Inert outside defun: a stray top-level (interactive) is harmless.
(setq interactive (macro args (list 'quote nil)))
;; --- editor helpers ---
(setq string-empty-p (lambda (s) (string= s "")))
(setq thing-at-point (lambda (thing)
  (internal--let bounds (bounds-of-thing-at-point thing))
  (if bounds (buffer-substring (car bounds) (cdr bounds)))))
