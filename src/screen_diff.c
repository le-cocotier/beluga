/**
 * @file screen_diff.c
 * @brief Writes only the screen rows that changed since the last frame
 * @details editorRefreshScreen() builds a complete frame of escape
 * sequences every tick. Sending all of it whenever one byte differs (the
 * cursor moved, a character was typed) repaints the whole terminal, which
 * flickers and costs bandwidth over SSH. Here the frame is replayed onto a
 * cell grid holding what the terminal currently shows; rows that end up
 * different are re-sent whole, followed by the cursor.
 *
 * The replay understands exactly what the editor emits: printable UTF-8,
 * CR/LF, cursor positioning and moves (CSI H f A B C D G), erase in line
 * and display (CSI K J), SGR colors and attributes, and cursor visibility
 * (CSI ?25 h/l). Anything else, or a frame that would scroll the
 * terminal, makes screenDiffWrite() send the frame unchanged and repaint
 * in full next time, so an unexpected sequence costs speed, never
 * correctness.
 *
 * Each code point is one cell, like everywhere else in the editor (see
 * the wide-character item in TODO.md). Rows are re-sent from column 0 in
 * the same order the frame drew them, so lines with wide characters look
 * the same as before.
 */

#include "../include/screen_diff.h"
#include "../include/append_buffer.h"
#include "../include/galloc.h"
#include "../include/terminal.h"

#include <stdio.h>
#include <string.h>

// SGR attribute bits.
#define A_BOLD 0x01
#define A_DIM 0x02
#define A_ITALIC 0x04
#define A_UNDERLINE 0x08
#define A_BLINK 0x10
#define A_REVERSE 0x20
#define A_HIDDEN 0x40
#define A_STRIKE 0x80

// A color: 0 is the terminal default, 1..16 one of the 16 basic colors
// + 1 (SGR 30-37 / 90-97, kept apart from the same index in the 256-color
// palette since terminals may draw bold basic colors brighter),
// COLOR_256 | n a palette index, and COLOR_RGB | 0xRRGGBB a 24-bit color.
#define COLOR_256 0x100u
#define COLOR_RGB 0x1000000u

typedef struct {
  unsigned int fg;
  unsigned int bg;
  unsigned char attrs;
} Sgr;

typedef struct {
  char glyph[4];
  unsigned char len;    // bytes in glyph
  unsigned char erased; // blank left by an erase, not a written space
  Sgr sgr;
} Cell;

typedef struct {
  Cell *cells;
  int rows;
  int cols;
  int row; // cursor
  int col;
  int cursor_visible;
  Sgr sgr; // current rendition
} Screen;

static Screen s_shown;    // what the terminal shows, when s_valid
static int s_valid = 0;
static Screen s_next;     // the frame being replayed

// A daemon window's s_shown, s_valid and s_next while another is drawn.
typedef struct {
  Screen shown;
  int valid;
  Screen next;
} SavedScreen;

static int sgrEqual(const Sgr *a, const Sgr *b) {
  return a->fg == b->fg && a->bg == b->bg && a->attrs == b->attrs;
}

static Cell *cellAt(Screen *s, int row, int col) {
  return &s->cells[row * s->cols + col];
}

static void eraseCells(Screen *s, int row, int from, int to) {
  Cell *c;
  int i;
  for (i = from; i < to; i++) {
    c = cellAt(s, row, i);
    c->glyph[0] = ' ';
    c->len = 1;
    c->erased = 1;
    c->sgr = s->sgr;
  }
}

static int screenAlloc(Screen *s, int rows, int cols) {
  Cell *cells;
  if (s->cells && s->rows == rows && s->cols == cols)
    return 1;
  cells = galloc((size_t)rows * (size_t)cols * sizeof(Cell));
  if (!cells)
    return 0;
  gfree(s->cells);
  s->cells = cells;
  s->rows = rows;
  s->cols = cols;
  return 1;
}

/**
 * @brief Applies the parameters of one SGR sequence to `sgr`.
 * @return 0 for a parameter this doesn't model.
 */
static int applySgr(Sgr *sgr, const int *p, int n) {
  int i;
  int v;
  unsigned int *target;
  if (n == 0) {
    memset(sgr, 0, sizeof(*sgr));
    return 1;
  }
  for (i = 0; i < n; i++) {
    v = p[i];
    if (v == 0)
      memset(sgr, 0, sizeof(*sgr));
    else if (v == 1)
      sgr->attrs |= A_BOLD;
    else if (v == 2)
      sgr->attrs |= A_DIM;
    else if (v == 3)
      sgr->attrs |= A_ITALIC;
    else if (v == 4)
      sgr->attrs |= A_UNDERLINE;
    else if (v == 5)
      sgr->attrs |= A_BLINK;
    else if (v == 7)
      sgr->attrs |= A_REVERSE;
    else if (v == 8)
      sgr->attrs |= A_HIDDEN;
    else if (v == 9)
      sgr->attrs |= A_STRIKE;
    else if (v == 22)
      sgr->attrs &= (unsigned char)~(A_BOLD | A_DIM);
    else if (v == 23)
      sgr->attrs &= (unsigned char)~A_ITALIC;
    else if (v == 24)
      sgr->attrs &= (unsigned char)~A_UNDERLINE;
    else if (v == 25)
      sgr->attrs &= (unsigned char)~A_BLINK;
    else if (v == 27)
      sgr->attrs &= (unsigned char)~A_REVERSE;
    else if (v == 28)
      sgr->attrs &= (unsigned char)~A_HIDDEN;
    else if (v == 29)
      sgr->attrs &= (unsigned char)~A_STRIKE;
    else if (v >= 30 && v <= 37)
      sgr->fg = (unsigned int)(v - 30) + 1;
    else if (v >= 90 && v <= 97)
      sgr->fg = (unsigned int)(v - 90 + 8) + 1;
    else if (v == 39)
      sgr->fg = 0;
    else if (v >= 40 && v <= 47)
      sgr->bg = (unsigned int)(v - 40) + 1;
    else if (v >= 100 && v <= 107)
      sgr->bg = (unsigned int)(v - 100 + 8) + 1;
    else if (v == 49)
      sgr->bg = 0;
    else if (v == 38 || v == 48) {
      target = (v == 38) ? &sgr->fg : &sgr->bg;
      if (i + 2 < n && p[i + 1] == 5 && p[i + 2] >= 0 && p[i + 2] <= 255) {
        *target = COLOR_256 | (unsigned int)p[i + 2];
        i += 2;
      } else if (i + 4 < n && p[i + 1] == 2 && p[i + 2] >= 0 && p[i + 2] <= 255 &&
                 p[i + 3] >= 0 && p[i + 3] <= 255 && p[i + 4] >= 0 &&
                 p[i + 4] <= 255) {
        *target = COLOR_RGB | ((unsigned int)p[i + 2] << 16) |
                  ((unsigned int)p[i + 3] << 8) | (unsigned int)p[i + 4];
        i += 4;
      } else {
        return 0;
      }
    } else {
      return 0;
    }
  }
  return 1;
}

/**
 * @brief Replays `frame` onto s_next.
 * @return 0 if the frame uses something this doesn't model.
 */
static int replay(Screen *s, const unsigned char *f, int len) {
  int i;
  int k;
  int glyph_len;
  int params[16];
  int nparams;
  int private_mode;
  int have_digit;
  int a;
  int b;
  unsigned char final;
  Cell *c;

  i = 0;
  while (i < len) {
    if (f[i] == '\r') {
      s->col = 0;
      i++;
    } else if (f[i] == '\n') {
      if (s->row + 1 >= s->rows)
        return 0; // would scroll
      s->row++;
      i++;
    } else if (f[i] == 0x1b) {
      if (i + 1 >= len || f[i + 1] != '[')
        return 0;
      i += 2;
      private_mode = 0;
      if (i < len && f[i] == '?') {
        private_mode = 1;
        i++;
      }
      nparams = 0;
      params[0] = 0;
      have_digit = 0;
      while (i < len && ((f[i] >= '0' && f[i] <= '9') || f[i] == ';')) {
        if (f[i] == ';') {
          if (nparams >= 15)
            return 0;
          nparams++; // an empty parameter stays 0
          params[nparams] = 0;
          have_digit = 0;
        } else {
          if (params[nparams] > 100000)
            return 0;
          params[nparams] = params[nparams] * 10 + (f[i] - '0');
          have_digit = 1;
        }
        i++;
      }
      if (have_digit || nparams > 0)
        nparams++;
      if (i >= len)
        return 0;
      final = f[i++];
      a = nparams > 0 ? params[0] : 0;
      b = nparams > 1 ? params[1] : 0;

      if (private_mode) {
        if (nparams == 1 && a == 25 && (final == 'h' || final == 'l'))
          s->cursor_visible = (final == 'h');
        else
          return 0;
        continue;
      }
      switch (final) {
      case 'H':
      case 'f':
        s->row = (a > 0 ? a : 1) - 1;
        s->col = (b > 0 ? b : 1) - 1;
        if (s->row >= s->rows)
          s->row = s->rows - 1;
        if (s->col >= s->cols)
          s->col = s->cols - 1;
        break;
      case 'A':
        s->row -= a > 0 ? a : 1;
        if (s->row < 0)
          s->row = 0;
        break;
      case 'B':
        s->row += a > 0 ? a : 1;
        if (s->row >= s->rows)
          s->row = s->rows - 1;
        break;
      case 'C':
        s->col += a > 0 ? a : 1;
        if (s->col >= s->cols)
          s->col = s->cols - 1;
        break;
      case 'D':
        if (s->col >= s->cols)
          s->col = s->cols - 1;
        s->col -= a > 0 ? a : 1;
        if (s->col < 0)
          s->col = 0;
        break;
      case 'G':
        s->col = (a > 0 ? a : 1) - 1;
        if (s->col >= s->cols)
          s->col = s->cols - 1;
        break;
      case 'K':
        if (s->col >= s->cols)
          return 0; // erasing with a wrap pending differs between terminals
        if (a == 0)
          eraseCells(s, s->row, s->col, s->cols);
        else if (a == 1)
          eraseCells(s, s->row, 0, s->col + 1);
        else if (a == 2)
          eraseCells(s, s->row, 0, s->cols);
        else
          return 0;
        break;
      case 'J':
        if (s->col >= s->cols)
          return 0;
        if (a == 0) {
          eraseCells(s, s->row, s->col, s->cols);
          for (k = s->row + 1; k < s->rows; k++)
            eraseCells(s, k, 0, s->cols);
        } else if (a == 1) {
          for (k = 0; k < s->row; k++)
            eraseCells(s, k, 0, s->cols);
          eraseCells(s, s->row, 0, s->col + 1);
        } else if (a == 2) {
          for (k = 0; k < s->rows; k++)
            eraseCells(s, k, 0, s->cols);
        } else {
          return 0;
        }
        break;
      case 'm':
        if (!applySgr(&s->sgr, params, nparams))
          return 0;
        break;
      default:
        return 0;
      }
    } else if (f[i] < 0x20 || f[i] == 0x7f) {
      return 0;
    } else {
      if (f[i] < 0x80)
        glyph_len = 1;
      else if ((f[i] & 0xE0) == 0xC0)
        glyph_len = 2;
      else if ((f[i] & 0xF0) == 0xE0)
        glyph_len = 3;
      else if ((f[i] & 0xF8) == 0xF0)
        glyph_len = 4;
      else
        return 0;
      if (i + glyph_len > len)
        return 0;
      for (k = 1; k < glyph_len; k++)
        if ((f[i + k] & 0xC0) != 0x80)
          return 0;
      if (s->col >= s->cols) {
        // Autowrap: the previous character filled the last column.
        if (s->row + 1 >= s->rows)
          return 0;
        s->row++;
        s->col = 0;
      }
      c = cellAt(s, s->row, s->col);
      memcpy(c->glyph, f + i, (size_t)glyph_len);
      c->len = (unsigned char)glyph_len;
      c->erased = 0;
      c->sgr = s->sgr;
      s->col++;
      i += glyph_len;
    }
  }
  return 1;
}

static void appendSgr(struct abuf *ab, const Sgr *sgr) {
  char seq[96];
  int n;
  unsigned int color;
  n = snprintf(seq, sizeof(seq), "\x1b[0");
  if (sgr->attrs & A_BOLD) n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";1");
  if (sgr->attrs & A_DIM) n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";2");
  if (sgr->attrs & A_ITALIC) n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";3");
  if (sgr->attrs & A_UNDERLINE) n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";4");
  if (sgr->attrs & A_BLINK) n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";5");
  if (sgr->attrs & A_REVERSE) n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";7");
  if (sgr->attrs & A_HIDDEN) n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";8");
  if (sgr->attrs & A_STRIKE) n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";9");
  color = sgr->fg;
  if (color & COLOR_RGB)
    n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";38;2;%u;%u;%u",
                  (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
  else if (color & COLOR_256)
    n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";38;5;%u", color & 0xFF);
  else if (color)
    n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";%u",
                  color <= 8 ? 30 + color - 1 : 90 + color - 9);
  color = sgr->bg;
  if (color & COLOR_RGB)
    n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";48;2;%u;%u;%u",
                  (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
  else if (color & COLOR_256)
    n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";48;5;%u", color & 0xFF);
  else if (color)
    n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";%u",
                  color <= 8 ? 40 + color - 1 : 100 + color - 9);
  n += snprintf(seq + n, sizeof(seq) - (size_t)n, "m");
  abAppend(ab, seq, n);
}

static int rowsDiffer(Screen *a, Screen *b, int row) {
  Cell *x;
  Cell *y;
  int i;
  for (i = 0; i < a->cols; i++) {
    x = cellAt(a, row, i);
    y = cellAt(b, row, i);
    if (x->len != y->len || x->erased != y->erased || !sgrEqual(&x->sgr, &y->sgr) ||
        memcmp(x->glyph, y->glyph, x->len) != 0)
      return 1;
  }
  return 0;
}

/**
 * @brief Sends one row of `s`: its written cells, then an erase for the
 * blank tail, so the terminal ends up with exactly that row.
 */
static void emitRow(struct abuf *ab, Screen *s, int row, Sgr *current) {
  char seq[32];
  int n;
  int end;
  int i;
  Cell *c;
  Cell *last;

  n = snprintf(seq, sizeof(seq), "\x1b[%d;1H", row + 1);
  abAppend(ab, seq, n);

  // The erased tail, if it shares one rendition, is one CSI K.
  end = s->cols;
  last = cellAt(s, row, s->cols - 1);
  if (last->erased) {
    while (end > 0) {
      c = cellAt(s, row, end - 1);
      if (!c->erased || !sgrEqual(&c->sgr, &last->sgr))
        break;
      end--;
    }
  }
  for (i = 0; i < end; i++) {
    c = cellAt(s, row, i);
    if (!sgrEqual(&c->sgr, current)) {
      appendSgr(ab, &c->sgr);
      *current = c->sgr;
    }
    abAppend(ab, c->glyph, c->len);
  }
  if (end < s->cols) {
    if (!sgrEqual(&last->sgr, current)) {
      appendSgr(ab, &last->sgr);
      *current = last->sgr;
    }
    abAppend(ab, "\x1b[K", 3);
  }
}

void screenDiffWrite(const char *frame, int len, int rows, int cols) {
  struct abuf ab = ABUF_INIT;
  Screen tmp;
  Sgr current;
  char seq[32];
  int full;
  int changed;
  int row;
  int n;

  if (rows <= 0 || cols <= 0 || !screenAlloc(&s_next, rows, cols)) {
    screenWrite(frame, (size_t)len);
    s_valid = 0;
    return;
  }

  full = !s_valid || s_shown.rows != rows || s_shown.cols != cols;
  if (full) {
    memset(&s_next.sgr, 0, sizeof(s_next.sgr));
    s_next.row = 0;
    s_next.col = 0;
    for (row = 0; row < rows; row++)
      eraseCells(&s_next, row, 0, cols);
    s_next.cursor_visible = 1;
  } else {
    memcpy(s_next.cells, s_shown.cells, (size_t)rows * (size_t)cols * sizeof(Cell));
    s_next.row = s_shown.row;
    s_next.col = s_shown.col;
    s_next.sgr = s_shown.sgr;
    s_next.cursor_visible = s_shown.cursor_visible;
  }

  if (!replay(&s_next, (const unsigned char *)frame, len)) {
    screenWrite(frame, (size_t)len);
    s_valid = 0;
    return;
  }

  changed = full || s_next.row != s_shown.row || s_next.col != s_shown.col ||
            s_next.cursor_visible != s_shown.cursor_visible;
  for (row = 0; row < rows && !changed; row++)
    if (rowsDiffer(&s_next, &s_shown, row))
      changed = 1;

  if (changed) {
    abAppend(&ab, "\x1b[?25l", 6);
    // Reset first: what the terminal has now is s_shown.sgr, but a full
    // repaint can't rely on it.
    abAppend(&ab, "\x1b[0m", 4);
    memset(&current, 0, sizeof(current));
    for (row = 0; row < rows; row++)
      if (full || rowsDiffer(&s_next, &s_shown, row))
        emitRow(&ab, &s_next, row, &current);
    // Leave the terminal where the frame left it, so the next frame's
    // replay starts from the same state.
    if (!sgrEqual(&current, &s_next.sgr))
      appendSgr(&ab, &s_next.sgr);
    n = snprintf(seq, sizeof(seq), "\x1b[%d;%dH",
                 (s_next.row < rows ? s_next.row : rows - 1) + 1,
                 (s_next.col < cols ? s_next.col : cols - 1) + 1);
    abAppend(&ab, seq, n);
    if (s_next.cursor_visible)
      abAppend(&ab, "\x1b[?25h", 6);
    screenWrite(ab.b, (size_t)ab.len);
    abFree(&ab);
  }

  // The column can sit one past the edge (wrap pending); the CUP above
  // put the real cursor on the last column, so record that.
  if (s_next.col >= cols)
    s_next.col = cols - 1;
  tmp = s_shown;
  s_shown = s_next;
  s_next = tmp;
  s_valid = 1;
}

void screenDiffInvalidate(void) { s_valid = 0; }

void screenDiffFree(void) {
  gfree(s_shown.cells);
  gfree(s_next.cells);
  memset(&s_shown, 0, sizeof(s_shown));
  memset(&s_next, 0, sizeof(s_next));
  s_valid = 0;
}

void screenDiffSwap(void **other) {
  SavedScreen *saved = *other;
  SavedScreen mine;
  if (!saved) {
    saved = gcalloc(1, sizeof(*saved));
    if (!saved) {
      // Keep drawing with this state; the window just repaints in full.
      s_valid = 0;
      return;
    }
  }
  mine.shown = s_shown;
  mine.valid = s_valid;
  mine.next = s_next;
  s_shown = saved->shown;
  s_valid = saved->valid;
  s_next = saved->next;
  *saved = mine;
  *other = saved;
}

void screenDiffFreeSaved(void *saved) {
  SavedScreen *s = saved;
  if (!s)
    return;
  gfree(s->shown.cells);
  gfree(s->next.cells);
  gfree(s);
}
