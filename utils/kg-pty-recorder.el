;;; kg-pty-recorder.el --- Record PTY tests for kg editor -*- lexical-binding: t -*-

;; Copyright (C) 2026 kg contributors
;;
;; Author: kg contributors
;; Keywords: testing, tools

;;; Commentary:

;; This package provides a wizard-like experience to generate PTY test YAML files
;; for the kg editor.
;;
;; Run `M-x kg-pty-recorder-start` to begin.

;;; Code:

(require 'cl-lib)

(defvar kg-pty-recorder-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map (kbd "C-c C-c") 'kg-pty-recorder-next-phase)
    map)
  "Keymap for `kg-pty-recorder-mode'.")

(define-minor-mode kg-pty-recorder-mode
  "Minor mode for recording kg PTY tests.
Press \\[kg-pty-recorder-next-phase] to transition phases."
  :init-value nil
  :lighter " kg-pty"
  :keymap kg-pty-recorder-mode-map)

(defvar-local kg-pty-recorder--phase 0)
(defvar-local kg-pty-recorder--initial-content "")
(defvar-local kg-pty-recorder--recorded-events nil)

;;;###autoload
(defun kg-pty-test-record ()
  "Start the kg PTY recorder wizard."
  (interactive)
  (let ((buf (get-buffer-create "*kg-pty-recorder*")))
    (switch-to-buffer buf)
    (kill-all-local-variables)
    (erase-buffer)
    (kg-pty-recorder-mode 1)
    (setq kg-pty-recorder--phase 1)
    (setq kg-pty-recorder--recorded-events nil)
    (setq header-line-format "kg PTY Recorder [Phase 1: Type Initial Content] -> Press C-c C-c when done")
    (message "Type the initial buffer contents. Press C-c C-c when done.")))

(defun kg-pty-recorder-next-phase ()
  "Transition to the next phase of the kg PTY recorder."
  (interactive)
  (cond
   ((= kg-pty-recorder--phase 1)
    ;; Enter phase 2
    (setq kg-pty-recorder--initial-content (buffer-string))
    (setq kg-pty-recorder--phase 2)
    (goto-char (point-min))
    (setq header-line-format "kg PTY Recorder [Phase 2: Recording Keys] -> Edit the buffer, press C-c C-c to finish")
    (add-hook 'post-command-hook #'kg-pty-recorder--post-command nil t)
    (message "Recording started. Press C-c C-c to finish."))
   ((= kg-pty-recorder--phase 2)
    ;; Finish recording
    (remove-hook 'post-command-hook #'kg-pty-recorder--post-command t)
    (setq kg-pty-recorder--phase 0)
    (setq header-line-format nil)
    (kg-pty-recorder-mode -1)
    (kg-pty-recorder-finish))))

(defun kg-pty-recorder--post-command ()
  "Record the keys that invoked the last command."
  (let ((vec (this-command-keys-vector)))
    (unless (or (= (length vec) 0)
                (string= (key-description vec) "C-c C-c"))
      (push vec kg-pty-recorder--recorded-events))))

(defun kg-pty-recorder--events-to-tokens (event-vectors)
  "Convert Emacs event vectors to kg YAML tokens."
  (let ((tokens nil))
    (dolist (vec event-vectors)
      (let ((i 0)
            (len (length vec)))
        (while (< i len)
          (let* ((ev (aref vec i))
                 (desc (key-description (vector ev))))
            (cond
             ((string= desc "DEL") (push "C-?" tokens))
             ((string= desc "C-@") (push "C-SPC" tokens))
             ((string= desc "RET") (push "RET" tokens))
             ((string= desc "SPC") (push "SPC" tokens))
             ((string= desc "TAB") (push "TAB" tokens))
             ;; Handle terminal M-x (ESC x)
             ((and (string= desc "ESC")
                   (< (1+ i) len)
                   (characterp (aref vec (1+ i)))
                   (= (length (key-description (vector (aref vec (1+ i))))) 1))
              (push (format "M-%s" (key-description (vector (aref vec (1+ i))))) tokens)
              (setq i (1+ i)))
             ((string= desc "ESC") (push "ESC" tokens))
             ;; Format simple keys
             ((= (length desc) 1) (push desc tokens))
             (t (push desc tokens)))
            (setq i (1+ i))))))
    (nreverse tokens)))

(defun kg-pty-recorder-finish ()
  "Finish recording and prompt to save the YAML file."
  (let* ((final-content (buffer-string))
         (tokens (kg-pty-recorder--events-to-tokens (reverse kg-pty-recorder--recorded-events)))
         (test-name (read-string "Test name (e.g. transpose-chars): "))
         (filename (read-string "Mock filename (e.g. test.txt): " "test.txt"))
         (default-dir (let ((root (locate-dominating-file default-directory "Makefile")))
                        (if root (expand-file-name "test/pty/" root) default-directory)))
         (yaml-file (read-file-name "Save YAML to: " default-dir nil nil (concat test-name ".yaml"))))
    (with-temp-file yaml-file
      (insert (format "name: %s\n" test-name))
      (insert (format "filename: %s\n" filename))
      (insert "initial: |\n")
      (kg-pty-recorder--insert-indented kg-pty-recorder--initial-content)
      (insert "keys:\n")
      (dolist (tok tokens)
        (if (string-match-p "^[A-Za-z][A-Za-z0-9_-]*$" tok)
            (insert (format "  - %s\n" tok))
          ;; Quote strings that have special meaning in yaml, e.g. " ", "[", "1", etc.
          (insert (format "  - %S\n" tok))))
      (insert "expected_saved: |\n")
      (kg-pty-recorder--insert-indented final-content))
    (message "Saved to %s" yaml-file)))

(defun kg-pty-recorder--insert-indented (str)
  "Insert STR with 2 spaces indentation."
  (if (string= str "")
      (insert "  \n")
    (let ((lines (split-string str "\n")))
      (when (and (> (length lines) 1) (string= (car (last lines)) ""))
        (setq lines (butlast lines)))
      (dolist (line lines)
        (insert (format "  %s\n" line))))))

(provide 'kg-pty-recorder)

;;; kg-pty-recorder.el ends here
