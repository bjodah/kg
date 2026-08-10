;;; target-init.el --- a realistic kg init file  -*- lexical-binding: t -*-
;;
;; Corpus file for utils/forecast_audit.py.  Hand-written, not vendored.
;; This is the corpus's FLOOR: it must load clean under test/kgbatch
;; (`make forecast-init-check'), so every name it calls has to exist.
;; The forecast-*.el sketches beside it are the ceiling and deliberately
;; do not load.
;;
;; What a user's ~/.config/kg/init.el is meant to look like: preferences,
;; two small commands, a hook, key bindings, and one mode tweak.

;;; --- preferences ---------------------------------------------------

(setq inhibit-startup-screen t)

(defvar my-notes-buffer "*notes*"
  "Buffer `my-open-notes' switches to.")

(defvar my-boring-suffixes '(".o" ".gcno" ".gcda" "~")
  "File-name endings `my-boring-name-p' rejects.")

(defvar my-heading-words nil
  "The heading `my-open-notes' inserts, computed at load time.")

;;; --- small helpers -------------------------------------------------

(defun my-boring-name-p (name)
  "Return t when NAME ends in one of `my-boring-suffixes'."
  (let ((rest my-boring-suffixes)
        (hit nil))
    (while (and rest (not hit))
      (when (string-suffix-p (car rest) name)
        (setq hit t))
      (setq rest (cdr rest)))
    hit))

(defun my-title-case (text)
  "Capitalize every whitespace-separated word of TEXT."
  (string-join (mapcar 'capitalize (split-string text)) " "))

(defun my-interesting-names (names)
  "NAMES without the boring ones, in order."
  (sort (seq-filter (lambda (n) (not (my-boring-name-p n))) names)
        'string<))

(setq my-heading-words (my-title-case "  kg session notes  "))

;;; --- commands ------------------------------------------------------

(defun my-open-notes ()
  "Switch to the notes buffer, appending a heading."
  (interactive)
  (with-current-buffer (get-buffer-create my-notes-buffer)
    (goto-char (point-max))
    (insert (format "* %s\n" my-heading-words)))
  (message "notes: %s" my-notes-buffer))

(defun my-insert-rule (&optional width)
  "Insert a rule WIDTH columns wide, 72 by default."
  (interactive)
  (let ((n (if width width 72))
        (rule ""))
    (while (< 0 n)
      (setq rule (concat rule "-"))
      (setq n (1- n)))
    (insert rule)
    (insert "\n")))

(defun my-report-widths (names)
  "Message NAMES with their lengths, longest first."
  (interactive)
  (message "%s"
           (mapconcat (lambda (n) (format "%s:%d" n (length n)))
                      (sort names (lambda (a b) (> (length a) (length b))))
                      ", ")))

;;; --- a hook --------------------------------------------------------

(defun my-announce-save ()
  "Say which buffer was just written."
  (message "wrote %s" (buffer-name)))

(add-hook 'after-save-hook 'my-announce-save)

;;; --- key bindings --------------------------------------------------

(global-set-key (kbd "C-c n") 'my-open-notes)
(global-set-key (kbd "C-c -") 'my-insert-rule)

;;; --- a mode tweak --------------------------------------------------
;; Dired gets the notes command on a key of its own; `define-key' takes
;; any sequence, unlike `global-set-key'.

(define-key 'dired-mode-map "C-c o" 'my-open-notes)

;;; target-init.el ends here
