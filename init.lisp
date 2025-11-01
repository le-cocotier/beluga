
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



(map-key "(" open-parenthesis)
(map-key "[" open-brackets)
(map-key "{" open-curly-brackets)
(map-key "'" open-single-quotes)
(map-key "\"" open-double-quotes)
