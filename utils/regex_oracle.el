;;; regex_oracle.el --- Emacs side of the regex differential test -*- lexical-binding: t -*-

;; Reads "<pattern>\t<subject>" lines from stdin and writes exactly one
;; result line per input line, in the protocol test/regex_differential.c
;; also speaks:
;;
;;	badpat			the pattern was rejected
;;	nomatch			no match anywhere in the subject
;;	match N s0 e0 s1 e1 ...	N spans as BYTE offsets, group 0 first,
;;				-1 -1 for a group that did not participate
;;
;; THE TRAP: `string-match' and `match-data' speak CHARACTER offsets, so
;; every offset is converted to a byte offset before it is printed.  kg
;; reports byte offsets natively; comparing the two without this conversion
;; makes every multi-byte subject look like a divergence.
;;
;; Run as: emacs -Q --batch -l utils/regex_oracle.el < cases

(set-language-environment "UTF-8")
(set-default-coding-systems 'utf-8-unix)
(setq case-fold-search nil)

(defun kg-regex-oracle-byte-offset (subject n)
  "Byte offset in SUBJECT of character offset N."
  (string-bytes (substring subject 0 n)))

(defun kg-regex-oracle-spans (subject)
  "Format the current match data against SUBJECT as a result line."
  (let ((data (match-data t))
	(spans nil))
    (while data
      (let ((beg (pop data))
	    (end (pop data)))
	(push (if (and beg end)
		  (format "%d %d"
			  (kg-regex-oracle-byte-offset subject beg)
			  (kg-regex-oracle-byte-offset subject end))
		;; A group that did not participate; kg spells it the same.
		"-1 -1")
	      spans)))
    (setq spans (nreverse spans))
    (format "match %d %s" (length spans) (mapconcat #'identity spans " "))))

(defun kg-regex-oracle-run (line)
  "Answer one tab-separated LINE of the differential protocol."
  (let* ((tab (string-search "\t" line))
	 (pattern (and tab (substring line 0 tab)))
	 (subject (and tab (substring line (1+ tab)))))
    (cond
     ((null tab) "badinput")
     (t (condition-case nil
	    (if (string-match pattern subject)
		(kg-regex-oracle-spans subject)
	      "nomatch")
	  (error "badpat"))))))

(let (line)
  (while (setq line (ignore-errors (read-string "")))
    (princ (kg-regex-oracle-run line))
    (princ "\n")))
