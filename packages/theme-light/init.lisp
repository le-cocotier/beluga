;; ─── Theme: light ───────────────────────────────────────────────────────────
;; (add-package "theme-light") — beluga's original light palette, now
;; just an ordinary package instead of a hardcoded C if/else. Fork this
;; file (or write your own from scratch) to build a custom theme: seven
;; (editor-set-theme-color role r g b) calls is the whole interface.
;;
;; Roles: "background", "keyword", "type", "string", "comment", "number",
;; "default" (everything else — plain text, punctuation, identifiers).

(editor-set-theme-color "background" 250 251 252)
(editor-set-theme-color "keyword" 166 38 164)
(editor-set-theme-color "type" 4 90 180)
(editor-set-theme-color "string" 80 161 79)
(editor-set-theme-color "comment" 152 152 152)
(editor-set-theme-color "number" 206 102 54)
(editor-set-theme-color "default" 86 89 90)
