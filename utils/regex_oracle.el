;;; regex_oracle.el --- Emacs side of the regex differential test -*- lexical-binding: t -*-

;; Reads "<mode>\t<pattern>\t<subject>" lines from stdin and writes exactly
;; one result line per input line, in the protocol test/regex_differential.c
;; also speaks.  The modes are:
;;
;;	f	the first match from offset 0
;;	fa	every successive match, iterated the way a caller must
;;
;; and the answers are:
;;
;;	badpat			the pattern was rejected
;;	nomatch			no match anywhere in the subject (mode f)
;;	match N s0 e0 s1 e1 ...	N spans as BYTE offsets, group 0 first,
;;				-1 -1 for a group that did not participate
;;	matches K <match>...	mode fa: K matches, each spelled as the
;;				"N s0 e0 ..." of a match line
;;
;; THE TRAP: `string-match' and `match-data' speak CHARACTER offsets, so
;; every offset is converted to a byte offset before it is printed.  kg
;; reports byte offsets natively; comparing the two without this conversion
;; makes every multi-byte subject look like a divergence.
;;
;; Mode fa iterates with the same rule kg's callers must use: continue at
;; the match end when it consumed something, at the next character when it
;; was empty, and stop at an empty match at the end of the subject.  That
;; rule is the whole reason the mode exists -- "end + 1" both skips
;; characters and, on an empty match, never terminates.
;;
;; Run as: emacs -Q --batch -l utils/regex_oracle.el < cases

(set-language-environment "UTF-8")
(set-default-coding-systems 'utf-8-unix)
(setq case-fold-search nil)

;; Both sides stop after this many matches, so one pathological case
;; cannot make a megabyte of output.  Must equal MAX_REPORTED in
;; test/regex_differential.c.
(defconst kg-regex-oracle-max-reported 32)

(defun kg-regex-oracle-byte-offset (subject n)
  "Byte offset in SUBJECT of character offset N."
  (string-bytes (substring subject 0 n)))

(defun kg-regex-oracle-spans (subject)
  "Format the current match data against SUBJECT as \"N s0 e0 ...\"."
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
    (format "%d %s" (length spans) (mapconcat #'identity spans " "))))

(defun kg-regex-oracle-forward (pattern subject)
  "The first match of PATTERN in SUBJECT, as a result line."
  (if (string-match pattern subject)
      (concat "match " (kg-regex-oracle-spans subject))
    "nomatch"))

(defun kg-regex-oracle-forward-all (pattern subject)
  "Every successive match of PATTERN in SUBJECT, as a result line."
  (let ((offset 0)
	(len (length subject))
	(found nil))
    (while (and offset
		(<= offset len)
		(< (length found) kg-regex-oracle-max-reported)
		(string-match pattern subject offset))
      (let ((beg (match-beginning 0))
	    (end (match-end 0)))
	(push (kg-regex-oracle-spans subject) found)
	(setq offset
	      (cond ((> end beg) end)
		    ;; An empty match consumed nothing, so the scan steps
		    ;; over a character itself; at the end of the subject
		    ;; there is none and the walk is over.
		    ((>= beg len) nil)
		    (t (1+ beg))))))
    (setq found (nreverse found))
    (format "matches %d%s" (length found)
	    (if found (concat " " (mapconcat #'identity found " ")) ""))))

(defun kg-regex-oracle-run (line)
  "Answer one tab-separated LINE of the differential protocol."
  (let* ((mode-end (string-search "\t" line))
	 (mode (and mode-end (substring line 0 mode-end)))
	 (rest (and mode-end (substring line (1+ mode-end))))
	 (tab (and rest (string-search "\t" rest)))
	 (pattern (and tab (substring rest 0 tab)))
	 (subject (and tab (substring rest (1+ tab)))))
    (cond
     ((null tab) "badinput")
     (t (condition-case nil
	    (cond
	     ((equal mode "f") (kg-regex-oracle-forward pattern subject))
	     ((equal mode "fa") (kg-regex-oracle-forward-all pattern subject))
	     (t "badinput"))
	  (error "badpat"))))))

(let (line)
  (while (setq line (ignore-errors (read-string "")))
    (princ (kg-regex-oracle-run line))
    (princ "\n")))
