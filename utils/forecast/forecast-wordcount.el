;;; forecast-wordcount.el --- sketch: word frequencies for a buffer  -*- lexical-binding: t -*-
;;
;; A FORECAST SKETCH.  Hand-written for utils/forecast_audit.py; nothing
;; loads, installs or evaluates this file.  Written the way it would be
;; written for Emacs.
;;
;; This is the sketch that answers the plan's Declined watch item on hash
;; tables honestly, by not avoiding them: counting occurrences of a key
;; over a whole buffer is the one shape where an Emacs package writer
;; reaches for `make-hash-table' without thinking about it, so that is
;; what is written here.  The alist spelling underneath
;; `forecast-wordcount--tally-alist' is the same algorithm without one,
;; and it is here so the report can say what the fallback costs rather
;; than assert that there is one.

(defvar forecast-wordcount-buffer "*wordcount*"
  "Buffer the report is written to.")

(defvar forecast-wordcount-minimum 1
  "Words seen fewer than this many times are left out of the report.")

(defvar forecast-wordcount-stop-words '("the" "a" "an" "of" "and" "to")
  "Words never counted.")

(defun forecast-wordcount--words ()
  "The current buffer's words, downcased, in order."
  (seq-remove 'string-empty-p
              (split-string (downcase (buffer-substring (point-min)
                                                        (point-max)))
                            "[^a-z']+")))

(defun forecast-wordcount--interesting-p (word)
  "Non-nil when WORD is worth counting."
  (and (< 1 (length word))
       (not (member word forecast-wordcount-stop-words))))

(defun forecast-wordcount--tally (words)
  "A hash table mapping each of WORDS to how often it occurs."
  (let ((table (make-hash-table :test 'equal)))
    (dolist (word words)
      (when (forecast-wordcount--interesting-p word)
        (puthash word (1+ (gethash word table 0)) table)))
    table))

(defun forecast-wordcount--tally-alist (words)
  "The same tally as an alist, for when no hash tables are available."
  (let ((counts nil))
    (dolist (word words)
      (when (forecast-wordcount--interesting-p word)
        (let ((cell (assoc word counts)))
          (if cell
              (setcdr cell (1+ (cdr cell)))
            (push (cons word 1) counts)))))
    counts))

(defun forecast-wordcount--pairs (table)
  "TABLE as a list of (WORD . COUNT), most frequent first."
  (let ((pairs nil))
    (maphash (lambda (word count) (push (cons word count) pairs)) table)
    (sort (seq-filter (lambda (p) (<= forecast-wordcount-minimum (cdr p)))
                      pairs)
          (lambda (a b) (> (cdr a) (cdr b))))))

(defun forecast-wordcount--bar (count width)
  "A COUNT-scaled bar at most WIDTH columns wide."
  (make-string (max 1 (min width count)) ?#))

(defun forecast-wordcount--row (pair width)
  "One report row for PAIR."
  (format "%6d  %-20s %s"
          (cdr pair)
          (car pair)
          (forecast-wordcount--bar (cdr pair) width)))

(defun forecast-wordcount-report (&optional limit)
  "Report the LIMIT most frequent words of the current buffer."
  (interactive)
  (let* ((pairs (forecast-wordcount--pairs
                 (forecast-wordcount--tally (forecast-wordcount--words))))
         (top (if limit (seq-take pairs limit) pairs))
         (total (apply '+ (mapcar 'cdr pairs)))
         (text (string-join
                (append (mapcar (lambda (p) (forecast-wordcount--row p 40))
                                top)
                        (list (format "%6d  %s" total "TOTAL")))
                "\n")))
    (with-current-buffer (get-buffer-create forecast-wordcount-buffer)
      (erase-buffer)
      (insert text)
      (goto-char (point-min)))
    (message "%d distinct word(s)" (length pairs))))

(defun forecast-wordcount-of (word)
  "How often WORD occurs in the current buffer."
  (interactive)
  (let ((count (or (cdr (assoc (downcase word)
                               (forecast-wordcount--tally-alist
                                (forecast-wordcount--words))))
                   0)))
    (message "%s: %d" word count)
    count))

(defun forecast-wordcount-ratio (word)
  "WORD's share of the buffer's counted words, as a percentage."
  (let* ((counts (forecast-wordcount--tally-alist
                  (forecast-wordcount--words)))
         (total (apply '+ (mapcar 'cdr counts)))
         (hits (or (cdr (assoc (downcase word) counts)) 0)))
    (if (zerop total)
        0
      (/ (* 100.0 hits) total))))

(provide 'forecast-wordcount)
;;; forecast-wordcount.el ends here
