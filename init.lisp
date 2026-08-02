;; ─── Theme: dark ────────────────────────────────────────────────────────────
;; (add-package "theme-dark") — beluga's original default palette, now
;; just an ordinary package instead of a hardcoded C if/else. Fork this
;; file (or write your own from scratch) to build a custom theme: seven
;; (editor-set-theme-color role r g b) calls is the whole interface.
;;
;; Roles: "background", "keyword", "type", "string", "comment", "number",
;; "default" (everything else — plain text, punctuation, identifiers).

(editor-set-theme-color "background" 40 44 52)
(editor-set-theme-color "keyword" 198 120 221)
(editor-set-theme-color "type" 97 175 239)
(editor-set-theme-color "string" 152 195 121)
(editor-set-theme-color "comment" 92 99 112)
(editor-set-theme-color "number" 209 154 102)
(editor-set-theme-color "default" 171 178 191)
