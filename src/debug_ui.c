/**
 * @file debug_ui.c
 * @brief The debugger in the editor: gutter marks, the "*debug*" panel and
 * the "*debug-output*" buffer
 * @details The panel is a read-only scratch buffer (EXPLORER_BUFF) like
 * "*search*": re-rendered in place from dap.c's state, with its own key
 * prefix ("debug-view") while it has focus, where ENTER acts on the row
 * under the cursor. s_rows remembers what each rendered row stands for.
 */

#include "../include/debug_ui.h"

#include "../include/append_buffer.h"
#include "../include/breakpoints.h"
#include "../include/buffer.h"
#include "../include/dap.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/project_search.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

#define PANEL_NAME "*debug*"
#define PANEL_PREFIX "debug-view"
#define OUTPUT_NAME "*debug-output*"
#define PANEL_OUTPUT_LINES 8 // the end of the output, shown in the panel
#define VALUE_MAX 160        // longer values are cut, with "…"

typedef enum { ROW_OTHER, ROW_FRAME, ROW_VAR } RowKind;

typedef struct
{
    RowKind kind;
    int frame;   // ROW_FRAME
    DapVar *var; // ROW_VAR
} PanelRow;

static int s_panel_id = -1;
static int s_output_id = -1;
static PanelRow *s_rows;
static int s_row_count;
static int s_row_cap;

/***********************************************************************************************************************
 *
 *                                                 Gutter
 *
 **********************************************************************************************************************/

char *debugUiGutterPath(const struct buffer_t *buf) {
  char *path = breakpointsBufferPath(buf);
  const char *current;
  int line;
  if (!path)
    return NULL;
  if (breakpointsInFile(path) > 0)
    return path;
  if (dapCurrentLine(&current, &line) && os_path_equal(current, path))
    return path;
  gfree(path);
  return NULL;
}

int debugUiWantsGutter(const struct buffer_t *buf) {
  char *path = debugUiGutterPath(buf);
  gfree(path);
  return path != NULL;
}

int debugUiIsCurrentLine(const char *path, const int line) {
  const char *current;
  int current_line;
  return path && dapCurrentLine(&current, &current_line) && current_line == line &&
         os_path_equal(current, path);
}

int debugUiDrawGutter(struct abuf *ab, const char *path, const int line) {
  const Breakpoint *bp;
  const char *glyph;
  const char *color;
  if (!path)
    return 0;
  bp = breakpointAt(path, line);
  if (debugUiIsCurrentLine(path, line)) {
    // ▶ where the program is stopped, on top of a breakpoint there.
    glyph = "▶";
    color = "\x1b[1;33m";
  } else if (bp) {
    // ● a breakpoint, ◆ one with a condition; hollow while a session runs
    // and the adapter hasn't placed it (no code on that line, not loaded).
    if (dapState() != DAP_OFF && !bp->verified)
      glyph = bp->condition ? "◇" : "○";
    else
      glyph = bp->condition ? "◆" : "●";
    color = "\x1b[1;31m";
  } else {
    return 0;
  }
  abAppend(ab, color, (int)strlen(color));
  abAppend(ab, glyph, (int)strlen(glyph));
  abAppend(ab, "\x1b[0m", 4);
  abAppend(ab, E.theme.BACKGROUND_COLOR, (int)strlen(E.theme.BACKGROUND_COLOR));
  abAppend(ab, " ", 1);
  return 1;
}

/***********************************************************************************************************************
 *
 *                                                 Scratch buffers
 *
 **********************************************************************************************************************/

static void clearRows(struct buffer_t *buf) {
  int i;
  for (i = 0; i < buf->numrows; i++)
    bufferFreeRow(&buf->row[i]);
  gfree(buf->row);
  buf->row = NULL;
  buf->numrows = 0;
}

static void addRow(struct buffer_t *buf, const char *text, const RowKind kind, const int frame,
                   DapVar *var) {
  Utf8Char *u = stringToUtf8((unsigned char *)text);
  PanelRow *grown;
  if (!u)
    return;
  bufferInsertRow(buf, buf->numrows, u, utf8StringLen(u) - 1);
  gfree(u);
  if (buf->buffer_id != s_panel_id)
    return;
  if (s_row_count == s_row_cap) {
    grown = grealloc(s_rows, sizeof(PanelRow) * (size_t)(s_row_cap ? s_row_cap * 2 : 64));
    if (!grown)
      return;
    s_rows = grown;
    s_row_cap = s_row_cap ? s_row_cap * 2 : 64;
  }
  s_rows[s_row_count].kind = kind;
  s_rows[s_row_count].frame = frame;
  s_rows[s_row_count].var = var;
  s_row_count++;
}

static void addText(struct buffer_t *buf, const char *text) {
  addRow(buf, text, ROW_OTHER, 0, NULL);
}

// The buffer named `name`, made if `*id` doesn't name it any more.
static struct buffer_t *scratchBuffer(int *id, const char *name, const char *prefix) {
  struct buffer_t *buf = bufferFindById(*id);
  Utf8Char *uname;
  if (buf && buf->type == EXPLORER_BUFF)
    return buf;
  uname = stringToUtf8((unsigned char *)name);
  if (!uname)
    return NULL;
  *id = bufferCreate(uname, READ_ONLY, EXPLORER_BUFF);
  gfree(uname);
  buf = bufferFindById(*id);
  if (buf && prefix)
    snprintf(buf->focus_prefix, sizeof(buf->focus_prefix), "%s", prefix);
  return buf;
}

// After re-rendering: the cursor stays on its row while there still is one.
static void keepCursorInside(struct buffer_t *buf) {
  if (buf->y >= buf->numrows)
    buf->y = buf->numrows > 0 ? buf->numrows - 1 : 0;
  buf->x = 0;
  buf->dirty = 0;
}

/***********************************************************************************************************************
 *
 *                                                 The panel
 *
 **********************************************************************************************************************/

static const char *baseName(const char *path) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  return slash ? slash + 1 : (path ? path : "");
}

// `s` on one line, cut to VALUE_MAX bytes (on a character boundary).
static void oneLine(const char *s, char *out, const size_t size) {
  size_t n = 0;
  size_t cut;
  if (!s)
    s = "";
  for (; *s && n + 1 < size; s++)
    out[n++] = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
  out[n] = '\0';
  if (n > VALUE_MAX) {
    cut = VALUE_MAX;
    while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80)
      cut--;
    snprintf(out + cut, size - cut, "…");
  }
}

static void addVars(struct buffer_t *buf, DapVar *vars, const int count, const int depth) {
  char line[512];
  char value[VALUE_MAX + 8];
  const char *marker;
  int i;
  for (i = 0; i < count; i++) {
    marker = vars[i].ref > 0 ? (vars[i].expanded ? "▾ " : "▸ ") : "  ";
    // A struct's value can be empty, its members being its children.
    oneLine(vars[i].value && *vars[i].value ? vars[i].value : (vars[i].ref > 0 ? "{…}" : ""),
            value, sizeof(value));
    if (vars[i].type && *vars[i].type)
      snprintf(line, sizeof(line), "%*s%s%s = %s  (%s)", depth * 2 + 2, "", marker,
               vars[i].name ? vars[i].name : "?", value, vars[i].type);
    else
      snprintf(line, sizeof(line), "%*s%s%s = %s", depth * 2 + 2, "", marker,
               vars[i].name ? vars[i].name : "?", value);
    addRow(buf, line, ROW_VAR, 0, &vars[i]);
    if (vars[i].expanded) {
      if (!vars[i].loaded)
        addText(buf, "        …");
      addVars(buf, vars[i].children, vars[i].child_count, depth + 1);
    }
  }
}

static void renderPanel(struct buffer_t *buf) {
  const DapFrame *frames;
  DapVar *scopes;
  const char *const *output;
  int frame_count;
  int selected;
  int scope_count;
  int output_count;
  int i;
  char line[512];
  const DapState state = dapState();

  clearRows(buf);
  s_row_count = 0;

  switch (state) {
  case DAP_OFF:
    if (dapExitCode() >= 0)
      snprintf(line, sizeof(line), "Not debugging: the program exited with code %d", dapExitCode());
    else
      snprintf(line, sizeof(line), "Not debugging. F5 starts (asks for the program to run)");
    break;
  case DAP_STARTING:
    snprintf(line, sizeof(line), "%s · starting", dapAdapterName());
    break;
  case DAP_RUNNING:
    snprintf(line, sizeof(line), "%s · running", dapAdapterName());
    break;
  default:
    snprintf(line, sizeof(line), "%s · stopped (%s)", dapAdapterName(), dapStopReason());
    break;
  }
  addText(buf, line);
  addText(buf, "F5 go  F10 next  F11 in  S-F11 out  S-F5 stop");
  addText(buf, "ENTER select/open  e evaluate  o output");

  if (state == DAP_STOPPED) {
    frames = dapFrames(&frame_count, &selected);
    addText(buf, "");
    addText(buf, "Call stack");
    for (i = 0; i < frame_count; i++) {
      if (frames[i].path)
        snprintf(line, sizeof(line), "%s#%d %s  %s:%d", i == selected ? "▶ " : "  ", i,
                 frames[i].name ? frames[i].name : "??", baseName(frames[i].path),
                 frames[i].line + 1);
      else
        snprintf(line, sizeof(line), "%s#%d %s", i == selected ? "▶ " : "  ", i,
                 frames[i].name ? frames[i].name : "??");
      addRow(buf, line, ROW_FRAME, i, NULL);
    }

    scopes = dapScopes(&scope_count);
    for (i = 0; i < scope_count; i++) {
      addText(buf, "");
      snprintf(line, sizeof(line), "%s%s", scopes[i].expanded ? "▾ " : "▸ ",
               scopes[i].name ? scopes[i].name : "Scope");
      addRow(buf, line, ROW_VAR, 0, &scopes[i]);
      if (scopes[i].expanded) {
        if (!scopes[i].loaded)
          addText(buf, "    …");
        addVars(buf, scopes[i].children, scopes[i].child_count, 0);
      }
    }
  }

  output = dapOutputTail(&output_count);
  if (output_count > 0) {
    addText(buf, "");
    addText(buf, "Output");
    for (i = output_count > PANEL_OUTPUT_LINES ? output_count - PANEL_OUTPUT_LINES : 0;
         i < output_count; i++) {
      snprintf(line, sizeof(line), "  %s", output[i]);
      addText(buf, line);
    }
  }
  keepCursorInside(buf);
}

static void renderOutput(struct buffer_t *buf) {
  const char *const *output;
  int count;
  int i;
  const int was_at_end = buf->y >= buf->numrows - 1;
  clearRows(buf);
  output = dapOutputTail(&count);
  for (i = 0; i < count; i++)
    addText(buf, output[i]);
  if (count == 0)
    addText(buf, "(no output yet)");
  // Follow the output while the cursor is on its last line.
  if (was_at_end)
    buf->y = buf->numrows - 1;
  keepCursorInside(buf);
}

void debugUiRefresh(void) {
  struct buffer_t *buf = bufferFindById(s_panel_id);
  if (buf && buf->type == EXPLORER_BUFF)
    renderPanel(buf);
  buf = bufferFindById(s_output_id);
  if (buf && buf->type == EXPLORER_BUFF)
    renderOutput(buf);
}

/***********************************************************************************************************************
 *
 *                                                 Panes
 *
 **********************************************************************************************************************/

static int isDebugBuffer(const int id) {
  return id >= 0 && (id == s_panel_id || id == s_output_id);
}

void debugUiOpenPanel(void) {
  struct buffer_t *buf = scratchBuffer(&s_panel_id, PANEL_NAME, PANEL_PREFIX);
  ScreenLayout *layout;
  int i;
  int keep;
  if (!buf)
    return;
  renderPanel(buf);
  layout = splitScreenGetLayout();
  for (i = 0; i < layout->num_panes; i++)
    if (layout->panes[i].buffer_id == s_panel_id)
      return; // already on screen
  keep = layout->active_pane;
  if (layout->num_panes == 1) {
    // Side by side: the code on the left, the panel on the right.
    if (splitScreenSplitActive(SPLIT_VERTICAL) != 0)
      return;
    layout = splitScreenGetLayout();
    splitScreenGetActivePane()->buffer_id = s_panel_id;
    splitScreenGetActivePane()->shown_buffer_id = -1;
  } else {
    // The last pane that isn't the active one.
    for (i = layout->num_panes - 1; i >= 0; i--)
      if (i != keep)
        break;
    layout->panes[i].buffer_id = s_panel_id;
    layout->panes[i].shown_buffer_id = -1;
    return;
  }
  // Back to the code.
  for (i = 0; i < layout->num_panes; i++)
    layout->panes[i].is_active = 0;
  layout->active_pane = keep;
  layout->panes[keep].is_active = 1;
  E.prefix_state = 0;
}

void debugUiShowLocation(const char *path, const int line) {
  ScreenLayout *layout = splitScreenGetLayout();
  EditorPane *pane = NULL;
  struct buffer_t *buf;
  struct buffer_t *shown;
  int id;
  int i;
  id = projectSearchOpenFile(path);
  buf = bufferFindById(id);
  if (!buf)
    return;
  // A pane already showing the file, else the active pane unless it's the
  // panel, else any pane showing a file, else any pane not the panel's.
  for (i = 0; i < layout->num_panes && !pane; i++)
    if (layout->panes[i].buffer_id == id)
      pane = &layout->panes[i];
  if (!pane && !isDebugBuffer(layout->panes[layout->active_pane].buffer_id))
    pane = &layout->panes[layout->active_pane];
  for (i = 0; i < layout->num_panes && !pane; i++) {
    shown = bufferFindById(layout->panes[i].buffer_id);
    if (shown && shown->type == FILE_BUFF)
      pane = &layout->panes[i];
  }
  for (i = 0; i < layout->num_panes && !pane; i++)
    if (!isDebugBuffer(layout->panes[i].buffer_id))
      pane = &layout->panes[i];
  if (!pane)
    pane = &layout->panes[layout->active_pane];

  buf->y = line < buf->numrows ? line : (buf->numrows > 0 ? buf->numrows - 1 : 0);
  buf->x = 0;
  // Scrolled into view (centered) only when it's off screen: stepping
  // through lines already in view doesn't make the pane jump.
  if (pane->buffer_id != id || buf->y < pane->y_offset ||
      buf->y >= pane->y_offset + pane->height)
    pane->shown_buffer_id = -1;
  else if (pane->is_active)
    pane->cursor_y = buf->y - pane->y_offset;
  pane->buffer_id = id;
}

void debugUiPanelSelect(void) {
  struct buffer_t *buf = bufferFindById(splitScreenGetActivePane()->buffer_id);
  const PanelRow *row;
  if (!buf || buf->buffer_id != s_panel_id || buf->y < 0 || buf->y >= s_row_count)
    return;
  row = &s_rows[buf->y];
  if (row->kind == ROW_FRAME)
    dapSelectFrame(row->frame);
  else if (row->kind == ROW_VAR)
    dapToggleVar(row->var);
}

void debugUiShowOutput(void) {
  struct buffer_t *buf = scratchBuffer(&s_output_id, OUTPUT_NAME, PANEL_PREFIX);
  EditorPane *active;
  if (!buf)
    return;
  buf->y = buf->numrows; // follow the end
  renderOutput(buf);
  active = splitScreenGetActivePane();
  active->buffer_id = s_output_id;
  active->shown_buffer_id = -1;
}
