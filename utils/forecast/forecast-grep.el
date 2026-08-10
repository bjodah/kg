;;; forecast-grep.el --- sketch: a buffer-local grep/occur helper  -*- lexical-binding: t -*-
;;
;; A FORECAST SKETCH.  Hand-written for utils/forecast_audit.py; nothing
;; loads, installs or evaluates this file.  It is written the way the
;; package would be written for Emacs -- without first checking which of
;; these names kg has -- because that is the only way the audit measures
;; demand rather than measuring what we already built.
;;
;; What it would do: collect every line of the current buffer matching a
;; pattern into a report buffer, one `FILE:LINE: TEXT' row per hit, with
;; the matched text emphasised and a summary line at the end.

(defvar forecast-grep-buffer "*forecast-grep*"
  "Buffer the report is written to.")

(defvar forecast-grep-context 0
  "Lines of context to include either side of a hit.")

(defvar forecast-grep-ignore-case nil
  "Non-nil folds case, by lowering both pattern and subject.")

(defun forecast-grep--lines ()
  "The current buffer's text, as a list of lines."
  (split-string (buffer-substring (point-min) (point-max)) "\n"))

(defun forecast-grep--fold (text)
  "TEXT, lowered when `forecast-grep-ignore-case' says so."
  (if forecast-grep-ignore-case (downcase text) text))

(defun forecast-grep--hit-p (pattern line)
  "Non-nil when LINE matches PATTERN."
  (string-match (forecast-grep--fold pattern) (forecast-grep--fold line)))

(defun forecast-grep--hits (pattern lines)
  "The (NUMBER . LINE) pairs of LINES matching PATTERN."
  (let ((n 0)
        (hits nil))
    (dolist (line lines)
      (setq n (1+ n))
      (when (forecast-grep--hit-p pattern line)
        (push (cons n line) hits)))
    (nreverse hits)))

(defun forecast-grep--highlight (pattern line)
  "LINE with every occurrence of PATTERN bracketed."
  (replace-regexp-in-string pattern
                            (lambda (m) (concat "[" m "]"))
                            line))

(defun forecast-grep--format-hit (name pattern hit)
  "One report row for HIT in the buffer called NAME."
  (format "%s:%d: %s"
          name
          (car hit)
          (string-trim (forecast-grep--highlight pattern (cdr hit)))))

(defun forecast-grep--report (name pattern hits)
  "The whole report for HITS as one string."
  (string-join
   (append (mapcar (lambda (h) (forecast-grep--format-hit name pattern h))
                   hits)
           (list (format "%d match%s for %s"
                         (length hits)
                         (if (= (length hits) 1) "" "es")
                         pattern)))
   "\n"))

(defun forecast-grep (pattern)
  "Report every line of the current buffer matching PATTERN."
  (interactive "sPattern: ")
  (let* ((name (buffer-name))
         (hits (forecast-grep--hits pattern (forecast-grep--lines)))
         (text (forecast-grep--report name pattern hits)))
    (with-current-buffer (get-buffer-create forecast-grep-buffer)
      (erase-buffer)
      (insert text)
      (goto-char (point-min)))
    (message "%d hit(s)" (length hits))))

(defun forecast-grep-literal (needle)
  "`forecast-grep' over a literal NEEDLE rather than a pattern."
  (interactive "sText: ")
  (forecast-grep (regexp-quote needle)))

(defun forecast-grep-word-at-point ()
  "`forecast-grep' for the word point is on."
  (interactive)
  (let ((word (thing-at-point 'word)))
    (if (or (null word) (string-empty-p word))
        (message "no word at point")
      (forecast-grep-literal word))))

(defun forecast-grep-next ()
  "Jump to the buffer position of the report row point is on."
  (interactive)
  (let ((row (thing-at-point 'line)))
    (when (string-match ":\\([0-9]+\\):" row)
      (let ((line (string-to-number (match-string 1 row))))
        (with-current-buffer (get-buffer (buffer-name))
          (goto-line line)
          (beginning-of-line))))))

(provide 'forecast-grep)
;;; forecast-grep.el ends here
