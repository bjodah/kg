;;; grep-buffer.el --- collect a buffer's matching lines into a report  -*- lexical-binding: t -*-
;;;
;;; Search the current buffer for a regexp and write every matching line
;;; into `grep-buffer-name', one `LINE: TEXT' row per hit, with the
;;; matched text bracketed and a count at the end.  `grep-buffer-goto'
;;; then takes you from a row back to the line it names.
;;;
;;; Load it with (require 'grep-buffer) and, if you want keys for it,
;;; bind the commands from your init file:
;;;
;;;   (require 'grep-buffer)
;;;   (global-set-key "C-c g" "grep-buffer")
;;;   (global-set-key "C-c w" "grep-buffer-word-at-point")
;;;   (global-set-key "C-c j" "grep-buffer-goto")
;;;
;;; It is deliberately not kg's built-in M-x occur, which is C, has
;;; next-error integration and re-runs itself.  This one is a package: it
;;; brackets what matched, counts the hits, folds case when you ask it
;;; to, and is a hundred lines of Lisp you can change.

(provide 'grep-buffer)

(defvar grep-buffer-name "*grep-buffer*"
  "Buffer the report is written to.")

(defvar grep-buffer-ignore-case nil
  "Non-nil folds case, by lowering both the pattern and each line.")

(defvar grep-buffer-highlight t
  "Non-nil brackets each match in the report, as [this].")

(defvar grep-buffer--source nil
  "Name of the buffer the last report was collected from.")

;; The current buffer's text as a list of lines.  One buffer-substring
;; rather than a walk with forward-line: the whole point of the split is
;; that the line NUMBER comes from the list position, so there is nothing
;; for point to remember.
(defun grep-buffer--lines ()
  (split-string (buffer-substring (point-min) (point-max)) "\n"))

;; TEXT, lowered when `grep-buffer-ignore-case' says so.  kg's regexp
;; engine has no case-fold-search, so folding is done to the strings.
(defun grep-buffer--fold (text)
  (if grep-buffer-ignore-case (downcase text) text))

;; The (NUMBER . LINE) pairs of LINES matching PATTERN, numbered from 1.
(defun grep-buffer--hits (pattern lines)
  (let ((n 0)
        (folded (grep-buffer--fold pattern))
        (hits nil))
    (dolist (line lines)
      (setq n (1+ n))
      (if (string-match folded (grep-buffer--fold line))
          (push (cons n line) hits)))
    (nreverse hits)))

;; LINE with every occurrence of PATTERN bracketed, or unchanged when
;; `grep-buffer-highlight' is nil.  Folding cannot be applied here -- the
;; replacement has to run over the text as it really is -- so a
;; case-folded search reports its lines unbracketed rather than
;; bracketing the wrong span.
(defun grep-buffer--highlight (pattern line)
  (if (and grep-buffer-highlight (not grep-buffer-ignore-case))
      (replace-regexp-in-string pattern
                                (lambda (m) (concat "[" m "]"))
                                line)
    line))

;; One report row: the line number, then the line with its indentation
;; trimmed off so the numbers line up.
(defun grep-buffer--row (pattern hit)
  (format "%d: %s"
          (car hit)
          (string-trim (grep-buffer--highlight pattern (cdr hit)))))

;; The whole report for HITS, as one string: the rows, then a summary.
(defun grep-buffer--report (name pattern hits)
  (string-join
   (append (mapcar (lambda (h) (grep-buffer--row pattern h)) hits)
           (list (format "%d match%s for %s in %s"
                         (length hits)
                         (if (= (length hits) 1) "" "es")
                         pattern
                         name)))
   "\n"))

(defun grep-buffer (pattern)
  "Report every line of the current buffer matching PATTERN."
  (interactive "sGrep buffer for regexp: ")
  (let ((name (buffer-name))
        (lines (grep-buffer--lines)))
    (let ((hits (grep-buffer--hits pattern lines)))
      (setq grep-buffer--source name)
      (switch-to-buffer grep-buffer-name)
      (erase-buffer)
      (insert (grep-buffer--report name pattern hits))
      (goto-char (point-min))
      (message "%d match(es) in %s" (length hits) name))))

(defun grep-buffer-literal (needle)
  "`grep-buffer' over a literal NEEDLE rather than a regexp."
  (interactive "sGrep buffer for text: ")
  (grep-buffer (regexp-quote needle)))

(defun grep-buffer-word-at-point ()
  "`grep-buffer' for the word point is on."
  (interactive)
  (let ((word (thing-at-point 'word)))
    (if (or (null word) (string-empty-p word))
        (message "No word at point")
      (grep-buffer-literal word))))

(defun grep-buffer-goto ()
  "From a report row, go to the line it names in the searched buffer."
  (interactive)
  (beginning-of-line)
  (let ((row (buffer-substring (point) (progn (end-of-line) (point)))))
    (beginning-of-line)
    (if (or (null grep-buffer--source) (not (string-match "^\\([0-9]+\\):" row)))
        (message "Not a %s row" grep-buffer-name)
      (let ((line (string-to-number (match-string 1 row))))
        (switch-to-buffer grep-buffer--source)
        (goto-line line)
        (message "Line %d of %s" line grep-buffer--source)))))

;;; grep-buffer.el ends here
