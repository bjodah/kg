;;; forecast-snippet.el --- sketch: a tiny templating package  -*- lexical-binding: t -*-
;;
;; A FORECAST SKETCH.  Hand-written for utils/forecast_audit.py; nothing
;; loads, installs or evaluates this file.  Written the way it would be
;; written for Emacs, so the audit measures demand and not memory.
;;
;; What it would do: expand `${NAME}' placeholders in a stored template
;; from an alist of values, with per-template defaults kept on a plist,
;; and insert the result at point.

(defvar forecast-snippet-table nil
  "Alist of (NAME . TEMPLATE-STRING).")

(defvar forecast-snippet-defaults nil
  "Alist of (NAME . PLIST), the per-template default field values.")

(defvar forecast-snippet-open "${"
  "Placeholder opener.")

(defvar forecast-snippet-close "}"
  "Placeholder closer.")

(defun forecast-snippet-define (name template &rest defaults)
  "Store TEMPLATE under NAME with DEFAULTS as its field plist."
  (setq forecast-snippet-table
        (cons (cons name template)
              (assq-delete-all name forecast-snippet-table)))
  (setq forecast-snippet-defaults
        (cons (cons name defaults)
              (assq-delete-all name forecast-snippet-defaults)))
  name)

(defun forecast-snippet--template (name)
  "TEMPLATE stored under NAME, or nil."
  (alist-get name forecast-snippet-table))

(defun forecast-snippet--default (name field)
  "The default for FIELD of template NAME."
  (plist-get (alist-get name forecast-snippet-defaults) field))

(defun forecast-snippet--placeholder-re ()
  "A regexp matching one placeholder, whatever the delimiters are."
  (concat (regexp-quote forecast-snippet-open)
          "\\([a-zA-Z0-9-]+\\)"
          (regexp-quote forecast-snippet-close)))

(defun forecast-snippet--fields (template)
  "Every field name mentioned in TEMPLATE, in order, without repeats."
  (let ((re (forecast-snippet--placeholder-re))
        (start 0)
        (found nil))
    (while (string-match re template start)
      (let ((field (intern (match-string 1 template))))
        (unless (memq field found)
          (push field found)))
      (setq start (match-end 0)))
    (nreverse found)))

(defun forecast-snippet--value (name field values)
  "FIELD's value: VALUES first, then NAME's defaults, then \"\"."
  (let ((given (alist-get field values)))
    (cond (given given)
          ((forecast-snippet--default name field)
           (forecast-snippet--default name field))
          (t ""))))

(defun forecast-snippet-expand (name values)
  "Expand template NAME with the alist VALUES."
  (let ((template (forecast-snippet--template name)))
    (unless template
      (error "No such snippet: %s" name))
    (let ((text template))
      (dolist (field (forecast-snippet--fields template))
        (setq text
              (replace-regexp-in-string
               (concat (regexp-quote forecast-snippet-open)
                       (regexp-quote (symbol-name field))
                       (regexp-quote forecast-snippet-close))
               (forecast-snippet--value name field values)
               text)))
      text)))

(defun forecast-snippet--indent-to-point (text)
  "TEXT with every line but the first indented to the current column."
  (let ((pad (make-string (current-column) ?\s)))
    (string-join (split-string text "\n") (concat "\n" pad))))

(defun forecast-snippet-insert (name)
  "Insert template NAME at point, indented to the current column."
  (interactive)
  (insert (forecast-snippet--indent-to-point
           (forecast-snippet-expand name nil))))

(defun forecast-snippet-names ()
  "Every defined snippet name, sorted."
  (sort (mapcar 'car forecast-snippet-table) 'string<))

(defun forecast-snippet-describe (name)
  "A one-line summary of template NAME."
  (let ((fields (forecast-snippet--fields (forecast-snippet--template name))))
    (format "%s: %d field(s): %s"
            name
            (length fields)
            (mapconcat 'symbol-name fields ", "))))

(forecast-snippet-define
 'defun-c
 "static ${type} ${name}(${args})\n{\n\t${body}\n}\n"
 :type "void" :args "void" :body "return;")

(provide 'forecast-snippet)
;;; forecast-snippet.el ends here
