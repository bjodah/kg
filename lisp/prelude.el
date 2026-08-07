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

;; Emacs spellings for Fe primitives.  `internal--let' keeps Fe's own
;; `let' primitive reachable after the Emacs `let' macro shadows it; the
;; function bodies below use it, in its one-binding `(internal--let NAME
;; VALUE)' spelling.  That spelling is now a subset of what the primitive
;; accepts -- Phase 8's fe work gave the primitive Emacs' binding lists
;; too -- but the macro below still shadows the name, because the
;; primitive introduces its binding into the *enclosing* body rather than
;; over a body of its own, and because it does not refuse `t', `nil' or a
;; keyword in binding position, which Emacs' `let' does and the macro
;; does.  `defalias' stores the captured primitive itself -- not a symbol
;; designator -- so the later redefinition of `let' cannot reach it
;; through the cell it aliased.
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
(defalias 'assq (lambda (key alist)
  (internal--let hit nil)
  (while (and alist (null hit))
    (if (eq key (car (car alist))) (setq hit (car alist)))
    (setq alist (cdr alist)))
  hit))
(defalias 'mapc (lambda (f lst)
  (internal--let original lst)
  (while lst
    (funcall f (car lst))
    (setq lst (cdr lst)))
  original))
;; SEPARATOR has been optional since Emacs 29, defaulting to "".
(defalias 'mapconcat (lambda (f lst &optional separator)
  (if (null separator) (setq separator ""))
  (internal--let result "")
  (internal--let first t)
  (while lst
    (if first
        (setq first nil)
      (setq result (concat result separator)))
    (setq result (concat result (funcall f (car lst))))
    (setq lst (cdr lst)))
  result))
(defalias 'nreverse (lambda (lst)
  (internal--let result nil)
  (while lst
    (internal--let next (cdr lst))
    (setcdr lst result)
    (setq result lst)
    (setq lst next))
  result))
(defalias 'delq (lambda (elt lst)
  (internal--let result lst)
  (internal--let previous nil)
  (while lst
    (if (eq elt (car lst))
        (if previous
            (setcdr previous (cdr lst))
          (setq result (cdr lst)))
      (setq previous lst))
    (setq lst (cdr lst)))
  result))
(defalias 'delete (lambda (elt lst)
  (internal--let result lst)
  (internal--let previous nil)
  (while lst
    (if (equal elt (car lst))
        (if previous
            (setcdr previous (cdr lst))
          (setq result (cdr lst)))
      (setq previous lst))
    (setq lst (cdr lst)))
  result))
;; Emacs' add-to-list is a *function* taking a symbol, not a macro taking
;; a place: (let ((s 'my-list)) (add-to-list s 1)) has to reach my-list.
;; A macro that pattern-matched a literal (quote NAME) could only ever
;; assign to a variable spelled `s'.  `set'/`symbol-value' are the pair
;; that makes it a function here; the membership test is `equal', as in
;; Emacs, and the return value is the resulting list.
(defalias 'add-to-list (lambda (variable item &optional appendp)
  (internal--let current (symbol-value variable))
  (if (member item current)
      current
    (set variable
      (if appendp
          (append current (list item))
        (cons item current))))))
(defalias 'identity (lambda (value) value))
;; Emacs' float syntax is exactly what fe's writer prints for a float
;; (shortest round-trip, always with a `.' or an exponent), and its
;; integer syntax is fe's for an integer, so %S is the whole conversion.
(defalias 'number-to-string (lambda (n)
  (if (numberp n)
      (format "%S" n)
    (signal 'wrong-type-argument (list 'numberp n)))))
;; Emacs answers a list of codepoints, and nil for "".  Built backwards
;; from the end so the spine is one `while' and no reverse is needed;
;; `substring' indexes in codepoints and `string-to-char' decodes one.
(defalias 'string-to-list (lambda (s)
  (internal--let i (string-length s))
  (internal--let res nil)
  (while (< 0 i)
    (setq i (- i 1))
    (setq res (cons (string-to-char (substring s i (+ i 1))) res)))
  res))
(defalias 'prog2 (macro (first second . rest)
  (list 'progn first (cons 'prog1 (cons second rest)))))
;; Emacs answers (wrong-number-of-arguments max 0) for a bare (max), not
;; a prose error, so these signal the condition with the same data.
(defalias 'max (lambda args
  (if (null args)
      (signal 'wrong-number-of-arguments (list 'max 0))
    (internal--let result (car args))
    (setq args (cdr args))
    (while args
      (if (> (car args) result) (setq result (car args)))
      (setq args (cdr args)))
    result)))
(defalias 'min (lambda args
  (if (null args)
      (signal 'wrong-number-of-arguments (list 'min 0))
    (internal--let result (car args))
    (setq args (cdr args))
    (while args
      (if (< (car args) result) (setq result (car args)))
      (setq args (cdr args)))
    result)))
;; --- control macros ---
(defalias 'cond (macro clauses
  (if clauses
      (list 'if (car (car clauses))
        (if (cdr (car clauses))
            (cons 'progn (cdr (car clauses)))
          (car (car clauses)))
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
;; The binding names are checked here, where both `let' and `let*' pass
;; through, rather than in each expansion: `t', nil and keywords are
;; constants and Emacs answers (setting-constant t) for (let ((t 1)) t).
;; fe's core `let' refuses all three too, so this is now a second opinion
;; rather than the only one -- it stays because `let*' expands to the
;; two-argument `internal--let' spelling, one binding at a time, and
;; because one rule with one message is worth more than the line it costs.
(defalias 'internal--bind-name (lambda (b)
  (internal--let name (if (atom b) b (car b)))
  (if (or (eq name t) (eq name nil) (keywordp name))
      (signal 'setting-constant (list name))
    name)))
(defalias 'internal--bind-value (lambda (b)
  (if (atom b) nil (car (cdr b)))))
;; `let' normalises its binding list and hands it to fe's core
;; bindings-list `let' (reachable here as `internal--let', captured above
;; before this macro shadowed the name).  Until Phase 11 this expanded to
;; an immediate lambda application instead, which is why a `let' over a
;; `defvar'd name was lexical: a lambda parameter is a lexical
;; environment entry unconditionally -- in fe and, measured, in Emacs 31
;; too -- so the special flag is consulted at fe's binding-list paths and
;; nowhere else (11A Decisions 2-3).  Routing through the core form is
;; what puts kg's `let' on one of those paths.  fe still compiles a
;; binding list with no marked target into the same lambda application,
;; so the lexical case pays nothing for this.
;;
;; Normalising rather than passing the list through is deliberate and
;; measured: fe's core form takes (NAME VALUE) pairs and bare symbols,
;; but raises wrong-type-argument for the one-element list (a) where
;; Emacs -- and kg before this change -- answers nil for (let ((a)) a).
;; The same walk keeps (let ((a 1 2)) a) answering 1, kg's long-standing
;; reading of a shape Emacs rejects; that is unchanged behaviour, not a
;; new decision, and it is recorded rather than fixed here.
(defalias 'let (macro (bindings . body)
  (internal--let pairs nil)
  (while bindings
    (setq pairs (cons (list (internal--bind-name (car bindings))
                            (internal--bind-value (car bindings)))
                  pairs))
    (setq bindings (cdr bindings)))
  (cons 'internal--let (cons (reverse pairs) body))))
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
;; `save-excursion' and `with-current-buffer' are `unwind-protect' over
;; the body, not a native that calls back into the evaluator with the
;; body as a lambda.  That is what lets a `throw' inside the body reach a
;; `catch' outside the form: fe walls a throw search at a native re-entry
;; boundary by design, so the old shape turned every such throw into
;; (no-catch TAG VALUE).  `condition-case' crossed the native shape too
;; (06E) and still crosses this one.
;;
;; The editor state each form preserves is captured before the
;; unwind-protect is entered and put back by its cleanup: a marker at
;; point in the current buffer for `save-excursion', the current buffer
;; alone for `with-current-buffer' -- which is Emacs' scope for the two
;; forms, and exactly what the natives preserved before.  Both are
;; GC-managed adapter objects, so an unwind that never reaches the
;; cleanup cannot leak one; the restore additionally hands
;; `save-excursion's marker record straight back to the bounded pool,
;; which is what keeps that pool a bound on open excursions rather than
;; on how many a loop may perform.
(defalias 'save-excursion (macro body
  (list 'internal--let
    (list (list 'internal--excursion (list 'internal--excursion-capture t)))
    (list 'unwind-protect (cons 'progn body)
      (list 'internal--excursion-restore 'internal--excursion)))))
(defalias 'with-current-buffer (macro (buf . body)
  (list 'internal--let
    (list (list 'internal--excursion (list 'internal--excursion-capture nil)))
    (list 'unwind-protect
      (list 'progn (list 'set-buffer buf) (cons 'progn body))
      (list 'internal--excursion-restore 'internal--excursion)))))
;; --- quasiquote: `x , ,@ read as (quasiquote x) etc. ---
(defalias 'internal--qq (lambda (form &optional depth)
  (if (null depth) (setq depth 1))
  (if (atom form)
      (list 'quote form)
    (if (eq (car form) 'unquote)
        (if (= depth 1)
            (car (cdr form))
          (list 'list (list 'quote 'unquote)
            (internal--qq (car (cdr form)) (- depth 1))))
      (if (eq (car form) 'quasiquote)
          (list 'list (list 'quote 'quasiquote)
            (internal--qq (car (cdr form)) (+ depth 1)))
        (internal--qq-list form depth))))))
;; A dotted tail reaches here two ways, and Emacs answers both the same.
;; The reader keeps `(a . b)' improper, so the walk below simply runs out
;; of conses with `form' still non-nil.  But `(1 . ,x)' is NOT improper:
;; the reader turns `,x' into the two-element list `(unquote x)', so the
;; whole form reads as the PROPER list `(1 unquote x)'.  Emacs' rule is
;; that an `unquote' form appearing in cdr position is the dotted tail,
;; which is also why `(a unquote b)' means `(a . VALUE-OF-b)' there.
;; `segs' is accumulated reversed and handed to internal--qq-dotted that
;; way; the tail is the last argument of the `append', which is exactly
;; what makes an improper result -- (append (list 'a) 'b) is (a . b) --
;; and keeps `,@' segments working in front of one.
(defalias 'internal--qq-list (lambda (form depth)
  (internal--let segs nil)
  (internal--let tail nil)
  (internal--let dotted nil)
  (while (consp form)
    (if (and (= depth 1) (eq (car form) 'unquote) (consp (cdr form)))
        (progn
          (setq tail (car (cdr form)))
          (setq dotted t)
          (setq form nil))
      (internal--let e (car form))
      (if (and (= depth 1) (consp e) (eq (car e) 'unquote-splicing))
          (setq segs (cons (car (cdr e)) segs))
        (setq segs (cons (list 'list (internal--qq e depth)) segs)))
      (setq form (cdr form))))
  (if form
      (internal--qq-dotted segs (internal--qq form depth))
    (if dotted
        (internal--qq-dotted segs tail)
      (cons 'append (reverse segs))))))
;; SEGS is still reversed here; the tail form joins it as the first
;; element so that reversing puts it last, where `append' reads it as the
;; final -- possibly non-list -- argument.
(defalias 'internal--qq-dotted (lambda (segs tail)
  (cons 'append (reverse (cons tail segs)))))
(defalias 'quasiquote (macro (form) (internal--qq form)))
;; --- definition forms ---
;; Argument lists go to Fe unchanged: its binder reads &optional and
;; &rest itself, as well as its own dotted and bare-symbol forms.  kg
;; used to lower "(a &optional b &rest r)" to "(a b . r)" here, which
;; worked only because the binder accepted any argument count.
(defalias 'internal--interactive-p (lambda (form)
  (if (atom form) nil (eq (car form) 'interactive))))
(defalias 'internal--docstring-p (lambda (form) (stringp form)))
(setq internal--docs nil)
(defalias 'internal--doc-put (lambda (name doc)
  (setq internal--docs (cons (cons name doc) internal--docs))
  doc))
(defalias 'documentation (lambda (name)
  (internal--let entry (assq name internal--docs))
  (if entry (cdr entry) nil)))
(defalias 'internal--has-interactive (lambda (body)
  (if body (internal--interactive-p (car body)) nil)))
;; Only the declaration immediately after the optional docstring is metadata.
;; A non-string descriptor is wrapped as a closure in the command's lexical
;; environment and evaluated at invocation time.
(defalias 'defun (macro (name params . body)
  (internal--let doc nil)
  (internal--let declaration nil)
  (internal--let spec nil)
  ;; A lone leading string is the *body*, not a docstring: Emacs answers
  ;; "Just a doc." for (defun onlydoc (x) "Just a doc."), and stripping it
  ;; here made the function return nil.  A string is documentation only
  ;; when at least one further form follows it -- which still leaves
  ;; (defun f () "Doc" (interactive)) returning nil, since the declaration
  ;; is removed after the docstring and the empty body becomes (nil).
  (if (and body (cdr body) (internal--docstring-p (car body)))
      (progn (setq doc (car body)) (setq body (cdr body))))
  (if (and body (internal--declare-p (car body)))
      (setq body (cdr body)))
  (if (internal--has-interactive body)
      (progn
        (setq declaration (car body))
        (setq body (cdr body))
        (if (null body) (setq body (list nil)))
        (internal--let f (cons 'lambda (cons params body)))
        (setq spec (car (cdr declaration)))
        (if (and spec (not (stringp spec)))
            (setq spec (cons 'lambda (cons nil (list spec)))))
        ;; One evaluation, command root first, function cell last (07D
        ;; item 3).  The old expansion defalias'd and then read the cell
        ;; back with (symbol-function 'name) for define-command: two
        ;; reads of one name, and a define-command failure -- a full
        ;; 32-slot command table -- left the new function installed under
        ;; a name that is no longer a command.  Binding the closure once
        ;; makes the command root and the function cell the same object,
        ;; and puts the step that can fail before the step that cannot.
        (list 'let (list (list 'internal--defun-fn f))
          (list 'define-command (list 'quote name)
            'internal--defun-fn spec doc)
          (list 'defalias (list 'quote name) 'internal--defun-fn)
          (if doc (list 'internal--doc-put (list 'quote name) doc) nil)
          (list 'quote name)))
    (progn
      (if (null body) (setq body (list nil)))
      (internal--let f (cons 'lambda (cons params body)))
      (list 'progn (list 'defalias (list 'quote name) f)
        (list 'internal--remove-command-if-present (list 'quote name))
        (if doc (list 'internal--doc-put (list 'quote name) doc) nil)
        (list 'quote name))))))
;; The same lone-string rule `defun' takes: a string is documentation
;; only when at least one further form follows it, because otherwise it
;; is the body.  Emacs answers "just a string" for
;; (defmacro m (x) "just a string") then (m 3); stripping it here made
;; the expansion nil.  The docstring goes to internal--doc-put like
;; `defun''s, so (documentation 'the-macro) answers.
(defalias 'defmacro (macro (name params . body)
  (internal--let doc nil)
  (if (and body (cdr body) (internal--docstring-p (car body)))
      (progn (setq doc (car body)) (setq body (cdr body))))
  (if (and body (internal--declare-p (car body)))
      (setq body (cdr body)))
  (if (null body) (setq body (list nil)))
  (list 'progn
    (list 'defalias (list 'quote name)
      (cons 'macro (cons params body)))
    (if doc (list 'internal--doc-put (list 'quote name) doc) nil)
    (list 'quote name))))
;; Fe distinguishes an unassigned symbol from one holding nil, so
;; defvar asks boundp rather than reading the variable -- which would
;; now raise void-variable -- and a variable holding nil stays nil.
;; (defvar SYMBOL &optional VALUE DOCSTRING): the value is (car rest)
;; whenever rest is non-nil, even when it is a string.  Emacs answers
;; "hello" for (progn (defvar dv "hello") dv); classifying a lone string
;; as the docstring left dv unbound.  Only the SECOND element is ever
;; documentation.
;;
;; Since Phase 11 the expansion also marks the symbol, which is what
;; makes `let' over it dynamic (11A Decision 2).  The two arities mark
;; differently, and the difference is Emacs' measured one rather than a
;; simplification: a two-argument `defvar' marks *full* --
;; `special-variable-p' answers t -- while a one-argument `(defvar v)'
;; sets the let-dynamic flag alone, so `special-variable-p' answers nil
;; and yet `(let ((v 5)) v)' is 5.  kg's marking is global where Emacs
;; scopes a one-argument `defvar' to the file it appears in; that
;; approximation is recorded in test/lisp-compat/features.json's
;; `prelude-defvar' row rather than defended.  Marking is one-way: fe
;; has no unmark, because Emacs has none either.
(defalias 'defvar (macro (name . rest)
  (internal--let value-present (if rest t nil))
  (internal--let doc
    (if (and (cdr rest) (stringp (car (cdr rest))))
        (car (cdr rest))
      nil))
  (list 'progn
    (list 'internal--mark-special (list 'quote name) value-present)
    (list 'if (list 'boundp (list 'quote name))
      nil
      (if value-present (list 'setq name (car rest)) nil))
    (if doc (list 'internal--doc-put (list 'quote name) doc) nil)
    (list 'quote name))))
;; `defconst' marks full, as Emacs' does, and -- also as Emacs' does --
;; the constancy is a declaration and not enforcement: the name is still
;; `let'-rebindable and still `setq'-able.
(defalias 'defconst (macro (name . rest)
  (internal--let doc (if (and (car rest) (cdr rest)
                              (stringp (car (cdr rest))))
                         (car (cdr rest)) nil))
  (list 'progn
    (list 'internal--mark-special (list 'quote name) t)
    (list 'setq name (car rest))
    (if doc (list 'internal--doc-put (list 'quote name) doc) nil)
    (list 'quote name))))
(defalias 'internal--custom-presentation-keyword-p (lambda (key)
  (or (eq key :type) (eq key :options) (eq key :group)
      (eq key :tag) (eq key :link) (eq key :version)
      (eq key :package-version))))
(defalias 'internal--custom-semantics-keyword-p (lambda (key)
  (or (eq key :initialize) (eq key :set) (eq key :get)
      (eq key :require) (eq key :set-after) (eq key :risky)
      (eq key :safe) (eq key :local))))
;; This is a declaration over `defvar', not a Customize state
;; implementation. Presentation metadata is deliberately inert.
(defalias 'defcustom (macro (name standard doc . keywords)
  (internal--let tail keywords)
  (while tail
    (if (or (null (cdr tail)) (not (keywordp (car tail))))
        (error "defcustom: keyword tail must contain pairs")
      (if (internal--custom-semantics-keyword-p (car tail))
          (error "defcustom: semantic keyword is unsupported")
        (if (not (internal--custom-presentation-keyword-p (car tail)))
            (error "defcustom: unknown keyword")))
      (setq tail (cdr (cdr tail)))))
  (list 'defvar name standard doc)))
(defalias 'custom-set-variables (macro entries
  (internal--let forms nil)
  (while entries
    (internal--let entry (car entries))
    (if (or (not (consp entry)) (not (eq (car entry) 'quote)))
        (error "custom-set-variables: entries must be quoted pairs")
      (internal--let pair (car (cdr entry)))
      (if (or (not (consp pair)) (not (consp (cdr pair)))
              (cdr (cdr pair)) (not (symbolp (car pair))))
          (error "custom-set-variables: entry must be (SYMBOL VALUE)")
        (setq forms (cons (list 'setq (car pair) (car (cdr pair))) forms)))
    (setq entries (cdr entries))))
  (cons 'progn (reverse forms))))
;; Inert outside defun: a stray top-level (interactive) is harmless.
(defalias 'interactive (macro args nil))
(defalias 'internal--declare-p (lambda (form)
  (and (consp form) (eq (car form) 'declare))))
(defalias 'ignore-errors (macro body
  (cons 'condition-case (cons nil (cons (cons 'progn body) '((error nil)))))))
(defalias 'setq-default (symbol-function 'setq))
(defalias 'setq-local (symbol-function 'setq))
(defalias 'kbd (lambda (key)
  (if (and (stringp key)
           (= (string-length key) 5)
           (string= (substring key 0 4) "C-c "))
      key
    (if (and (stringp key)
             (= (string-length key) 7)
             (string= (substring key 0 6) "C-c C-"))
        key
      (error "kbd: cannot bind key sequence")))))
;; --- the loader ---
;; `load' and `require' are Lisp loops over C stream natives (Phase 12's
;; fix cycle, on fe's input-unit trio), NOT C calls into a nested
;; evaluation: each form is read by internal--read-form -- which latches
;; the form's own path:LINE for diagnostics -- and run by `eval' in the
;; CURRENT run, inside the input unit internal--load-begin entered.  That
;; is what lets a `throw' out of a loaded file reach a `catch' around the
;; (load ...), an error reach an enclosing condition-case, and a quit
;; stay a quit, exactly as if the file's forms were written in place --
;; Emacs' dynamic-extent rule, which the retired containment barrier
;; could not honour for throws (the old load-throw-reachability
;; divergence).  The handle is bound inside the unwind-protect so there
;; is no window where an open stream has no cleanup armed; the cleanup
;; closes the stream (freeing the C buffer and restoring the enclosing
;; input unit) on every completion kind via fe's cleanup drain.
;; internal--read-form answers (FORM) or nil, keeping end-of-file
;; distinguishable from a form that reads as nil.
(defalias 'internal--load-loop (lambda (path)
  (let ((h nil))
    (unwind-protect
        (progn
          (setq h (internal--load-begin path))
          (let ((cell (internal--read-form h)))
            (while cell
              (eval (car cell))
              (setq cell (internal--read-form h)))))
      (if h (internal--load-end h))))))
(defalias 'load (lambda (name)
  (internal--load-loop (internal--resolve-load name))
  t))
;; require: the no-op arm (already provided) is internal--require-resolve
;; answering nil.  The cyclic-require stack entry is published inside the
;; unwind-protect for the same no-window reason as the stream handle, and
;; popped in its cleanup -- the pop used to be a C-side
;; FeProtectWithCleanup, and rides fe's cleanup drain either way, so a
;; condition-case catching a load error leaves a retry a retry rather
;; than a false "cyclic require".  The did-not-provide verdict
;; (internal--require-check) runs only on the success path, before the
;; pop so the C side still sees this chain's depth.
(defalias 'require (lambda (feature &optional filename)
  (let ((path (internal--require-resolve feature filename)))
    (if path
        (let ((pushed nil))
          (unwind-protect
              (progn
                (setq pushed (internal--require-push feature))
                (internal--load-loop path)
                (internal--require-check feature))
            (if pushed (internal--require-pop))))))
  feature))

;; --- editor helpers ---
(defalias 'string-empty-p (lambda (s) (string= s "")))
(defalias 'thing-at-point (lambda (thing)
  (internal--let bounds (bounds-of-thing-at-point thing))
  (if bounds (buffer-substring (car bounds) (cdr bounds)))))
