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
;; --- hygiene for the prelude's own temporaries -----------------------
;;
;; A `let' over a name the user has `defvar'd binds DYNAMICALLY (Phase
;; 11, 11A Decision 2), and every definition in this file is written with
;; ordinary readable temporary names.  So a prelude temporary that is
;; still bound while USER code runs -- a `mapcar' function, a `sort'
;; predicate, a form `load' is evaluating -- is a capture in both
;; directions at once: the callback's assignment lands on the prelude's
;; accumulator, and the prelude's accumulator is what the user's variable
;; reads back.  Measured on the tree before the sweep below:
;;
;;   (defvar res 0)
;;   (mapcar (lambda (x) (setq res x) x) (list 1 2 3))
;;
;; raised wrong-type-argument -- the callback had overwritten `mapcar's
;; half-built list with an integer -- and `res' was still 0 afterwards.
;; `(defvar first 0) (mapconcat (lambda (x) (setq first x) x) (list "a"
;; "b") "-")' answered "ab", losing the separator, and `(defvar current
;; (list 9)) (add-to-list 'current 1)' left `current' at (9), because the
;; `set' wrote the shadow the function's own binding had just made.
;;
;; THE FIX IS THE SHAPE THE BINDING TAKES.  `save-excursion' fixed its one
;; temporary in Phase 14 the way a MACRO can: it mints a `gensym' at
;; expansion time, and an uninterned symbol is a name nothing the user
;; writes can reach.  A function body has no expansion to mint one in, so
;; these use the other unreachable binding kind fe has: a LAMBDA
;; PARAMETER, which fe binds lexically unconditionally -- the special flag
;; is consulted at fe's binding-list paths and nowhere else (11A Decision
;; 2), which is exactly why `let' over a `defvar'd name was lexical here
;; until Phase 11 routed it onto the core form.  So each capturable
;; temporary below is bound by an immediately-applied lambda,
;; `((lambda (NAME) BODY) INITIAL)' -- the pre-Phase-11 lowering of `let',
;; used deliberately and locally where hygiene is the requirement.  The
;; name stays the readable one, and no `defvar' of it can reach the
;; binding.
;;
;; GENSYM WAS IMPLEMENTED FIRST AND MEASURED OUT.  A macro substituting a
;; fresh gensym through each of these fourteen bodies at load time works
;; and reads well, but kg's arena is fixed and collects nothing during
;; startup, so the substitution walk's own cost is permanent high-water
;; mark.  Measured, at kgbatch's post-prelude probe: 11281 -> 40153 live
;; objects of 56259 (20.0% -> 71.4%) with a copying walk, and
;; 11281 -> 20906 (37.2%) with a destructive one that allocates nothing
;; itself.  That +9625 carries into test_lisp.c's Phase 8 census, whose
;; margin assertion is peak_live * 3 < total_slots and whose measured
;; figure is 14579, so the destructive walk alone puts it past a third of
;; the arena.  The cost is fe's call overhead, not the copy: about nine
;; object slots per call, one call per cons -- measured at 590 slots to
;; substitute one name through a 45-cons lambda.  The lambda-parameter
;; form gives the identical property for nothing.
;;
;; ONLY the temporaries that are live while user code runs are swept.  The
;; rest -- `reverse's `res', `split-string's `parts', every
;; macro-expansion temporary, which is most of them -- bind and unbind
;; with no callback in between, so nothing can observe them.  doc/TODO.md
;; carries the census and the classification.
;; --- list library, all iterative ---
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
;; Emacs' `length' is generic over sequences and answers
;; (wrong-type-argument sequencep X) for anything that is not one -- not
;; `listp', which is what the bare `while' below reported for (length 5)
;; by falling into (cdr 5).  A dotted pair still says listp, from the same
;; walk and about the offending TAIL, which is Emacs' answer too:
;; (length '(1 . 2)) is (wrong-type-argument listp 2) on both sides.
(defalias 'length (lambda (x)
  (if (stringp x)
      (string-length x)
    (if (not (listp x))
        (signal 'wrong-type-argument (list 'sequencep x)))
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
  ((lambda (res)
     (while lst
       (setq res (cons (funcall f (car lst)) res))
       (setq lst (cdr lst)))
     (reverse res))
   nil)))
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
  ((lambda (original)
     (while lst
       (funcall f (car lst))
       (setq lst (cdr lst)))
     original)
   lst)))
;; SEPARATOR has been optional since Emacs 29, defaulting to "".
(defalias 'mapconcat (lambda (f lst &optional separator)
  (if (null separator) (setq separator ""))
  ((lambda (result first)
     (while lst
       (if first
           (setq first nil)
         (setq result (concat result separator)))
       (setq result (concat result (funcall f (car lst))))
       (setq lst (cdr lst)))
     result)
   "" t)))
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
;;
;; CURRENT is a lambda parameter for a reason that is not the callback
;; one: VARIABLE is a name the CALLER chose, so `(add-to-list 'current 1)'
;; used to have the temporary shadow the very variable the `set' was
;; supposed to write.  The write reached the shadow, the caller's list was
;; left untouched, and nothing said so.
(defalias 'add-to-list (lambda (variable item &optional appendp)
  ((lambda (current)
     (if (member item current)
         current
       (set variable
         (if appendp
             (append current (list item))
           (cons item current)))))
   (symbol-value variable))))
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
  ((lambda (i)
     (while (< i count)
       (funcall body i)
       (setq i (+ i 1))))
   0)))
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
;;
;; The temporary the captured state is bound to is a `gensym' -- an
;; uninterned symbol, so nothing the body can write reaches it.  It used
;; to be the ordinary symbol `internal--excursion', which meant a body
;; that assigned that name made the cleanup raise, and a raising cleanup
;; REPLACES the completion it is unwinding, so the user's own error was
;; lost.  Measured before the change:
;;   (condition-case e (save-excursion (setq internal--excursion nil)
;;     (error "MY-ERROR")) (error (format "%S" e)))
;; answered the cleanup's complaint about a value of the wrong type.  It
;; answers ("MY-ERROR") now.  This was Phase 14's hygiene demonstration,
;; and the first name of the sweep the header block above describes.  It
;; is a `gensym' rather than a lambda parameter because it can be: these
;; two are MACROS, so a name can be minted at expansion time and the
;; temporary has to survive into an expansion the body is spliced inside.
(defalias 'save-excursion (macro body
  (internal--let sym (gensym "internal--excursion-"))
  (list 'internal--let
    (list (list sym (list 'internal--excursion-capture t)))
    (list 'unwind-protect (cons 'progn body)
      (list 'internal--excursion-restore sym)))))
(defalias 'with-current-buffer (macro (buf . body)
  (internal--let sym (gensym "internal--excursion-"))
  (list 'internal--let
    (list (list sym (list 'internal--excursion-capture nil)))
    (list 'unwind-protect
      (list 'progn (list 'set-buffer buf) (cons 'progn body))
      (list 'internal--excursion-restore sym)))))
;; `with-temp-buffer' is Emacs' shape exactly: a fresh buffer, made
;; current for the body, killed on every way out.  Three details are kg's
;; and each one is forced.
;;
;; The NAME is minted at run time, from `gensym', not at expansion time:
;; a fixed name would make a nested or recursive `with-temp-buffer' reuse
;; the buffer it is already inside, and Emacs' `generate-new-buffer' --
;; which kg does not have -- is what stops that there.
;;
;; The cleanup CLEARS THE MODIFIED FLAG before killing.  kg's
;; `kill-buffer' refuses a modified buffer rather than asking (there is
;; no prompt inside an unwind cleanup to ask with), and a temp buffer
;; that the body wrote to is modified by definition, so without this line
;; every useful `with-temp-buffer' would raise on its way out.
;;
;; The kill is the OUTER form, after `with-current-buffer' has already
;; put the caller's buffer back: killing the current buffer would leave
;; the frame pointing at a dead one.  And it is wrapped in
;; `ignore-errors', for the reason `save-excursion' above documents: a
;; cleanup that raises REPLACES the completion it is unwinding, so a
;; refused kill would swallow the body's value AND the body's error.  kg
;; refuses one when the editor's lifecycle event queue is full, which is
;; a real bound: the queue drains once per keystroke, each temp buffer
;; costs it three events out of 64, so a single command that opens more
;; than about twenty temp buffers starts leaking them.  Leaking a buffer
;; is a worse outcome than not leaking one and a much better outcome than
;; losing what the body computed.
(defalias 'with-temp-buffer (macro body
  (internal--let name (gensym "internal--temp-name-"))
  (list 'internal--let
    (list (list name (list 'symbol-name (list 'gensym " *temp*-"))))
    (list 'unwind-protect
      (cons 'with-current-buffer
        (cons (list 'get-buffer-create name) body))
      (list 'progn
        (list 'with-current-buffer (list 'get-buffer-create name)
          (list 'set-buffer-modified-p nil))
        (list 'ignore-errors (list 'kill-buffer (list 'get-buffer-create name))))))))
;; `beginning-of-buffer'/`end-of-buffer' are `goto-char' sugar here, and
;; deliberately nothing more.  Emacs' are commands that also PUSH THE
;; MARK, so C-x C-x returns you; kg's `set-mark' additionally lights the
;; region up, which a Lisp call has no business doing, and kg has no
;; unhighlighted `push-mark' to use instead.  Recorded as a divergence
;; (the manifest's beginning-of-buffer/end-of-buffer rows) rather than
;; approximated.  The Emacs manual's own advice for Lisp code is
;; `(goto-char (point-min))', which is what these are.
(defalias 'beginning-of-buffer (lambda () (goto-char (point-min))))
(defalias 'end-of-buffer (lambda () (goto-char (point-max))))
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
;; The definition registry: one entry per name a definition form has
;; introduced, whose cdr is the docstring or nil.  Two questions read it.
;; `documentation' asks for the text, and answers nil either way -- for a
;; name with no docstring and for a name nothing has defined.  `apropos'
;; (lisp/help-fns.el) asks for the NAMES, which is why an undocumented
;; `defun' registers as well: a function you can call and cannot find is
;; not much better than one that does not exist.  The cost of that is two
;; conses per definition.
(setq internal--docs nil)
;; Emacs puts a variable's docstring on the symbol's plist, under
;; `variable-documentation', where `documentation-property' reads it back
;; -- so `(get 'fill-column 'variable-documentation)' answers there and
;; a program can `put' one over it.  The registry above is kg's own and
;; stays: `apropos' asks `documentation' for a line about ANY name, and
;; the property is the only half Emacs has.  Nothing is stored twice --
;; both hold the same string object -- and nothing is stored at all for
;; an undocumented `defvar', which is why this is a call and not a bare
;; `put'.
;; (documentation-property SYMBOL PROPERTY &optional RAW): the property,
;; when it is a string.  Emacs stores a built-in variable's documentation
;; as an offset into its DOC file and resolves it here, which is why the
;; function exists at all rather than callers writing `get'; kg has no
;; DOC file and no built-in variables to need one, so a non-string
;; property is nil -- Emacs' own answer for an integer that indexes
;; nothing, and a recorded divergence for a cons, which Emacs reads as a
;; (FILE . POSITION) reference and raises on.  RAW is accepted and
;; ignored: kg stores no `substitute-command-keys' markup to leave in.
(defalias 'documentation-property (lambda (symbol property &optional raw)
  (internal--let text (get symbol property))
  (if (stringp text) text nil)))
;; The registry first, then the command table: a BUILT-IN command has no
;; Lisp definition to have recorded a docstring, and the one-line summary
;; cmd.c carries -- the same text M-x and the help screen show -- is the
;; honest answer for it.
(defalias 'documentation (lambda (name)
  (internal--let entry (assq name internal--docs))
  (if (and entry (cdr entry))
      (cdr entry)
    (internal--command-documentation name))))
;; Every name the registry knows, newest first and with duplicates, which
;; is the shape the alist has; `apropos' filters and de-duplicates.
(defalias 'internal--defined-names (lambda ()
  (internal--let names nil)
  (internal--let tail internal--docs)
  (while tail
    (setq names (cons (car (car tail)) names))
    (setq tail (cdr tail)))
  names))
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
        ;; The specification as WRITTEN, kept beside the thunk the line
        ;; below wraps a form one in.  `define-command' takes it as a
        ;; fifth argument and `interactive-form' reads it back; without
        ;; it a form spec is only a closure, which is callable and shows
        ;; a reader nothing (Phase 19, doc/TODO.md's interactive
        ;; reflection row).
        (internal--let raw spec)
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
            'internal--defun-fn spec doc (list 'quote raw))
          (list 'defalias (list 'quote name) 'internal--defun-fn)
          (list 'internal--doc-put (list 'quote name) doc)
          (list 'quote name)))
    (progn
      (if (null body) (setq body (list nil)))
      (internal--let f (cons 'lambda (cons params body)))
      (list 'progn (list 'defalias (list 'quote name) f)
        (list 'internal--remove-command-if-present (list 'quote name))
        (list 'internal--doc-put (list 'quote name) doc)
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
    (list 'internal--doc-put (list 'quote name) doc)
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
;; and yet `(let ((v 5)) v)' is 5.  A one-argument mark is scoped to
;; its INPUT UNIT (one load, require, batch file or M-:), as Emacs
;; scopes it to the evaluation unit; the residuals -- kg answers
;; lexically for a defun written after the defvar and called from
;; another file, where Emacs stays dynamic, and every mark is visible
;; to host context (hooks, command dispatch) -- are recorded in
;; test/lisp-compat/features.json's `prelude-defvar' row rather than
;; defended.  Marking is one-way: fe has no unmark, because Emacs has
;; none either.
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
    (list 'internal--doc-put (list 'quote name) doc)
    (list 'internal--variable-doc-put (list 'quote name) doc)
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
    (list 'internal--doc-put (list 'quote name) doc)
    (list 'internal--variable-doc-put (list 'quote name) doc)
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
;; Emacs' `autoload' arms lazy loading: it puts an autoload object in the
;; function cell, and the first call loads the file and re-dispatches.
;; kg's records nothing and loads nothing, so a function that only an
;; autoload would have provided stays `void-function' at its first CALL,
;; and `fboundp' says nil where Emacs says t.  That is the correct kg
;; answer until kg has package loading at all; what the form buys is that
;; a package's own header no longer stops the load before its first
;; definition -- s.el:34 is the measured first blocker, `void-function
;; autoload'.  The divergence is the `prelude-autoload' manifest row.
(defalias 'autoload (macro args nil))
(defalias 'internal--declare-p (lambda (form)
  (and (consp form) (eq (car form) 'declare))))
(defalias 'ignore-errors (macro body
  (cons 'condition-case (cons nil (cons (cons 'progn body) '((error nil)))))))
;; `setq-local' and `setq-default' were documented aliases of `setq' until
;; Phase 18 -- they wrote the one global binding, and their manifest rows
;; said `divergent' for exactly that reason.  Both are now macros over the
;; two natives that address the right binding: `internal--set-buffer-local'
;; creates this buffer's binding if it has none and writes it, and
;; `set-default' writes the value buffers without a binding of their own
;; see.  Both take SYMBOL VALUE pairs and answer the last value, as `setq'
;; does, and both raise on a dangling final SYMBOL rather than assigning
;; nil to it.
(defalias 'internal--setq-local-forms (lambda (pairs setter)
  (internal--let forms nil)
  (while pairs
    (if (null (cdr pairs))
        (error "setq-local: odd number of arguments")
      (setq forms (cons (list setter (list 'quote (car pairs))
                              (car (cdr pairs)))
                        forms))
      (setq pairs (cdr (cdr pairs)))))
  (cons 'progn (reverse forms))))
(defalias 'setq-local (macro pairs
  (internal--setq-local-forms pairs 'internal--set-buffer-local)))
(defalias 'setq-default (macro pairs
  (internal--setq-local-forms pairs 'set-default)))
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
;; Both temporaries are lambda parameters: `eval' below runs the LOADED
;; FILE's forms with them still bound, so a `let'-bound `h' or `cell'
;; would be captured by any file that had `defvar'd either name.
(defalias 'internal--load-loop (lambda (path)
  ((lambda (h)
     (unwind-protect
         (progn
           (setq h (internal--load-begin path))
           ((lambda (cell)
              (while cell
                (eval (car cell))
                (setq cell (internal--read-form h))))
            (internal--read-form h)))
       (if h (internal--load-end h))))
   nil)))
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
  ((lambda (path)
     (if path
         ((lambda (pushed)
            (unwind-protect
                (progn
                  (setq pushed (internal--require-push feature))
                  (internal--load-loop path)
                  (internal--require-check feature))
              (if pushed (internal--require-pop))))
          nil)))
   (internal--require-resolve feature filename))
  feature))

;; --- startup ---
;; The startup screen switch, declared here so it is `boundp' and
;; `special-variable-p' before any init file runs, exactly as Emacs
;; declares it.  kg's startup screen is the centred logo an empty buffer
;; shows; main.c reads these once, after the init file has had its say,
;; and draws no banner when either is non-nil.
;;
;; In Emacs `inhibit-startup-message' is a `defvaralias' of
;; `inhibit-startup-screen' -- one variable under two names.  kg has no
;; variable aliases, so these are two ordinary variables and the C side
;; treats EITHER being non-nil as "suppressed".  That covers what a user
;; writes; what it does not do is make a `setq' of one name read back
;; through the other, and that is the whole of the divergence.
(defvar inhibit-startup-screen nil
  "Non-nil means do not show the startup screen.")
(defvar inhibit-startup-message nil
  "Emacs' other name for `inhibit-startup-screen'; either one suppresses it.")
(defvar tab-width 8
  "Distance between tab stops (for display of tab characters), in columns.")

;; --- editor helpers ---
(defalias 'string-empty-p (lambda (s) (string= s "")))
(defalias 'thing-at-point (lambda (thing)
  (internal--let bounds (bounds-of-thing-at-point thing))
  (if bounds (buffer-substring (car bounds) (cdr bounds)))))

;; Emacs' line-motion pair, as plain functions for programs: the
;; cmdtable rows of the same names own M-x and the C-a/C-e keys until
;; built-in keys resolve to command names (keymap Plan 01), so neither
;; carries (interactive).  With N not nil or 1, move forward N - 1
;; lines first (0 names the previous line), Emacs' contract; goto-line
;; clamps at both buffer edges, which Emacs also does, and is a no-op
;; in an empty buffer.
(defalias 'move-beginning-of-line (lambda (&optional n)
  (goto-line (+ (line-number-at-pos) (if n n 1) -1))
  nil))

;; End of line is the position before the line's newline; the last
;; line has none and ends at end of buffer.  (bounds-of-thing-at-point
;; 'line) takes the newline in -- its cdr is the next line's start --
;; so step back over exactly one character when the character before
;; cdr is a newline.  That is every line but the last (an empty middle
;; line steps from cdr back onto its own car, which is where its end
;; is); on the last line the character before cdr is either its last
;; character or, when the line is empty so car equals cdr, the
;; PREVIOUS line's newline -- the (< car cdr) guard exists for that
;; one case, where stepping would leave the line.
(defalias 'move-end-of-line (lambda (&optional n)
  (if (and n (not (= n 1)))
      (goto-line (+ (line-number-at-pos) n -1)))
  (let ((bounds (bounds-of-thing-at-point 'line)))
    (if bounds
        (goto-char (if (and (< (car bounds) (cdr bounds))
                            (= (char-after (- (cdr bounds) 1)) 10))
                       (- (cdr bounds) 1)
                     (cdr bounds)))))
  nil))

;; --- the package-writer's string and list library (Phase 15) ---------
;;
;; Everything below is ordinary Lisp over the natives above it, which is
;; the point: `utils/forecast/AUDIT.md' ranked these names by how often
;; the Lisp we want to write reaches for them, and almost none of them
;; needed C.  The three that did -- `string-match', `regexp-quote' and
;; the case conversions -- are natives because they are seams onto the
;; regex engine and onto byte-level text, not because they are hard.
;;
;; Rule 2 of this file's header holds here as everywhere: nothing below
;; recurses over a list spine.  The merge sort is the one place that
;; would naturally have, and it is written as two `while' loops instead.

;; --- match data ---
;; The last match may have been against a buffer or against a string, and
;; `match-string' needs different extractors for the two.  That is not a
;; special case, it is the units lining up: a STRING match reports
;; 0-based character indices and `substring' is 0-based; a BUFFER match
;; reports 1-based positions and `buffer-substring' is 1-based.  A group
;; that did not participate is nil on both sides, as in Emacs.
(defalias 'match-string (lambda (n &optional string)
  (let ((from (match-beginning n)))
    (if (null from)
        nil
      (if string
          (substring string from (match-end n))
        (buffer-substring from (match-end n)))))))

;; --- strings ---
;; Emacs' default SEPARATORS is the regexp "[ \f\t\n\r\v]+", and with it
;; OMIT-NULLS defaults to t; with an explicit SEPARATORS it defaults to
;; nil.  Measured on 31.0.90: (split-string "") is nil and
;; (split-string "" ",") is (""), which is that asymmetry and nothing
;; else.  The loop is Emacs' own: the guard that a repeated EMPTY match at
;; the same place advances by one character is what makes a pattern like
;; "x*" terminate.  Emacs' fourth TRIM argument is not accepted.
(defalias 'split-string (lambda (string &optional separators omit-nulls)
  (let ((re (if separators separators "[ \f\t\n\r\v]+"))
        (keep-nulls (if separators (not omit-nulls) nil))
        (limit (length string))
        (start 0)
        (notfirst nil)
        (parts nil)
        (going t))
    (while going
      (let ((from (if (and notfirst (= start (match-beginning 0))
                           (< start limit))
                      (+ start 1)
                    start)))
        (if (and (< start limit) (string-match re string from))
            (progn
              (let ((piece (substring string start (match-beginning 0))))
                (if (or keep-nulls (not (string= piece "")))
                    (setq parts (cons piece parts))))
              (setq start (match-end 0))
              (setq notfirst t))
          (setq going nil))))
    (let ((piece (substring string start limit)))
      (if (or keep-nulls (not (string= piece "")))
          (setq parts (cons piece parts))))
    (reverse parts))))
(defalias 'string-join (lambda (strings &optional separator)
  (mapconcat 'identity strings separator)))
;; Emacs' TRIM-LEFT/TRIM-RIGHT regexp arguments are refused rather than
;; ignored: the default set -- space, tab, newline, return, form feed,
;; vertical tab -- is what every caller wants, and an anchored user
;; regexp would need the `\\=`' the engine does not have.
(defalias 'internal--trim-reject (lambda (name regexp)
  (if regexp (error "%s: a REGEXP argument is unsupported" name))))
(defalias 'internal--trim-char-p (lambda (c)
  (or (= c 32) (= c 9) (= c 10) (= c 13) (= c 12) (= c 11))))
(defalias 'string-trim-left (lambda (string &optional regexp)
  (internal--trim-reject "string-trim-left" regexp)
  (let ((i 0) (n (length string)) (done nil))
    (while (and (not done) (< i n))
      (if (internal--trim-char-p (string-to-char (substring string i (+ i 1))))
          (setq i (+ i 1))
        (setq done t)))
    (substring string i))))
(defalias 'string-trim-right (lambda (string &optional regexp)
  (internal--trim-reject "string-trim-right" regexp)
  (let ((n (length string)) (done nil))
    (while (and (not done) (< 0 n))
      (if (internal--trim-char-p
            (string-to-char (substring string (- n 1) n)))
          (setq n (- n 1))
        (setq done t)))
    (substring string 0 n))))
(defalias 'string-trim (lambda (string &optional trim-left trim-right)
  (string-trim-left (string-trim-right string trim-right) trim-left)))
(defalias 'string-prefix-p (lambda (prefix string &optional ignore-case)
  (let ((n (length prefix)))
    (and (<= n (length string))
         (let ((head (substring string 0 n)))
           (if ignore-case
               (string= (downcase prefix) (downcase head))
             (string= prefix head)))))))
(defalias 'string-suffix-p (lambda (suffix string &optional ignore-case)
  (let ((n (length suffix)))
    (and (<= n (length string))
         (let ((tail (substring string (- (length string) n))))
           (if ignore-case
               (string= (downcase suffix) (downcase tail))
             (string= suffix tail)))))))
;; `string<' and `string>' were HERE until Phase 20, as prelude Lisp that
;; turned both operands into lists of character codes and compared them a
;; cons at a time.  They are fe primitives now (`StringOperandLess', a
;; string cell at a time), which is why nothing defines them below: the
;; cost was charged per CHARACTER to every caller's step budget, and
;; `apropos' -- kg's one heavy sorter -- was capped at 40 results because
;; of it.
;; Emacs' \\& (the whole match), \\N (group N) and \\\\ in a replacement
;; string.  An escape Emacs rejects outright ("\\q") yields the escaped
;; character here; that is the one shape of replacement string the two
;; disagree about.
(defalias 'internal--replace-expand (lambda (rep string)
  (let ((chars (string-to-list rep)) (out ""))
    (while chars
      (let ((c (car chars)))
        (if (and (= c 92) (cdr chars))
            (let ((d (car (cdr chars))))
              (setq chars (cdr chars))
              (cond ((= d 38)
                     (setq out (concat out (match-string 0 string))))
                    ((= d 92) (setq out (concat out "\\")))
                    ((and (<= 48 d) (<= d 57))
                     (let ((m (match-string (- d 48) string)))
                       (setq out (concat out (if m m "")))))
                    (t (setq out (concat out (char-to-string d))))))
          (setq out (concat out (char-to-string c)))))
      (setq chars (cdr chars)))
    out)))
;; REP is a replacement string or a function of the matched text.  Emacs'
;; FIXEDCASE is accepted and ignored -- kg never case-adjusts a
;; replacement, because it never case-folds a match either -- and its
;; SUBEXP and START arguments are not accepted.  The empty-match rule is
;; Emacs' own: an empty match copies one character and advances, and the
;; final empty match at end of string is not replaced, which is why
;; (replace-regexp-in-string "x*" "-" "abc") is "-a-b-c" and not
;; "-a-b-c-".
;; All five temporaries are live across the `funcall' of a function REP,
;; so all five are lambda parameters rather than `let' bindings.
(defalias 'replace-regexp-in-string (lambda
  (regexp rep string &optional fixedcase literal)
  ((lambda (start out limit)
     (while (and (< start limit) (string-match regexp string start))
       ((lambda (mb me)
          (setq out (concat out (substring string start mb)))
          (setq out (concat out
                      (if (stringp rep)
                          (if literal rep
                            (internal--replace-expand rep string))
                        (funcall rep (match-string 0 string)))))
          (if (= me mb)
              (progn
                (if (< mb limit)
                    (setq out (concat out (substring string mb (+ mb 1)))))
                (setq start (+ mb 1)))
            (setq start me)))
        (match-beginning 0) (match-end 0)))
     (concat out (substring string (if (< start limit) start limit))))
   0 "" (length string))))

;; --- lists ---
(defalias 'cdar (lambda (x) (cdr (car x))))
(defalias 'caddr (lambda (x) (car (cdr (cdr x)))))
(defalias 'cdddr (lambda (x) (cdr (cdr (cdr x)))))
(defalias 'cadddr (lambda (x) (car (cdr (cdr (cdr x))))))
(defalias 'elt (lambda (sequence n)
  (if (stringp sequence)
      (progn
        (if (or (< n 0) (<= (length sequence) n))
            (signal 'args-out-of-range (list sequence n)))
        (string-to-char (substring sequence n (+ n 1))))
    (nth n sequence))))
(defalias 'butlast (lambda (list &optional n)
  (let ((keep (- (length list) (if n n 1))) (out nil))
    (while (and (< 0 keep) list)
      (setq out (cons (car list) out))
      (setq list (cdr list))
      (setq keep (- keep 1)))
    (reverse out))))
(defalias 'copy-sequence (lambda (sequence)
  (if (stringp sequence)
      (substring sequence 0)
    (let ((out nil))
      (while sequence
        (setq out (cons (car sequence) out))
        (setq sequence (cdr sequence)))
      (reverse out)))))
(defalias 'number-sequence (lambda (from &optional to inc)
  (if (null to)
      (list from)
    (let ((step (if inc inc 1)) (n from) (out nil))
      (if (= step 0) (error "The increment can not be zero"))
      (if (< 0 step)
          (while (<= n to)
            (setq out (cons n out))
            (setq n (+ n step)))
        (while (<= to n)
          (setq out (cons n out))
          (setq n (+ n step))))
      (reverse out)))))
;; Destructive, as Emacs': every argument but the last has to be a list,
;; and the result is the first non-nil one with the rest spliced onto it.
(defalias 'mapcan (lambda (function sequence)
  (apply 'nconc (mapcar function sequence))))
(defalias 'assq-delete-all (lambda (key alist)
  (let ((result alist) (previous nil))
    (while alist
      (if (and (consp (car alist)) (eq key (car (car alist))))
          (if previous
              (setcdr previous (cdr alist))
            (setq result (cdr alist)))
        (setq previous alist))
      (setq alist (cdr alist)))
    result)))
;; Emacs' TESTFN and REMOVE arguments are not accepted; the lookup is
;; `assq', which is Emacs' own default.
(defalias 'alist-get (lambda (key alist &optional default)
  (let ((cell (assq key alist)))
    (if cell (cdr cell) default))))
(defalias 'plist-get (lambda (plist prop)
  (let ((hit nil))
    (while (and plist (cdr plist) (null hit))
      (if (eq (car plist) prop) (setq hit (cdr plist)))
      (setq plist (cdr (cdr plist))))
    (if hit (car hit) nil))))
;; Destructive where it can be, as Emacs': an existing property is
;; overwritten in place and a new one is spliced onto the tail, so only
;; (plist-put nil ...) has to build a fresh list.
(defalias 'plist-put (lambda (plist prop value)
  (let ((tail plist) (done nil))
    (while (and tail (cdr tail) (not done))
      (if (eq (car tail) prop)
          (progn (setcar (cdr tail) value) (setq done t))
        (setq tail (cdr (cdr tail)))))
    (cond (done plist)
          ((null plist) (list prop value))
          (t (let ((last plist))
               (while (cdr last) (setq last (cdr last)))
               (setcdr last (list prop value))
               plist))))))
;; A stable bottom-up merge sort.  It sorts the VALUES into a fresh list
;; and then writes them back over the input's own cons cells with
;; `setcar', which is what makes it match Emacs 31.0.90 exactly: measured
;; there, (let* ((c (list 2)) (x (cons 3 c)) (y (sort x #'<))) ...) leaves
;; y `eq' to x and leaves c holding 3 -- the cells keep their identities
;; and their contents move.  Emacs' keyword calling convention
;; ((sort SEQ :lessp ...)) is not accepted.
;; PREDICATE is user code, and all three of these accumulate across a
;; call to it, so all three are lambda parameters.  `sort's `values' and
;; `cell' stay an ordinary `let': it is entered after the last predicate
;; call has returned, so nothing can observe them.
(defalias 'internal--merge (lambda (a b predicate)
  ((lambda (out)
     (while (and a b)
       (if (funcall predicate (car b) (car a))
           (progn (setq out (cons (car b) out)) (setq b (cdr b)))
         (setq out (cons (car a) out))
         (setq a (cdr a))))
     (while a (setq out (cons (car a) out)) (setq a (cdr a)))
     (while b (setq out (cons (car b) out)) (setq b (cdr b)))
     (nreverse out))
   nil)))
(defalias 'internal--merge-pairs (lambda (runs predicate)
  ((lambda (out)
     (while runs
       (if (cdr runs)
           (progn
             (setq out (cons (internal--merge (car runs) (car (cdr runs))
                               predicate)
                         out))
             (setq runs (cdr (cdr runs))))
         (setq out (cons (car runs) out))
         (setq runs nil)))
     (nreverse out))
   nil)))
(defalias 'sort (lambda (sequence predicate)
  ((lambda (runs)
     (while (cdr runs)
       (setq runs (internal--merge-pairs runs predicate)))
     (let ((values (car runs)) (cell sequence))
       (while cell
         (setcar cell (car values))
         (setq values (cdr values))
         (setq cell (cdr cell))))
     sequence)
   (mapcar 'list sequence))))

;; --- the seq- shim ---
;; Lists only: Emacs' seq- functions are generic over every sequence type
;; through cl-generic, and kg has lists and strings and no dispatch.
(defalias 'seq-map (lambda (function sequence) (mapcar function sequence)))
(defalias 'seq-filter (lambda (predicate sequence)
  ((lambda (out)
     (while sequence
       (if (funcall predicate (car sequence))
           (setq out (cons (car sequence) out)))
       (setq sequence (cdr sequence)))
     (reverse out))
   nil)))
(defalias 'seq-remove (lambda (predicate sequence)
  (seq-filter (lambda (x) (not (funcall predicate x))) sequence)))
(defalias 'seq-find (lambda (predicate sequence &optional default)
  ((lambda (hit found)
     (while (and sequence (not found))
       (if (funcall predicate (car sequence))
           (progn (setq hit (car sequence)) (setq found t)))
       (setq sequence (cdr sequence)))
     (if found hit default))
   nil nil)))
(defalias 'seq-some (lambda (predicate sequence)
  ((lambda (hit)
     (while (and sequence (null hit))
       (setq hit (funcall predicate (car sequence)))
       (setq sequence (cdr sequence)))
     hit)
   nil)))
(defalias 'seq-take (lambda (sequence n)
  (let ((out nil))
    (while (and sequence (< 0 n))
      (setq out (cons (car sequence) out))
      (setq sequence (cdr sequence))
      (setq n (- n 1)))
    (reverse out))))

;; --- arithmetic ---
;; (+ number 0) rather than `number' on the non-negative arm so that
;; (abs -0.0) is 0.0, as it is in Emacs: -0.0 is not less than 0, and
;; adding zero to it is the one operation that normalises the sign.
(defalias 'abs (lambda (number)
  (if (numberp number)
      (if (< number 0) (- number) (+ number 0))
    (signal 'wrong-type-argument (list 'numberp number)))))
;; (% X Y) is the remainder, taking X's sign; (mod X Y) the modulus,
;; taking Y's.  fe's two-argument `truncate' and `floor' are exactly the
;; two roundings that difference is made of, so neither needs a bit of
;; arithmetic of its own.  `%' is integers-only, as in Emacs; `mod'
;; accepts floats.
(defalias '% (lambda (x y)
  (if (and (integerp x) (integerp y))
      (- x (* y (truncate x y)))
    (signal 'wrong-type-argument
      (list 'integer-or-marker-p (if (integerp x) y x))))))
(defalias 'mod (lambda (x y) (- x (* y (floor x y)))))
;; An arithmetic shift: right by N is floor division by 2^N, so
;; (ash -16 -2) is -4 and not -3.  A left shift multiplies, and a product
;; past int64 raises fe's arith-error where Emacs would widen to a bignum
;; -- kg's standing no-bignum divergence, not a new one.  `logand',
;; `logior' and `logxor' are deliberately absent; see doc/lisp-api.md.
(defalias 'ash (lambda (value count)
  (if (< count 0)
      (floor value (expt 2 (- count)))
    (* value (expt 2 count)))))

;; --- documentation for the definitions above -------------------------
;;
;; One table rather than a docstring on each `defalias' above, for a
;; mechanical reason: the definitions ARE `defalias' calls, which take no
;; documentation -- only `defun', `defmacro', `defvar' and `defconst' feed
;; `internal--doc-put', and the prelude cannot use `defun' before it
;; defines it.  Rewriting the file around that would reorder the
;; bootstrap; a table at the end does not, and it keeps the cost visible
;; in one place.
;;
;; PUBLIC NAMES ONLY.  The `internal--' definitions are the prelude's own
;; machinery and are deliberately undocumented: `documentation' answering
;; for them would be a claim that they are surface.
;;
;; `string<' and `string>' are the two entries that document something
;; this file does NOT define -- they became fe primitives in Phase 20 and
;; their rows stayed, because they are the comparators every `sort' call
;; in kg's own Lisp is written with and `C-h f' answering "Not
;; documented." for them would be a worse answer than two conses.
;;
;; The cost is measured, not assumed (Phase 19, and the figures move with
;; the table): 102 entries and 5527 bytes of text cost +1341 objects of
;; the 56259-slot arena -- 9912 -> 11253 live after the prelude, 17.6% ->
;; 20.0% -- and +0.22 ms of prelude load time, 2.57 -> 2.79 ms median of
;; five interleaved runs on a counting build.  That is 8.6% of the
;; prelude and 0.19% of kg's ~117 ms startup, which is why the mechanism
;; stayed a Lisp alist instead of moving to a C table the way the plan
;; allowed if it 'measures as a startup cost'.
;;
;; One line each, which is a deliberate cut: Emacs' own docstrings run to
;; paragraphs and this arena is fixed.  Argument names are the ones the
;; definitions above use.
;;
;; `nconc' rather than `append': the literal below is the prelude's own
;; and nothing else holds it, so joining it onto whatever `defvar' put
;; there costs no copy.
(setq internal--docs (nconc '(
  (1+ . "Return N plus one.")
  (1- . "Return N minus one.")
  (% . "Return the remainder of X divided by Y, with X's sign.")
  (abs . "Return the absolute value of N.")
  (add-to-list . "Add ELEMENT to the list in VARIABLE unless it is already a member.")
  (alist-get . "Return the value of KEY in ALIST, or DEFAULT when it is absent.")
  (append . "Return the concatenation of the argument lists; the last may be any object.")
  (ash . "Return VALUE arithmetically shifted left by COUNT, or right when it is negative.")
  (assoc . "Return the first pair of ALIST whose car `equal's KEY.")
  (assq . "Return the first pair of ALIST whose car is `eq' to KEY.")
  (assq-delete-all . "Return ALIST without the pairs whose car is `eq' to KEY.")
  (autoload . "Accept an autoload declaration; kg loads nothing and records nothing.")
  (beginning-of-buffer . "Move point to the start of the buffer.")
  (butlast . "Return LIST without its last element, or without its last N.")
  (caar . "Return the car of the car of X.")
  (cadddr . "Return the fourth element of X.")
  (caddr . "Return the third element of X.")
  (cadr . "Return the second element of X.")
  (cdar . "Return the cdr of the car of X.")
  (cdddr . "Return X without its first three elements.")
  (cddr . "Return X without its first two elements.")
  (cond . "Evaluate the body of the first clause whose test is non-nil.")
  (copy-sequence . "Return a fresh copy of the list SEQUENCE.")
  (custom-set-variables . "Set each quoted (SYMBOL VALUE) pair, as a Custom file does.")
  (defconst . "Define NAME as a constant with VALUE, and mark it special.")
  (defcustom . "Define NAME as a user option with STANDARD value; a declaration over `defvar'.")
  (defmacro . "Define NAME as a macro taking PARAMS.")
  (defun . "Define NAME as a function taking PARAMS; an (interactive ...) body makes it a command.")
  (defvar . "Declare NAME as a variable, giving it VALUE if it has none, and mark it special.")
  (delete . "Return LIST without the elements `equal' to ELEMENT.")
  (delq . "Return LIST without the elements `eq' to ELEMENT.")
  (documentation . "Return the documentation string recorded for NAME, or nil.")
  (dolist . "Bind the first element of SPEC to each element of its list and run the body.")
  (dotimes . "Bind the first element of SPEC to each integer from 0 below its count.")
  (elt . "Return the element of SEQUENCE at index N; SEQUENCE may be a list or a string.")
  (end-of-buffer . "Move point to the end of the buffer.")
  (equal . "Return t when A and B have the same structure and contents.")
  (identity . "Return X.")
  (ignore-errors . "Run the body, returning nil instead of raising an `error'.")
  (interactive . "Declare a command's interactive specification; inert outside `defun'.")
  (kbd . "Return KEYS unchanged: kg's key sequences are already strings.")
  (last . "Return the last cons of LIST, or its last N conses.")
  (length . "Return the number of elements in SEQUENCE, or the characters in a string.")
  (let . "Bind each (NAME VALUE) of BINDINGS in parallel and run the body.")
  (let* . "Bind each (NAME VALUE) of BINDINGS in sequence and run the body.")
  (listp . "Return t when X is a cons or nil.")
  (load . "Read and evaluate the Lisp file FILE, searching `load-path' for a bare name.")
  (mapc . "Call FUNCTION on each element of LIST for effect, and return LIST.")
  (mapcan . "Call FUNCTION on each element of LIST and `nconc' the results.")
  (mapcar . "Return the list of FUNCTION's values over the elements of LIST.")
  (mapconcat . "Join FUNCTION's values over LIST into one string, with SEPARATOR between them.")
  (match-string . "Return the text matched by group N of the last search, or nil.")
  (max . "Return the largest of the arguments.")
  (member . "Return the tail of LIST starting at the first element `equal' to ELEMENT.")
  (memq . "Return the tail of LIST starting at the first element `eq' to ELEMENT.")
  (min . "Return the smallest of the arguments.")
  (mod . "Return the modulus of X by Y, with Y's sign.")
  (move-beginning-of-line . "Move point to the beginning of the current line.")
  (move-end-of-line . "Move point to the end of the current line.")
  (nconc . "Join the argument lists by rewriting their tails, and return the result.")
  (nreverse . "Return LIST reversed, rewriting its tails.")
  (nth . "Return the Nth element of LIST, counting from zero.")
  (nthcdr . "Return LIST without its first N elements.")
  (null . "Return t when X is nil.")
  (number-sequence . "Return the list of numbers from FROM to TO, stepping by STEP.")
  (number-to-string . "Return the printed representation of the number N.")
  (plist-get . "Return the value of PROPERTY in the property list PLIST.")
  (plist-put . "Store VALUE for PROPERTY in the property list PLIST and return it.")
  (pop . "Remove and return the first element of the list in PLACE.")
  (prog1 . "Run the body and return the value of its first form.")
  (prog2 . "Run the body and return the value of its second form.")
  (progn . "Run the body and return the value of its last form.")
  (push . "Add ELEMENT to the front of the list in PLACE.")
  (quasiquote . "The backquote reader macro: build FORM, evaluating its unquoted parts.")
  (replace-regexp-in-string . "Return TEXT with each match of REGEXP replaced by REPLACEMENT.")
  (require . "Load the feature FEATURE unless it has already been provided.")
  (reverse . "Return a fresh list with the elements of LIST in the opposite order.")
  (save-excursion . "Run the body and restore the buffer and point afterwards.")
  (seq-filter . "Return the elements of SEQUENCE for which PREDICATE is non-nil.")
  (seq-find . "Return the first element of SEQUENCE for which PREDICATE is non-nil.")
  (seq-map . "Return the list of FUNCTION's values over the elements of SEQUENCE.")
  (seq-remove . "Return the elements of SEQUENCE for which PREDICATE is nil.")
  (seq-some . "Return the first non-nil value of PREDICATE over SEQUENCE.")
  (seq-take . "Return the first N elements of SEQUENCE.")
  (setq-default . "Set the global (default) value of each NAME, ignoring any buffer-local one.")
  (setq-local . "Set the current buffer's own value of each NAME, creating it if needed.")
  (sort . "Return LIST sorted by the two-argument PREDICATE.")
  (split-string . "Return the list of substrings of TEXT separated by matches of SEPARATORS.")
  (string< . "Return t when string A sorts before string B.")
  (string> . "Return t when string A sorts after string B.")
  (string-empty-p . "Return t when the string S has no characters.")
  (string-join . "Return the strings of LIST concatenated, with SEPARATOR between them.")
  (string-prefix-p . "Return t when STRING begins with PREFIX.")
  (string-suffix-p . "Return t when STRING ends with SUFFIX.")
  (string-to-list . "Return the list of character codes in STRING.")
  (string-trim . "Return STRING without leading or trailing whitespace.")
  (string-trim-left . "Return STRING without leading whitespace.")
  (string-trim-right . "Return STRING without trailing whitespace.")
  (thing-at-point . "Return the text of the THING at point, or nil; THING may be `word' or `symbol'.")
  (unless . "Run the body when the condition is nil.")
  (when . "Run the body when the condition is non-nil.")
  (with-current-buffer . "Run the body with BUFFER current, and restore the previous one.")
  (with-temp-buffer . "Run the body in a fresh temporary buffer, and kill it afterwards.")
  (zerop . "Return t when the number N is zero.")
  ) internal--docs))
