;;; emacs-shim.el --- canonical-record oracle shim for the Elisp package corpus  -*- lexical-binding: t; -*-

;; Not part of Fe or kg's own Lisp; this only runs under the pinned
;; Emacs oracle via `emacs -Q --batch -l oracle/emacs-shim.el CASE.json',
;; driven by utils/check_elisp_packages.py --regenerate-oracle.  It is a
;; byte-for-byte copy of test/lisp-compat/oracle/emacs-shim.el's record
;; protocol, plus one addition: the vendored package sources under
;; external/elpa/ are not on Emacs' default load-path, so this shim adds
;; the repository root to it before any case's setup forms run.  (The kg
;; side resolves the same relative load path against test/kgbatch's cwd,
;; which is the repository root, so the two oracles load the identical
;; tracked file.)
;;
;; `kind` is always decided by *which branch below ran* -- the
;; `condition-case` clause that caught control, or the plain return path
;; -- never by inspecting the text of an error message.

(require 'json)

;; Make external/elpa/ resolvable for (load "external/elpa/s.el") etc.
(add-to-list 'load-path
	     (expand-file-name "../../.."
			       (file-name-directory load-file-name)))

(defun kg-compat--read-case (path)
  "Read the JSON case object at PATH as an alist."
  (with-temp-buffer
    (insert-file-contents path)
    (goto-char (point-min))
    (json-parse-buffer :object-type 'alist :array-type 'list)))

(defun kg-compat--eval-string (source)
  "Read one form from SOURCE and evaluate it with lexical binding on."
  (eval (car (read-from-string source)) t))

(defun kg-compat--run-case (case)
  "Evaluate CASE's setup forms then its expression, and return one record."
  (let ((setup (alist-get 'setup case))
        (expr (alist-get 'expr case)))
    (condition-case err
        (progn
          (dolist (form setup)
            (kg-compat--eval-string form))
          (kg-compat--eval-string expr))
      (:success
       `((kind . "value")
         (type . ,(symbol-name (type-of err)))
         (printed . ,(prin1-to-string err))))
      (quit
       '((kind . "quit")))
      (error
       `((kind . "condition")
         (condition . ,(symbol-name (car err)))
         (data . ,(prin1-to-string (cdr err)))
         (condition_source . "structured"))))))

(let* ((path (car command-line-args-left))
       (case (kg-compat--read-case path))
       (record (kg-compat--run-case case)))
  (send-string-to-terminal (json-serialize record))
  (send-string-to-terminal "\n"))
