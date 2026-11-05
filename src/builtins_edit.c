/**
 * @file builtins_edit.c
 * @brief Lisp builtins that read and change buffer text directly
 * @details Until these, a package could only edit through the keystroke-
 * level builtins (editor-insert-char, editor-insert-new-line, kill and
 * paste), one character at a time. Positions are 0-based (row col), like
 * cursor-row / cursor-col / line-text; rows and columns past the end are
 * clamped. Every edit goes through bufferApplyTextEdit(): it is recorded
 * for undo (one step per call), marks the buffer modified and reaches the
 * language server like typing does. Read-only buffers are left alone.
 */

#include "../include/builtins.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/galloc.h"
#include "../include/split_screen.h"
#include "../include/syntax_highlighter.h"

#include <string.h>

extern EditorConfig E;

static struct buffer_t *activeBuffer(void) {
  const EditorPane *active = splitScreenGetActivePane();
  return active ? bufferFindById(active->buffer_id) : NULL;
}

/**
 * @brief The `index`-th argument as an int.
 * @return 1, or 0 (with *e set) when it's missing or not an integer.
 */
static int argInt(Lisp args, int index, int *out, LispError *e) {
  Lisp a = args;
  while (index-- > 0 && lisp_is_pair(a))
    a = lisp_cdr(a);
  if (!lisp_is_pair(a) || lisp_type(lisp_car(a)) != LISP_INT) {
    *e = LISP_ERROR_ARG_TYPE;
    return 0;
  }
  *out = (int)lisp_int(lisp_car(a));
  return 1;
}

static int argString(Lisp args, int index, const char **out, LispError *e) {
  Lisp a = args;
  while (index-- > 0 && lisp_is_pair(a))
    a = lisp_cdr(a);
  if (!lisp_is_pair(a) || lisp_type(lisp_car(a)) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return 0;
  }
  *out = lisp_string(lisp_car(a));
  return 1;
}

/**
 * @brief Clamps (row, col) into `buf`.
 */
static void clampPosition(const struct buffer_t *buf, int *row, int *col) {
  if (buf->numrows == 0) {
    *row = 0;
    *col = 0;
    return;
  }
  if (*row < 0)
    *row = 0;
  if (*row >= buf->numrows)
    *row = buf->numrows - 1;
  if (*col < 0)
    *col = 0;
  if (*col > buf->row[*row].size)
    *col = buf->row[*row].size;
}

/**
 * @brief Puts (r1, c1)–(r2, c2) in document order.
 */
static void orderRange(int *r1, int *c1, int *r2, int *c2) {
  int t;
  if (*r1 > *r2 || (*r1 == *r2 && *c1 > *c2)) {
    t = *r1; *r1 = *r2; *r2 = t;
    t = *c1; *c1 = *c2; *c2 = t;
  }
}

static int editable(const struct buffer_t *buf) {
  return buf && buf->state != READ_ONLY && buf->type != TERMINAL_BUFF;
}

/**
 * @brief Replaces a range and leaves the cursor after the new text, as
 * one undo step.
 */
static void replaceRange(struct buffer_t *buf, int r1, int c1, int r2, int c2,
                         const char *text) {
  Utf8Char empty[1];
  if (buf->numrows == 0) {
    // bufferApplyTextEdit() works on existing rows.
    memset(empty, 0, sizeof(empty));
    bufferInsertRow(buf, 0, empty, 0);
  }
  clampPosition(buf, &r1, &c1);
  clampPosition(buf, &r2, &c2);
  orderRange(&r1, &c1, &r2, &c2);
  // A fresh group: the whole call undoes at once, and doesn't merge into
  // what was typed just before.
  buf->undo_group_open = 0;
  bufferApplyTextEdit(buf, r1, c1, r2, c2, text);
  buf->undo_group_open = 0;
  syntaxHighlighterInvalidateAfter(buf, r1 - 1);
}

/**
 * @brief (buffer-insert-string text) — inserts `text` (newlines allowed) at
 * the cursor of the current buffer; the cursor ends up after it.
 */
Lisp l_bufferInsertString(Lisp args, LispError *e, LispContext ctx) {
  struct buffer_t *buf = activeBuffer();
  const char *text;
  (void)ctx;
  if (!argString(args, 0, &text, e))
    return lisp_null();
  if (!editable(buf))
    return lisp_null();
  replaceRange(buf, buf->y, buf->x, buf->y, buf->x, text);
  return lisp_null();
}

/**
 * @brief (buffer-delete-range row1 col1 row2 col2) — deletes the text
 * between the two positions (either order); the cursor ends up there.
 */
Lisp l_bufferDeleteRange(Lisp args, LispError *e, LispContext ctx) {
  struct buffer_t *buf = activeBuffer();
  int r1, c1, r2, c2;
  (void)ctx;
  if (!argInt(args, 0, &r1, e) || !argInt(args, 1, &c1, e) ||
      !argInt(args, 2, &r2, e) || !argInt(args, 3, &c2, e))
    return lisp_null();
  if (!editable(buf) || buf->numrows == 0)
    return lisp_null();
  replaceRange(buf, r1, c1, r2, c2, "");
  return lisp_null();
}

/**
 * @brief (buffer-replace-range row1 col1 row2 col2 text) — replaces the
 * text between the two positions with `text`; the cursor ends up after it.
 */
Lisp l_bufferReplaceRange(Lisp args, LispError *e, LispContext ctx) {
  struct buffer_t *buf = activeBuffer();
  int r1, c1, r2, c2;
  const char *text;
  (void)ctx;
  if (!argInt(args, 0, &r1, e) || !argInt(args, 1, &c1, e) ||
      !argInt(args, 2, &r2, e) || !argInt(args, 3, &c2, e) ||
      !argString(args, 4, &text, e))
    return lisp_null();
  if (!editable(buf))
    return lisp_null();
  replaceRange(buf, r1, c1, r2, c2, text);
  return lisp_null();
}

/**
 * @brief (buffer-text) — the whole current buffer as one string, lines
 * joined with "\n"; (buffer-text row1 col1 row2 col2) — just that range.
 */
Lisp l_bufferText(Lisp args, LispError *e, LispContext ctx) {
  struct buffer_t *buf = activeBuffer();
  int r1 = 0, c1 = 0, r2, c2;
  char *text;
  Lisp result;
  if (!buf || buf->numrows == 0)
    return lisp_make_string2("", ctx);

  r2 = buf->numrows - 1;
  c2 = buf->row[r2].size;
  if (lisp_is_pair(args) &&
      (!argInt(args, 0, &r1, e) || !argInt(args, 1, &c1, e) ||
       !argInt(args, 2, &r2, e) || !argInt(args, 3, &c2, e)))
    return lisp_null();
  clampPosition(buf, &r1, &c1);
  clampPosition(buf, &r2, &c2);
  orderRange(&r1, &c1, &r2, &c2);

  text = bufferGetRegionText(buf, r1, c1, r2, c2);
  result = lisp_make_string2(text ? text : "", ctx);
  gfree(text);
  return result;
}

/**
 * @brief (set-cursor row col) — moves the cursor of the current buffer
 * (clamped to the text); the view follows on the next redraw.
 */
Lisp l_setCursor(Lisp args, LispError *e, LispContext ctx) {
  struct buffer_t *buf = activeBuffer();
  int row, col;
  (void)ctx;
  if (!argInt(args, 0, &row, e) || !argInt(args, 1, &col, e))
    return lisp_null();
  if (!buf)
    return lisp_null();
  clampPosition(buf, &row, &col);
  buf->y = row;
  buf->x = col;
  return lisp_null();
}

/**
 * @brief (region-bounds) — (row1 col1 row2 col2) of the selection, in
 * document order, or #f when no mark is set.
 */
Lisp l_regionBounds(Lisp args, LispError *e, LispContext ctx) {
  struct buffer_t *buf = activeBuffer();
  int r1, c1, r2, c2;
  (void)args;
  (void)e;
  if (!buf || !buf->mark_active)
    return lisp_make_bool(0);
  bufferRegionBounds(buf, &r1, &c1, &r2, &c2);
  return lisp_cons(lisp_make_int(r1),
                   lisp_cons(lisp_make_int(c1),
                             lisp_cons(lisp_make_int(r2),
                                       lisp_cons(lisp_make_int(c2),
                                                 lisp_null(), ctx),
                                       ctx),
                             ctx),
                   ctx);
}

/**
 * @brief (buffer-local-set name value) — stores `value` under the symbol
 * or string `name` for the current buffer only; (buffer-local name
 * [default]) reads it back, `default` (or #f) when unset. For packages
 * that keep per-buffer state (a mode, a cached path) without a global
 * table keyed by buffer id.
 */
Lisp l_bufferLocalSet(Lisp args, LispError *e, LispContext ctx) {
  struct buffer_t *buf = activeBuffer();
  Lisp name;
  Lisp value;
  Lisp p;
  if (!lisp_is_pair(args) || !lisp_is_pair(lisp_cdr(args))) {
    *e = LISP_ERROR_TOO_FEW_ARGS;
    return lisp_null();
  }
  name = lisp_car(args);
  value = lisp_car(lisp_cdr(args));
  if (lisp_type(name) == LISP_STRING)
    name = lisp_make_symbol(lisp_string(name), ctx);
  if (lisp_type(name) != LISP_SYMBOL) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  if (!buf)
    return lisp_null();

  // buf->locals is a collector root (lisp_gc.c), so it can live in C memory.
  for (p = buf->locals; lisp_is_pair(p); p = lisp_cdr(p))
    if (lisp_equal(lisp_car(lisp_car(p)), name)) {
      lisp_set_cdr(lisp_car(p), value);
      return value;
    }
  buf->locals = lisp_cons(lisp_cons(name, value, ctx), buf->locals, ctx);
  return value;
}

Lisp l_bufferLocal(Lisp args, LispError *e, LispContext ctx) {
  struct buffer_t *buf = activeBuffer();
  Lisp name;
  Lisp p;
  Lisp fallback = lisp_make_bool(0);
  if (!lisp_is_pair(args)) {
    *e = LISP_ERROR_TOO_FEW_ARGS;
    return lisp_null();
  }
  name = lisp_car(args);
  if (lisp_is_pair(lisp_cdr(args)))
    fallback = lisp_car(lisp_cdr(args));
  if (lisp_type(name) == LISP_STRING)
    name = lisp_make_symbol(lisp_string(name), ctx);
  if (!buf)
    return fallback;
  for (p = buf->locals; lisp_is_pair(p); p = lisp_cdr(p))
    if (lisp_equal(lisp_car(lisp_car(p)), name))
      return lisp_cdr(lisp_car(p));
  return fallback;
}
