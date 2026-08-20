;;; regex_oracle.el --- Emacs side of the regex differential test -*- lexical-binding: t -*-

;; Reads "<mode>\t<pattern>\t<subject>" lines from stdin and writes exactly
;; one result line per input line, in the protocol test/regex_differential.c
;; also speaks.  The modes are:
;;
;;	f	the first match from offset 0
;;	fa	every successive match, iterated the way a caller must
;;	fb	the first match from offset 0 under a MATCH WINDOW
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
;; Mode fb is mode f under a bounded window, which is what Emacs' search
;; BOUND is.  THE LIMIT IS NOT IN THE PROTOCOL: both sides derive it from
;; the subject as half its length in BYTES, so a case line means the same
;; thing in every mode.  Two conversions make that comparable here:
;;
;;   * `string-match' takes no bound at all, so this mode asks
;;     `re-search-forward' in a temp buffer, where BOUND exists.  Buffer
;;     positions are 1-based, so every offset comes back through
;;     (- pos (point-min)).  Subjects are single-line by this harness's
;;     own policy, which is what makes a buffer's `^'/`$' (line anchors)
;;     and a string's (subject anchors here) pick the same offsets.
;;   * BOUND is a CHARACTER position and the limit is a byte count, so it
;;     is the largest character offset whose byte offset does not exceed
;;     the limit.  That is exact rather than an approximation: every match
;;     end is on a character boundary, so "ends at or before the byte
;;     limit" and "ends at or before that character" name the same set.
;;
;; A limit is NOT a shorter subject -- narrowing or truncating the buffer
;; would make `\'` hold at BOUND, and kg's engine deliberately does not --
;; so the buffer here always holds the whole subject.
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

(defun kg-regex-oracle-spans-from (subject base)
  "Format the current match data against SUBJECT as \"N s0 e0 ...\".
BASE is subtracted from every offset first: 0 for `string-match\='s
character offsets, (point-min) for a buffer search\='s positions."
  (let ((data (match-data t))
	(spans nil))
    (while data
      (let ((beg (pop data))
	    (end (pop data)))
	(push (if (and beg end)
		  (format "%d %d"
			  (kg-regex-oracle-byte-offset subject (- beg base))
			  (kg-regex-oracle-byte-offset subject (- end base)))
		;; A group that did not participate; kg spells it the same.
		"-1 -1")
	      spans)))
    (setq spans (nreverse spans))
    (format "%d %s" (length spans) (mapconcat #'identity spans " "))))

(defun kg-regex-oracle-spans (subject)
  "Format the current `string-match\=' data against SUBJECT."
  (kg-regex-oracle-spans-from subject 0))

(defun kg-regex-oracle-char-limit (subject limit-bytes)
  "Largest character offset in SUBJECT within LIMIT-BYTES bytes."
  (let ((n 0)
	(len (length subject)))
    (while (and (< n len)
		(<= (string-bytes (substring subject 0 (1+ n))) limit-bytes))
      (setq n (1+ n)))
    n))

(defun kg-regex-oracle-forward (pattern subject)
  "The first match of PATTERN in SUBJECT, as a result line."
  (if (string-match pattern subject)
      (concat "match " (kg-regex-oracle-spans subject))
    "nomatch"))

(defun kg-regex-oracle-forward-bounded (pattern subject)
  "The first match of PATTERN in SUBJECT under this mode\='s window."
  (let ((chars (kg-regex-oracle-char-limit
		subject (/ (string-bytes subject) 2))))
    (with-temp-buffer
      (set-buffer-multibyte t)
      (setq-local case-fold-search nil)
      (insert subject)
      (goto-char (point-min))
      (if (re-search-forward pattern (+ (point-min) chars) t)
	  (concat "match " (kg-regex-oracle-spans-from subject (point-min)))
	"nomatch"))))

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
	     ((equal mode "fb")
	      (kg-regex-oracle-forward-bounded pattern subject))
	     (t "badinput"))
	  (error "badpat"))))))

(let (line)
  (while (setq line (ignore-errors (read-string "")))
    (princ (kg-regex-oracle-run line))
    (princ "\n")))
