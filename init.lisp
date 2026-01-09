
(define open-parenthesis (lambda() (
                                   (editor-insert-char "(")
                                   (editor-insert-char ")")
                                   (move-cursor "left")
                                   )
                           )
  )

(define open-brackets (lambda() (
                                   (editor-insert-char "[")
                                   (editor-insert-char "]")
                                   (move-cursor "left")
                                   )
                           )
  )

(define open-curly-brackets (lambda() (
                                   (editor-insert-char "{")
                                   (editor-insert-char "}")
                                   (move-cursor "left")
                                   )
                           )
  )

(define open-single-quotes (lambda() (
                                   (editor-insert-char "'")
                                   (editor-insert-char "'")
                                   (move-cursor "left")
                                   )
                           )
  )

(define open-double-quotes (lambda() (
                                   (editor-insert-char "\"")
                                   (editor-insert-char "\"")
                                   (move-cursor "left")
                                   )
                           )
  )

(define open-chevrons (lambda() (
                                   (editor-insert-char "<")
                                   (editor-insert-char ">")
                                   (move-cursor "left")
                                   )
                           )
  )


(map-key "(" open-parenthesis "no-prefix")
(map-key "[" open-brackets "no-prefix")
(map-key "{" open-curly-brackets "no-prefix")
(map-key "'" open-single-quotes "no-prefix")
(map-key "\"" open-double-quotes "no-prefix")
(map-key "<" open-chevrons "no-prefix")
