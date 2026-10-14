/**
 * @file output.c
 * @brief Screen rendering and output module for the Beluga text editor
 * @details Handles all screen updates, cursor positioning, status bar
 * rendering, and display synchronization using ANSI escape sequences
 */

#include "../include/output.h"
#include "../include/galloc.h"
#include "../include/append_buffer.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/lsp_ui.h"
#include "../include/buffer_switcher.h"
#include "../include/file_finder.h"
#include "../include/list_popup.h"
#include "../include/which_key.h"
#include "../include/row_op.h"
#include "../include/screen_diff.h"
#include "../include/split_screen.h"
#include "../include/syntax_highlighter.h"
#include "../include/term_render.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/***********************************************************************************************************************
 *
 *                                                 extern definitions
 *
 **********************************************************************************************************************/

extern EditorConfig E;

/***********************************************************************************************************************
 *
 *                                                 functions definitions
 *
 **********************************************************************************************************************/

/**
 * @brief Width in columns of the line-number gutter for `buf`, including
 * its trailing space, or 0 when line numbers are off / not applicable.
 * @details Sized to the buffer's actual line count so a 12000-line file
 * gets 5 digits and a short one doesn't waste columns, with a floor of
 * LINE_NUMBER_MIN_WIDTH so the text doesn't visibly shift left/right as a
 * file grows past a power of ten. Only FILE_BUFFs get numbers: a terminal
 * pane's rows are a PTY's screen, not a document, and an explorer sidebar
 * is a menu.
 */
int editorLineNumberWidth(const struct buffer_t *buf) {
  int digits;
  int n;
  if (!buf || !E.constantes.LINE_NUMBERS || buf->type != FILE_BUFF)
    return 0;

  digits = 1;
  for (n = buf->numrows > 0 ? buf->numrows : 1; n >= 10; n /= 10)
    digits++;
  if (digits < LINE_NUMBER_MIN_WIDTH)
    digits = LINE_NUMBER_MIN_WIDTH;
  return digits + 1; // + a space separating numbers from the marker/text
}

/**
 * @brief Total width reserved to the left of a pane's text: line numbers
 * plus the diagnostic/git marker column.
 * @details Single source of truth for the whole gutter. Drawing, scrolling
 * and cursor placement each used to recompute "has_gutter ? GUTTER_WIDTH : 0"
 * independently, so any new gutter element had to be added in lockstep in
 * several files or the cursor would sit a few columns off the text.
 */
int editorGutterWidth(const struct buffer_t *buf) {
  int marker;
  if (!buf)
    return 0;
  marker = (buf->type == FILE_BUFF && (E.constantes.LSP || buf->git_available))
                   ? GUTTER_WIDTH : 0;
  return editorLineNumberWidth(buf) + marker;
}

/**
 * @brief If buf's mark is active and this row overlaps the mark..point
 * region, fills in `from` and `to` with the row's selected character-column
 * range (in the row's own coordinates, not yet adjusted for horizontal
 * scroll) and `extends_past_eol` with whether the selection continues onto
 * the next row (so the line's trailing padding should be highlighted too).
 * @return 1 if this row has a (possibly empty) selection range, 0 if the
 * mark isn't active or this row is outside the region entirely.
 */
static int rowSelectionRange(const struct buffer_t *buf, const int file_row,
                             int *from, int *to, int *extends_past_eol) {
  int sr;
  int sc;
  int er;
  int ec;
  int before;
  if (!buf->mark_active)
    return 0;

  before = (buf->mark_y < buf->y) ||
               (buf->mark_y == buf->y && buf->mark_x <= buf->x);
  if (before) {
    sr = buf->mark_y; sc = buf->mark_x;
    er = buf->y; ec = buf->x;
  } else {
    sr = buf->y; sc = buf->x;
    er = buf->mark_y; ec = buf->mark_x;
  }
  if (file_row < sr || file_row > er || (sr == er && sc == ec))
    return 0;

  *from = (file_row == sr) ? sc : 0;
  *to = (file_row == er) ? ec : buf->row[file_row].size;
  *extends_past_eol = (file_row < er);
  return 1;
}

/**
 * @brief Wraps visible columns [vis_from, vis_to) of an already-rendered
 * row string (which may itself contain syntax-highlighting ANSI SGR codes)
 * in reverse video, to show a selection. Escape sequences are copied
 * through untouched and don't count as columns, so this composes safely
 * with highlight_line()'s own color codes. Caller must gfree() the result.
 * @param rendered The already-rendered row string to wrap in reverse video
 * @param rendered_len Length of `rendered`, in bytes
 * @param vis_from First visible column to start reverse video at
 * @param vis_to -1 means "to the end of the rendered content".
 * @param out_len Out-param: length of the returned string, in bytes
 */
static char *applySelectionHighlight(const char *rendered, int rendered_len,
                                     int vis_from, int vis_to, int *out_len) {
  size_t cap = (size_t) rendered_len + 32;
  char *out = galloc(cap);
  size_t out_pos = 0;
  int col = 0;
  int reversed = 0;
  int i = 0;
  int start;
  unsigned char lead;
  int clen;

  while (i < rendered_len) {
    if (out_pos + 16 > cap) { cap *= 2; out = grealloc(out, cap); }

    if ((unsigned char) rendered[i] == 0x1b) {
      start = i;
      i++;
      if (i < rendered_len && rendered[i] == '[') {
        i++;
        while (i < rendered_len && !(rendered[i] >= '@' && rendered[i] <= '~'))
          i++;
        if (i < rendered_len)
          i++; // consume the final byte
      }
      memcpy(out + out_pos, rendered + start, (size_t)(i - start));
      out_pos += (size_t)(i - start);
      continue;
    }

    if (!reversed && col == vis_from) {
      memcpy(out + out_pos, "\x1b[7m", 4);
      out_pos += 4;
      reversed = 1;
    }
    if (reversed && vis_to >= 0 && col == vis_to) {
      memcpy(out + out_pos, "\x1b[27m", 5);
      out_pos += 5;
      reversed = 0;
    }

    lead = (unsigned char) rendered[i];
    clen = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
    if (i + clen > rendered_len) clen = 1;
    memcpy(out + out_pos, rendered + i, (size_t) clen);
    out_pos += (size_t) clen;
    i += clen;
    col++;
  }
  if (reversed) {
    if (out_pos + 5 > cap) { cap += 5; out = grealloc(out, cap); }
    memcpy(out + out_pos, "\x1b[27m", 5);
    out_pos += 5;
  }
  *out_len = (int) out_pos;
  return out;
}

/**
 * @brief Renders a single pane with its buffer content
 */
/**
 * @brief Recomputes a pane's view when it has started showing a different
 * buffer.
 * @details Scroll offsets live on the pane, not the buffer, so without this
 * a buffer switched into a pane inherited the previous buffer's scroll
 * position: switching from line 1000 of a file to a short buffer left its
 * first rows scrolled out of sight above the cursor. On a change of buffer
 * the view is placed around the new buffer's own cursor instead: from the
 * top when the cursor fits on the first screen, otherwise centered (or, for
 * a terminal, bottom-aligned like the shell's own screen).
 * A no-op while the pane keeps showing the same buffer, so ordinary
 * scrolling (editorScroll()'s minimal adjustments) is untouched.
 */
static void paneFitShownBuffer(EditorPane *pane, const struct buffer_t *buf) {
  if (pane->shown_buffer_id == pane->buffer_id)
    return;
  pane->shown_buffer_id = pane->buffer_id;

  if (buf->y < pane->height)
    pane->y_offset = 0;                            // fits on the first screen
  else if (buf->type == TERMINAL_BUFF)
    pane->y_offset = buf->y - (pane->height - 1);  // a shell's screen: bottom-aligned
  else
    pane->y_offset = buf->y - pane->height / 2;    // deep in a file: centered
  pane->cursor_y = buf->y - pane->y_offset;
  // Horizontal: start from column 0, editorScroll() walks right to the
  // cursor if it's off-screen.
  pane->x_offset = 0;
  pane->cursor_x = 0;
}

static void editorDrawPane(struct abuf *ab, EditorPane *pane) {
  int file_row;
  char pos_buf[32];
  int pos_len;
  int byte_len_to_print;
  char *highlighted;
  const struct buffer_t *buf;
  int has_gutter;
  int lineno_width;
  int gutter_width;
  int content_width;
  int y;
  int is_cursor_line;
  int i;
  char *lineno_color;
  char num_buf[24];
  int num_len;
  GitLineStatus git_status;
  int sel_from;
  int sel_to;
  int sel_extends;
  int has_selection;
  int vis_from;
  int vis_to;
  const LanguageSyntax *lang;
  int wrapped_len;
  char *wrapped;
  int row_off;
  char *line;
  int line_len;
  int filled;

  if (pane == NULL || pane->buffer_id < 0)
    return;

  buf = bufferFindById(pane->buffer_id);
  if (buf == NULL)
    return;
  paneFitShownBuffer(pane, buf);

  // The diagnostic gutter only makes sense for real source files — drawing
  // it for a terminal pane would eat GUTTER_WIDTH columns the PTY was never
  // told to give up, misaligning its own line-wrapping. content_width is
  // what's actually left for text once the gutter (if any) is accounted for;
  // gutter + content_width always adds up to the pane's full on-screen span.
  has_gutter = buf->type == FILE_BUFF && (E.constantes.LSP || buf->git_available);
  lineno_width = editorLineNumberWidth(buf);
  gutter_width = editorGutterWidth(buf);
  content_width = pane->width - gutter_width;
  if (content_width < 0)
    content_width = 0;

  // Seed the syntax highlighter's cross-row block-comment state for
  // wherever this pane's scroll position actually starts, rather than
  // whatever comment_section was last left at by a previous pane or a
  // previous scroll position — see the comment_state fields on
  // struct buffer_t and commentStateAtRowStart()'s own doc comment.
  syntaxHighlighterSeed(commentStateAtRowStart((struct buffer_t *) buf, pane->y_offset));

  // Draw all pane
  for (y = 0; y < pane->height; y++) {
    file_row = y + pane->y_offset;
    is_cursor_line = (pane->is_active && file_row == buf->y);

    // Set cursor at start of pane row
    pos_len = snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH",
                       pane->origin_y + y + 1, pane->origin_x + 1);
    abAppend(ab, pos_buf, pos_len);

    // Apply background color (6 bytes for RGB format)
    abAppend(ab, E.theme.BACKGROUND_COLOR,
             (int)strlen(E.theme.BACKGROUND_COLOR));
    // ...and the theme's default foreground, so text drawn without its own
    // explicit color (the tilde below, and the whole plain-text branch
    // further down for anything that isn't a .c/.h file — the explorer
    // sidebar included) renders in a deliberate, theme-consistent color
    // instead of whatever was last left active by a previous row (line
    // numbers reset to the terminal's own default and never re-set an
    // explicit fg of their own; a syntax-highlighted C row leaves its
    // last token's color active). Harmless for highlight_line() output:
    // its very first byte is always one of its own color codes, which
    // simply overrides this immediately.
    abAppend(ab, E.theme.COLOR_DEFAULT, (int) strlen(E.theme.COLOR_DEFAULT));

    // pane line is out of buffer height
    if (file_row >= buf->numrows) {
      // Empty line - show tilde, but still reserve the gutter's columns so
      // the tilde lines up with where text starts on real lines above it
      for (i = 0; i < gutter_width; ++i)
        abAppend(ab, " ", 1);
      abAppend(ab, "~", 1);
      for (i = 0; i < content_width - 1; ++i) {
        abAppend(ab, " ", 1);
      }
    } else {
      if (lineno_width) {
        // The cursor's own line is highlighted (bold, undimmed) so the eye
        // can find it without hunting for the terminal cursor itself.
        //
        // Colors come from the theme rather than a hardcoded "\x1b[2;37m"/
        // "\x1b[1;37m" (dim/bold ANSI white): COLOR_COMMENT is already,
        // by every theme's own design, the muted/secondary tone relative
        // to COLOR_DEFAULT, which is exactly the dim/prominent pairing
        // this gutter wants — and unlike literal ANSI white, it can't end
        // up lighter than the background. It did: bold+white is rendered
        // bright/intense by most terminals, which on theme-light's
        // near-white background (250,251,252) made the cursor's own line
        // number — the one meant to stand out — the least visible thing
        // on screen.
        lineno_color = is_cursor_line ? E.theme.COLOR_DEFAULT : E.theme.COLOR_COMMENT;
        num_len = snprintf(num_buf, sizeof(num_buf), "%*d ",
                               lineno_width - 1, file_row + 1);
        if (is_cursor_line)
          abAppend(ab, "\x1b[1m", 4);
        abAppend(ab, lineno_color, (int) strlen(lineno_color));
        abAppend(ab, num_buf, num_len);
        abAppend(ab, "\x1b[0m", 4);
        abAppend(ab, E.theme.BACKGROUND_COLOR, (int) strlen(E.theme.BACKGROUND_COLOR));
        // The \x1b[0m above also clears the default-fg set at the top of
        // this row's render — re-set it before content starts, or every
        // non-.c/.h FILE_BUFF (the common case — .txt, .md, .lisp, ...)
        // renders its text in the terminal's own default color, not the
        // theme's, the moment line numbers are on (the default).
        abAppend(ab, E.theme.COLOR_DEFAULT, (int) strlen(E.theme.COLOR_DEFAULT));
      }
      if (has_gutter) {
        git_status = (buf->git_lines && file_row < buf->git_lines_count)
                                        ? buf->git_lines[file_row] : GIT_LINE_NONE;
        lspUiDrawGutter(ab, &E.lsp_diagnostics, pane->buffer_id, file_row, git_status);
      }

      // Selection highlight — not applied to TERMINAL_BUFF (a live shell's
      // own escape sequences make wrapping unreliable, and "mark a region
      // in a PTY view" isn't a meaningful operation anyway).
      sel_from = 0; sel_to = 0; sel_extends = 0;
      has_selection = buf->type != TERMINAL_BUFF &&
                          rowSelectionRange(buf, file_row, &sel_from, &sel_to, &sel_extends);
      vis_from = 0; vis_to = -1;
      if (has_selection) {
        vis_from = sel_from - pane->x_offset;
        if (vis_from < 0) vis_from = 0;
        if (!sel_extends) {
          vis_to = sel_to - pane->x_offset;
          if (vis_to < 0) vis_to = 0;
        }
      }

      if (buf->type == TERMINAL_BUFF) {
        // Clipped to the pane, exactly like the file paths below: a
        // terminal row is as wide as the PTY was when the shell wrote it,
        // which stops matching the pane the moment the layout changes.
        highlighted = termRenderRow(&buf->row[file_row], &byte_len_to_print,
                                    pane->x_offset, content_width);
        abAppend(ab, highlighted, byte_len_to_print);
        gfree(highlighted);
      } else {
        if (is_cursor_line)
            abAppend(ab, E.theme.LINE_CURSOR_BACKGROUND, (int) strlen(E.theme.LINE_CURSOR_BACKGROUND));
        else
            abAppend(ab, E.theme.BACKGROUND_COLOR, (int) strlen(E.theme.BACKGROUND_COLOR));
        // Extension-based lookup against whatever packages have actually
        // registered via (syntax-define-language ...), rather than a
        // hardcoded "does the filename end in c or h" check — the old
        // check also matched anything merely *ending* in those letters
        // (a file called "search" would have gone through the C
        // tokenizer), and gave C highlighting no matter what packages
        // were loaded. Now a buffer with no registered language for its
        // extension always falls through to plain text, C included.
        lang = syntaxLanguageFor(buf->filename);
        if (lang) {
          byte_len_to_print = 0;
          // Render line with syntax highlighting, constrain to pane width
          highlighted = highlight_line(&buf->row[file_row], &byte_len_to_print,
                                       pane->x_offset, content_width, lang);

          if (has_selection) {
            wrapped_len = 0;
            wrapped = applySelectionHighlight(highlighted, byte_len_to_print,
                                                    vis_from, vis_to, &wrapped_len);
            gfree(highlighted);
            highlighted = wrapped;
            byte_len_to_print = wrapped_len;
          }

          // Print only up to pane width
          abAppend(ab, highlighted, byte_len_to_print);
          gfree(highlighted);
        } else {

          // Render basic line, starting at the horizontal scroll offset.
          // Clamp the offset to this row's length first: x_offset tracks
          // the (possibly much longer) line the cursor sits on, so a
          // shorter row drawn in the same pane can have size < x_offset,
          // and &characters[x_offset] would then read past the row. size
          // itself is the terminator cell, which renders as empty.
          row_off = pane->x_offset < buf->row[file_row].size
                            ? pane->x_offset
                            : buf->row[file_row].size;
          line = (char *)utf8ToNString(
              &buf->row[file_row].characters[row_off], content_width);
          line_len = (int) strlen(line);

          if (has_selection) {
            wrapped_len = 0;
            wrapped = applySelectionHighlight(line, line_len, vis_from, vis_to, &wrapped_len);
            gfree(line);
            line = wrapped;
            line_len = wrapped_len;
          }
          abAppend(ab, line, line_len);
          gfree(line);
          line = NULL;
        }
      }

      // Fill remaining space with background color to pane width — also in
      // reverse video if the selection continues past this line's content
      // (a whole line selected, including its line break).
      if (has_selection && sel_extends)
        abAppend(ab, "\x1b[7m", 4);
      // Start from how many columns the content actually filled — clamped
      // at 0, since a row shorter than x_offset contributed nothing and
      // (size - x_offset) would otherwise go negative and over-pad.
      filled = buf->row[file_row].size - pane->x_offset;
      if (filled < 0) filled = 0;
      for (i = filled; i < content_width; i++) {
        abAppend(ab, " ", 1);
      }
      if (has_selection && sel_extends)
        abAppend(ab, "\x1b[27m", 5);
    }
  }
}

/**
 * @brief Renders all panes based on current split configuration
 */
/**
 * @brief Draws every pane, then the dividers between them.
 * @details Dividers are derived from the panes' own rectangles rather than
 * from a single global split mode, which is what lets the layout nest
 * arbitrarily: splitScreenSplitActive() always leaves one column (or row)
 * unclaimed between the two halves it makes, so any pane whose right edge
 * stops short of the screen has a gap there, and the same for its bottom
 * edge. Drawing a rule down/along each such edge fills exactly those gaps,
 * for any number of panes in any arrangement.
 *
 * The old version only understood a two-pane layout and read the split
 * direction from layout->mode, so it drew a full-height rule at pane 0's
 * width — which also painted over pane 0's last text column, since that
 * layout left no room for the divider to sit in.
 *
 * Each rule is bounded to its own pane's span, not the whole screen, so a
 * divider between two stacked panes on the right doesn't cut across the
 * pane on the left. Neighbouring panes may name the same gap; drawing it
 * twice is harmless.
 */
static void editorDrawAllPanes(struct abuf *ab) {
  const ScreenLayout *layout = splitScreenGetLayout();
  int i;
  const EditorPane *p;
  char pos_buf[32];
  int y;
  int len;
  int x;

  for (i = 0; i < layout->num_panes; i++)
    editorDrawPane(ab, &layout->panes[i]);

  for (i = 0; i < layout->num_panes; i++) {
    p = &layout->panes[i];

    // Vertical rule down the gap on this pane's right edge.
    if (p->origin_x + p->width < E.screencols) {
      for (y = 0; y < p->height; y++) {
        len = snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH",
                           p->origin_y + y + 1, p->origin_x + p->width + 1);
        abAppend(ab, pos_buf, len);
        // "│" is U+2502, a 3-byte UTF-8 sequence — sizeof(literal)-1 here
        // instead of a hand-counted length avoids a repeat of the bug this
        // line used to have: a hardcoded length of 9 (right for the plain-
        // ASCII "-" rule below, wrong for this one) silently truncated the
        // trailing "\x1b[0m" reset down to a dangling, unterminated "\x1b[",
        // corrupting the escape sequence stream from the very next divider
        // row onward — invisible while every split rendered at whatever
        // size the terminal started at, and only actually reachable once
        // resizing (which redraws the dividers at their new positions) was
        // wired up for real.
        abAppend(ab, "\x1b[1m│\x1b[0m", sizeof("\x1b[1m│\x1b[0m") - 1);
      }
    }

    // Horizontal rule along the gap under this pane.
    if (p->origin_y + p->height < E.screenrows) {
      len = snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH",
                         p->origin_y + p->height + 1, p->origin_x + 1);
      abAppend(ab, pos_buf, len);
      for (x = 0; x < p->width; x++)
        abAppend(ab, "\x1b[1m-\x1b[0m", sizeof("\x1b[1m-\x1b[0m") - 1);
    }
  }
}

/**
 * @brief Updates scroll offsets to keep cursor visible on screen
 * @details Adjusts E.row_offset and E.col_offset to ensure the cursor remains
 * within the visible viewport. Also updates E.rx (rendered x-coordinate).
 * @note Updates global editor state E
 * @see editorRowCxToRx()
 */
void editorScroll() {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  int usable_width;

  // bufferFindById() returns NULL for an id that isn't in the list — a
  // pane still pointing at a closed buffer, or at -1 after a failed
  // bufferCreate(). The cursor-tracking loop below dereferences buf on
  // every iteration, so there's nothing to scroll toward without it.
  if (buf == NULL)
    return;
  paneFitShownBuffer(active, buf);

  // Same gutter accounting as editorDrawPane(): with the gutter reserving
  // GUTTER_WIDTH columns, only what's left is actually available before the
  // cursor needs to scroll right, or it would visually run past the pane.
  usable_width = active->width - editorGutterWidth(buf);
  if (usable_width < 1)
    usable_width = 1;

  // compute relative coordinates

  while (buf->x != active->cursor_x + active->x_offset ||
         buf->y != active->cursor_y + active->y_offset) {
    if (buf->x < active->cursor_x + active->x_offset) {
      // LEFT
      if (active->cursor_x == 0 && active->x_offset) {
        active->x_offset--;
      } else {
        active->cursor_x--;
      }
    } else {
      // RIGHT
      if (buf->x > active->cursor_x + active->x_offset) {
        if (active->cursor_x == usable_width - 1) {
          active->x_offset++;
        } else {
          active->cursor_x++;
        }
      }
    }

    if (buf->y < active->cursor_y + active->y_offset) {
      if (active->cursor_y == 0 && active->y_offset) {
        active->y_offset--;
      } else {
        active->cursor_y--;
      }
    }

    if (buf->y > active->cursor_y + active->y_offset) {
      if (active->cursor_y == active->height - 1) {
        active->y_offset++;
      } else {
        active->cursor_y++;
      }
    }
  }
}

/**
 * @brief Renders the status bar at the bottom of the screen
 * @details Displays filename, line count, dirty flag, and current cursor
 * position in an inverted color bar. Right-aligns the cursor position
 * indicator.
 * @param ab Pointer to append buffer structure for accumulating output
 * @note Uses ANSI escape codes for color inversion
 */
void editorDrawStatusBar(struct abuf *ab) {
  // TO MODIFY
  int len;
  int offset = 0;

  char status[E.screencols];
  char buf_status[1024] = "";
  char marker, dirty_marker;
  char mode_buf[24];
  char *mode_str = "";
  char *string_filename = NULL;

  EditorPane *active = splitScreenGetActivePane();
  Buffer *buf = bufferFindById(active->buffer_id);
  Buffer *b = NULL;
  ScreenLayout *layout = splitScreenGetLayout();

  // Positioned explicitly rather than inheriting wherever the pane loop
  // happened to leave the cursor. That implicit coupling held only while
  // the last thing drawn was always the bottom-right pane; once dividers
  // were drawn after the panes (needed to support arbitrary splits), the
  // status bar started rendering at the last divider instead of at the
  // bottom of the screen. E.screenrows already excludes these two rows.
  char bar_pos[32];
  int bar_pos_len = snprintf(bar_pos, sizeof(bar_pos), "\x1b[%d;1H", E.screenrows + 1);
  char git_status[80] = "";
  int first_buffer_id;
  abAppend(ab, bar_pos, bar_pos_len);

  abAppend(ab, "\x1b[7m", 4); // inverting colors

  // Pane count rather than the old " [V-SPLIT]"/" [H-SPLIT]": layout->mode
  // only records the *last* split direction, which stopped describing the
  // screen once panes could be split repeatedly and in both directions.
  if (layout->num_panes > 1) {
    snprintf(mode_buf, sizeof(mode_buf), " [%d panes]", layout->num_panes);
    mode_str = mode_buf;
  }

  if (buf && buf->git_available) {
    snprintf(git_status, sizeof(git_status), " (%s%s)", buf->git_branch,
             buf->git_dirty ? "*" : "");
  }

  // Build buffer status showing all buffers with dirty indicators
  if (E.buffers != NULL) {
    first_buffer_id = E.buffers->buffer_id;
    b = E.buffers;
    do {
      string_filename = (char *)utf8ToString(b->filename);
      marker = (b->buffer_id == active->buffer_id) ? '>' : ' ';
      dirty_marker = b->dirty ? '*' : ' ';
      offset +=
          snprintf(&buf_status[offset], sizeof(buf_status) - offset, "%c%d:%s%c ",
                   marker, b->buffer_id,
                   b->filename ? string_filename : "[No Name]", dirty_marker);
      gfree(string_filename);
      b = b->next;
    } while (b->buffer_id != first_buffer_id);
  }

  len = snprintf(status, sizeof(status), "%s%s%s", buf_status, git_status, mode_str);

  if (len > E.screencols) {
    len = E.screencols;
  }

  abAppend(ab, status, len);
  while (len < E.screencols) {
    abAppend(ab, " ", 1);
    ++len;
  }

  abAppend(ab, "\x1b[m", 3); // normal text mode
  abAppend(ab, "\r\n", 2);
}

/**
 * @brief Renders the message bar below the status bar
 * @details Displays temporary status messages for a limited time (5 seconds).
 * Only displays message if within time window and within screen width.
 * @param ab Pointer to append buffer structure for accumulating output
 * @note Messages are set by editorSetStatusMessage()
 */
void editorDrawMessageBar(struct abuf *ab) {
  char *status = (char *) utf8ToString(E.status_msg);
  int msg_len = (int)strlen(status);

  // Same reasoning as the status bar: place it, don't inherit a position.
  char pos[32];
  int pos_len = snprintf(pos, sizeof(pos), "\x1b[%d;1H", E.screenrows + 2);
  int bytes;
  int cols;
  abAppend(ab, pos, pos_len);

  abAppend(ab, ERASE_END_LINE, 3);
  if (msg_len && time(NULL) - E.status_msg_time < 5) {
    // Clip to the screen width (in characters, not bytes): this is the
    // terminal's last row, so a longer message — a long path, say — wraps
    // and scrolls the whole screen up a line until the next full redraw.
    bytes = 0; cols = 0;
    while (bytes < msg_len && cols < E.screencols - 1) {
      bytes++;
      while (bytes < msg_len && ((unsigned char) status[bytes] & 0xC0) == 0x80)
        bytes++;
      cols++;
    }
    abAppend(ab, status, bytes);
  }
  gfree(status);
}

/**
 * @brief Performs complete screen refresh and buffer synchronization
 * @details Clears screen, redraws all visible content (rows, status bar,
 * message bar), positions cursor, and writes accumulated buffer to stdout. This
 * is the main rendering function called each frame.
 * @note Updates global editor state E (via editorScroll())
 * @see editorDrawRows()
 * @see editorDrawStatusBar()
 * @see editorDrawMessageBar()
 */

// ── Frame de-duplication ────────────────────────────────────────────────────
//
// editorRunLoop() calls this unconditionally on every ~50ms tick — needed so
// time-driven UI (the which-key delay, the 5s status-message timeout) keeps
// working without a keypress — but that means a totally static screen (user
// reading, not typing) was being sent an identical, full-screen repaint
// roughly 19 times a second, hiding and re-showing the cursor every single
// time. That's what actually produced the flicker: many terminals (tmux
// especially) visibly blink the hardware cursor on every hide/show pair even
// when nothing underneath moved. Caching the last frame written and skipping
// the write() when the new one is byte-for-byte identical (same content,
// same cursor position) fixes it without touching the redraw cadence itself.
// Lives for the whole session; editorFreeScreenCache() releases it at exit.
static char *g_prev_frame = NULL;
static int g_prev_frame_len = 0;
static int g_prev_frame_cap = 0;

static int screenFrameChanged(const char *buf, int len) {
  if (len != g_prev_frame_len) return 1;
  return len > 0 && memcmp(buf, g_prev_frame, (size_t) len) != 0;
}

static void screenFrameCache(const char *buf, int len) {
  char *grown;
  if (len > g_prev_frame_cap) {
    grown = grealloc(g_prev_frame, (size_t) len);
    if (!grown) return; // best-effort: next frame just always looks "changed"
    g_prev_frame = grown;
    g_prev_frame_cap = len;
  }
  memcpy(g_prev_frame, buf, (size_t) len);
  g_prev_frame_len = len;
}

void editorFreeScreenCache(void) {
  screenDiffFree();
  gfree(g_prev_frame);
  g_prev_frame = NULL;
  g_prev_frame_len = 0;
  g_prev_frame_cap = 0;
}

void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;
  char buf[32];
  EditorPane *active;
  struct buffer_t *buffer;
  const char *diag;
  char single_line[512];
  int i;

  abAppend(&ab, HIDE_CURSOR, 6);
  abAppend(&ab, CURSOR_TOP_LEFT, 3);
  abAppend(&ab, E.theme.BACKGROUND_COLOR,
           (int)strlen(E.theme.BACKGROUND_COLOR));

  editorScroll();

  active = splitScreenGetActivePane();
  buffer = bufferFindById(active->buffer_id);

  editorDrawAllPanes(&ab);
  if (E.constantes.LSP) {

    // ── LSP: draw completion popup every frame while visible
    // ──────────────────

    // reset flags
    if (lspActiveClient()) {
      lspActiveClient()->completion_just_arrived = 0;
      lspActiveClient()->completion_requested = 0;
    }

    // ── LSP: diagnostic for current line in status bar
    // ────────────────────────
    diag = buffer ? lspUiDiagnosticAtCursor(&E.lsp_diagnostics,
                                                       active->buffer_id,
                                                       buffer->y)
                              : NULL;
    if (diag) {
      i = 0;

      // Copy until newline, \0, or screen width
      while (diag[i] && diag[i] != '\n' && i < E.screencols - 4) {
        single_line[i] = diag[i];
        i++;
      }

      // If message was truncated, add ellipsis
      if (diag[i] != '\0' && diag[i] != '\n') {
        single_line[i++] = '.';
        single_line[i++] = '.';
        single_line[i++] = '.';
      }
      single_line[i] = '\0';
      editorSetStatusMessage("● %s", single_line);
    }
  }
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);
  if (E.constantes.LSP && lspActiveClient() &&
      lspActiveClient()->state == LSP_READY) {
    lspUiDrawInfo(&ab, &E.lsp_info);
    lspUiDrawCompletion(&ab, &E.lsp_completion);
  }
  bufferSwitcherDraw(&ab);
  listPopupDraw(&ab);
  fileFinderDraw(&ab);
  whichKeyDraw(&ab);

  // ── Position cursor (account for gutter width) ────────────────────────────
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
           active->cursor_y + active->origin_y + 1,
           active->cursor_x + active->origin_x + 1 + editorGutterWidth(buffer));
  abAppend(&ab, buf, (int)strlen(buf));
  abAppend(&ab, SHOW_CURSOR, 6);

  if (screenFrameChanged(ab.b, ab.len)) {
    screenDiffWrite(ab.b, ab.len, E.screenrows + 2, E.screencols);
    screenFrameCache(ab.b, ab.len);
  }
  abFree(&ab);
}
