/**
 * @file config_load.c
 * @brief Loads Lisp config files form by form and keeps the errors
 * @details The interpreter's own lisp_read_file() parses a whole file into
 * one (begin ...) and stops at the first problem, which used to mean either
 * die("init failed") (a syntax error in init.lisp) or a silently half-loaded
 * config (an error while evaluating it, or anywhere in a package). Here the
 * file is split into its top-level forms first, then each form is read and
 * evaluated on its own: a bad form is recorded with its file and line, and
 * the next one still runs.
 */

#include "../include/config_load.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/lisp.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern EditorConfig E;

// How much of the interpreter's own message (written to its err_port) is
// kept per error.
#define CONFIG_DETAIL_MAX 160

static char **s_errors = NULL;
static int s_error_count = 0;

void configReportError(const char *fmt, ...) {
  char line[512];
  char **grown;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  grown = (char **)grealloc(s_errors, (size_t)(s_error_count + 1) * sizeof(char *));
  if (!grown)
    return;
  s_errors = grown;
  s_errors[s_error_count] = utilStrdup(line);
  if (s_errors[s_error_count])
    s_error_count++;
  appDebug("config: %s\n", line);
}

int configErrorCount(void) { return s_error_count; }

void configErrorsFree(void) {
  int i;
  for (i = 0; i < s_error_count; i++)
    gfree(s_errors[i]);
  gfree(s_errors);
  s_errors = NULL;
  s_error_count = 0;
}

/**
 * @brief Line number (1-based) of `at` inside `text`.
 */
static int lineOf(const char *text, const char *at) {
  int line = 1;
  const char *p;
  for (p = text; p < at; p++)
    if (*p == '\n')
      line++;
  return line;
}

/**
 * @brief Skips whitespace and `;` comments.
 */
static const char *skipBlank(const char *p, const char *end) {
  while (p < end) {
    if (*p == ';') {
      while (p < end && *p != '\n')
        p++;
    } else if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == '\f') {
      p++;
    } else {
      break;
    }
  }
  return p;
}

static int isDelimiter(const char c) {
  return c == '(' || c == ')' || c == '[' || c == ']' || c == '"' ||
         c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
         c == '\f';
}

/**
 * @brief Finds the end of the top-level datum starting at `p`.
 * @details Mirrors the interpreter's lexer closely enough to split a file:
 * strings (with backslash escapes), `#\x` character literals (so `#\(` is
 * not a paren), `;` comments, and the quote prefixes ' ` , ,@.
 * @param[out] status 0 = complete datum, 1 = unclosed list/string (the
 * datum runs to the end of the file), 2 = a stray closing paren.
 * @return One past the datum's last character.
 */
static const char *datumEnd(const char *p, const char *end, int *status) {
  int depth = 0;
  *status = 0;

  // Quote prefixes attach to the datum that follows.
  while (p < end && (*p == '\'' || *p == '`' || *p == ',' || *p == '@')) {
    p++;
    p = skipBlank(p, end);
  }
  if (p >= end) {
    *status = 1;
    return end;
  }
  if (*p == ')' || *p == ']') {
    *status = 2;
    return p + 1;
  }

  do {
    if (p >= end) {
      *status = 1;
      return end;
    }
    switch (*p) {
    case ';':
      while (p < end && *p != '\n')
        p++;
      break;
    case '"':
      p++;
      while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end)
          p++;
        p++;
      }
      if (p >= end) {
        *status = 1;
        return end;
      }
      p++;
      break;
    case '#':
      p++;
      if (p < end && *p == '\\') {
        p++; // the character itself, whatever it is
        if (p < end)
          p++;
        while (p < end && !isDelimiter(*p)) // #\space, #\newline, ...
          p++;
      }
      break;
    case '(':
    case '[':
      depth++;
      p++;
      break;
    case ')':
    case ']':
      depth--;
      p++;
      break;
    default:
      if (depth == 0) {
        // A bare atom at top level: runs to the next delimiter.
        while (p < end && !isDelimiter(*p))
          p++;
      } else {
        p++;
      }
      break;
    }
  } while (depth > 0);

  return p;
}

/**
 * @brief Reads back what the interpreter wrote to its err_port since
 * `start`, flattened to one line.
 */
static void errPortSince(long start, char *out, size_t out_size) {
  FILE *port = lisp_stderr(E.ctx);
  long now;
  size_t n;
  size_t i;
  out[0] = '\0';
  if (!port || start < 0 || out_size == 0)
    return;
  fflush(port);
  now = ftell(port);
  if (now <= start || fseek(port, start, SEEK_SET) != 0)
    return;
  n = fread(out, 1, out_size - 1, port);
  out[n] = '\0';
  fseek(port, 0, SEEK_END);
  for (i = 0; i < n; i++)
    if (out[i] == '\n' || out[i] == '\r' || out[i] == '\t')
      out[i] = ' ';
  while (n > 0 && out[n - 1] == ' ')
    out[--n] = '\0';
}

static void reportFormError(const char *label, int line, LispError err,
                            long port_start) {
  char detail[CONFIG_DETAIL_MAX];
  errPortSince(port_start, detail, sizeof(detail));
  // The interpreter's own message ("foo is not defined.") says more than
  // the error kind does, so it wins when there is one.
  if (detail[0])
    configReportError("%s:%d: %s", label, line, detail);
  else
    configReportError("%s:%d: %s", label, line, lisp_error_string(err));
}

int configLoadFile(const char *path, const char *label) {
  FILE *f;
  char *text;
  long size;
  size_t got;
  const char *p;
  const char *end;
  const char *form_end;
  int status;
  int line;
  int before = s_error_count;
  long port_start;
  LispError err;
  Lisp form;

  f = fopen(path, "rb");
  if (!f) {
    configReportError("%s: can't open %s", label, path);
    return s_error_count - before;
  }
  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    configReportError("%s: can't read %s", label, path);
    return s_error_count - before;
  }
  text = (char *)galloc((size_t)size + 1);
  if (!text) {
    fclose(f);
    configReportError("%s: out of memory", label);
    return s_error_count - before;
  }
  got = fread(text, 1, (size_t)size, f);
  fclose(f);
  text[got] = '\0';

  p = text;
  end = text + got;
  while ((p = skipBlank(p, end)) < end) {
    line = lineOf(text, p);
    form_end = datumEnd(p, end, &status);
    if (status == 1) {
      configReportError("%s:%d: unclosed ( or \" - the rest of the file was "
                        "skipped",
                        label, line);
      break;
    }
    if (status == 2) {
      configReportError("%s:%d: unexpected )", label, line);
      p = form_end;
      continue;
    }

    port_start = lisp_stderr(E.ctx) ? ftell(lisp_stderr(E.ctx)) : -1;
    err = LISP_ERROR_NONE;
    form = lisp_read_range(p, form_end, &err, E.ctx);
    if (err != LISP_ERROR_NONE) {
      reportFormError(label, line, err, port_start);
    } else {
      err = LISP_ERROR_NONE;
      lisp_eval(form, &err, E.ctx);
      if (err != LISP_ERROR_NONE)
        reportFormError(label, line, err, port_start);
    }
    p = form_end;
  }

  gfree(text);
  return s_error_count - before;
}

void configShowErrors(void) {
  Utf8Char *name;
  Utf8Char *uline;
  struct buffer_t *buf;
  int id;
  int i;

  if (s_error_count == 0)
    return;

  name = stringToUtf8((unsigned char *)"*config-errors*");
  id = bufferCreate(name, READ_ONLY, EXPLORER_BUFF);
  gfree(name);
  buf = bufferFindById(id);
  if (buf && buf->numrows == 0) {
    for (i = 0; i < s_error_count; i++) {
      uline = stringToUtf8((unsigned char *)s_errors[i]);
      if (!uline)
        continue;
      bufferInsertRow(buf, buf->numrows, uline, utf8StringLen(uline) - 1);
      gfree(uline);
    }
    buf->dirty = 0;
  }

  if (s_error_count == 1)
    editorSetStatusMessage("Config error: %s", s_errors[0]);
  else
    editorSetStatusMessage("Config error: %s (+%d more, see *config-errors*)",
                           s_errors[0], s_error_count - 1);
}
