((nil . ((eval . (let ((utils-dir (expand-file-name "utils" dir-locals-directory)))
                   (unless (member utils-dir load-path)
                     (add-to-list 'load-path utils-dir))))
          (eval . (require 'kg-pty-recorder))
          (eval . (local-set-key (kbd "C-c r") 'kg-pty-test-record)))))

