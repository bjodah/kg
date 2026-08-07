;;; pipeline-text.el --- Proof 3's editor half: pipelines over a buffer.  -*- lexical-binding: t; -*-
;;;
;;; The other half of the parent plan's §14 Proof 3.  `pipeline.el' is
;;; pure and is measured against Emacs 31 case by case; this file is the
;;; part that could not be, because it reads a buffer and defines
;;; interactive commands.  Keeping them apart is what makes the claim
;;; "its pure-language portions also run unchanged under Emacs 31"
;;; checkable instead of aspirational.
;;;
;;; It reaches its helper with `require', not `load': `load' resolves a
;;; bare name only in the config directory, while `require' searches
;;; `load-path', so `require' is what lets an installed copy and a
;;; per-user copy both work.  That single (require 'pipeline) is also
;;; §14's provide/require and multiple-files bullets.
;;;
;;; Nothing here throws across a native re-entry boundary: every catch in
;;; this package is inside one `pipeline.el' function, and the commands
;;; below call into it and take a value back.  A `throw' out of a
;;; `with-current-buffer' or `save-excursion' body USED TO BE answered
;;; `no-catch' (kg's then-recorded catch-throw-reachability divergence),
;;; so that shape is deliberately absent rather than accidentally
;;; missing.  Phase 11 closed it for those two forms -- they are prelude
;;; `unwind-protect' macros now, and a throw out of either reaches a
;;; catch outside it -- but a hook, a process filter and a nested
;;; `command-execute' are still walls, so the absence is still
;;; deliberate.

(require 'pipeline)

(provide 'pipeline-text)

;; The package's own default pipeline: add one, triple, then subtract
;; twenty.  A function rather than a variable so a caller reading
;; `pipeline-text-steps' as a value gets nothing -- the Lisp-2 point
;; `pipeline.el' makes with `pipeline-origin', restated where a user
;; would meet it.
;;
;; The third step is what makes the early exit below observable: with a
;; pipeline that only ever grows, stopping at the last step and running
;; to the end are the same number, and a catch/throw that cannot change
;; an answer proves nothing.
(defun pipeline-text-steps ()
  (list (pipeline-adder 1) (pipeline-scaler 3) (pipeline-adder -20)))

(defun pipeline-text-run ()
  "Insert the default pipeline's answer for the size of this buffer."
  (interactive)
  (insert (number-to-string
           (pipeline-run (pipeline-text-steps)
                         (- (point-max) (point-min))))))

(defun pipeline-text-run-until ()
  "Insert the default pipeline's answer, stopping at the first step past 5."
  (interactive)
  (insert (number-to-string
           (pipeline-run-until (pipeline-text-steps)
                               (- (point-max) (point-min))
                               5))))

(defun pipeline-text-explain ()
  "Insert what (pipeline-boost 3) expands to: one step, then fully.

The reflective use §14's macro-expansion bullet asks for -- a package
that can tell a user what its own macros mean, rather than only running
them."
  (interactive)
  (insert (format "%S then %S"
                  (pipeline-explain-step (list 'pipeline-boost 3))
                  (pipeline-explain (list 'pipeline-boost 3)))))

(defun pipeline-text-run-safely ()
  "Run a pipeline whose second step signals, and insert the report.

The error is data here, not an escape: `pipeline-run-safely' catches it
and the command finishes normally, so the buffer is edited."
  (interactive)
  (insert (format "%S"
                  (pipeline-run-safely
                   (list (pipeline-adder 1) (lambda (x) (car x)))
                   (- (point-max) (point-min))))))

(defun pipeline-text-run-unguarded ()
  "Run the same failing pipeline with no handler, so the error escapes.

The counterpart to `pipeline-text-run-safely': the condition reaches
kg's command boundary and is reported there, and nothing is inserted."
  (interactive)
  (insert (number-to-string
           (pipeline-run (list (pipeline-adder 1) (lambda (x) (car x)))
                         (- (point-max) (point-min))))))
