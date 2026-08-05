;;; auto-fill.el --- proof package for kg's require/load-path work.
;;;
;;; Provides `auto-fill-mode', a buffer-local `after-change-functions'
;;; hook that breaks a line once it grows past `fill-column', the way
;;; Emacs' auto-fill-mode does for the common case of typing past the
;;; margin.  Deliberately narrow, and does not try to be Emacs' fill
;;; engine: it reacts to insertions only, breaks at the last space at or
;;; before the column, and leaves a line alone when it holds no space to
;;; break at -- there is no fill-prefix, no paragraph awareness and no
;;; re-filling of a whole paragraph.
;;;
;;; Load it with (require 'auto-fill) and turn it on per buffer with
;;; (auto-fill-mode), e.g. from `find-file-hook' or an init file.

(provide 'auto-fill)

(defvar fill-column 70
  "Column auto-fill-mode tries to keep lines at or under.")

;; The display column of buffer position POS.  Moves point to get it --
;; current-column has no by-position form -- so every caller wraps its
;; own use of this in save-excursion; it is not safe to call bare.  Fe has
;; had `>' since sub-plan 05C, but every comparison in this file stays
;; written as a flipped `<': the package is the byte-for-byte source that
;; test/pty/lisp-auto-fill-mode-break.yaml plants in its own HOME, so
;; respelling the code here would have to be a matched edit in the case
;; for no behavioural gain.
(defun auto-fill--column-at (pos)
  (goto-char pos)
  (current-column))

;; The last space in [FROM, TO) whose own column is at or before
;; fill-column, or nil when there is none -- a single word longer than
;; fill-column is left alone, the same as Emacs when it finds no earlier
;; break point.  save-excursion undoes every goto-char the scan (via
;; auto-fill--column-at) makes along the way, so the caller's point is
;; exactly where it found it.
(defun auto-fill--find-break (from to)
  (save-excursion
    (let ((pos from) (break-pos nil))
      (while (and (< pos to) (<= (auto-fill--column-at pos) fill-column))
        (if (eq (char-after pos) 32)
            (setq break-pos pos))
        (setq pos (1+ pos)))
      break-pos)))

;; after-change-functions is called (buf start end old-len); react only
;; to insertions (start < end), and only when the insertion pushed point
;; past fill-column.  The break itself is a single (replace-region ...)
;; call -- the space becomes a newline -- so it is one undo step, never a
;; delete followed by an insert.
;; save-excursion wraps the whole check, not just auto-fill--find-break's
;; own scan: auto-fill--column-at's goto-char is a query with a side
;; effect, and without this the *condition* alone -- (< fill-column
;; (auto-fill--column-at end)) -- already moves point to END and leaves
;; it there even when the answer is nil and nothing else in this function
;; runs.  END's own character position is never touched by the break
;; (replace-region only ever rewrites something strictly before it, one
;; byte for one byte), so restoring point to wherever it was on entry is
;; always the right place to leave it, break or no break.
(defun auto-fill--maybe-break (buf start end old-len)
  (when (< start end)
    (with-current-buffer buf
      (save-excursion
        (when (< fill-column (auto-fill--column-at end))
          (let* ((line (bounds-of-thing-at-point 'line))
                 (break-pos (and line (auto-fill--find-break (car line) end))))
            (when break-pos
              (replace-region break-pos (1+ break-pos) "\n"))))))))

(defun auto-fill-mode ()
  "Break lines in the current buffer at `fill-column' as they are typed
past it."
  (interactive)
  (add-hook 'after-change-functions 'auto-fill--maybe-break t))
