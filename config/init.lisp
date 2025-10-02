(define TAB-LENGTH 2)
(define QUIT-TIMES 1)

(map-key "CTRL-q" editor-quit)
(map-key "CTRL-s" editor-save)

(define editor-delete-next-char (lambda () (
                                            (move-cursor "right")
                                            (editor-delete-previous-char)
                                            )
                                  )
  )


(map-key "ARROW-UP" '(move-cursor "up"))
(map-key "ARROW-DOWN" '(move-cursor "down"))
(map-key "ARROW-RIGHT" '(move-cursor "right"))
(map-key "ARROW-LEFT" '(move-cursor "left"))
(map-key "ENTER" editor-insert-new-line)
(map-key "CTRL-a" move-cursor-beg-line)
(map-key "CTRL-e" move-cursor-end-line)
(map-key "BACKSPACE" editor-delete-previous-char)
(map-key "DEL" editor-delete-next-char)
(map-key "PAGE-UP" move-cursor-page-up)
(map-key "PAGE-DOWN" move-cursor-page-down)

; Key binds


