/**
 * @file autosave.c
 * @brief Periodic copies of unsaved buffers, and recovering them
 * @details See autosave.h for the behaviour. The copy is written next to
 * its final name and renamed into place, so a crash while autosaving
 * leaves the previous copy intact.
 */

#include "../include/autosave.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/file_io.h"
#include "../include/galloc.h"
#include "../include/input.h"
#include "../include/os.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

extern EditorConfig E;

// The tick runs every ~50ms; buffers are only looked at once a second.
static time_t s_last_scan = 0;

/**
 * @brief Whether `buf` is something autosave should copy at all.
 */
static int autosaveApplies(const struct buffer_t *buf) {
  return buf && buf->type == FILE_BUFF && buf->state == READ_AND_WRITE &&
         buf->fullname != NULL;
}

/**
 * @brief ~/.beluga/autosave/<fullname with / \ : replaced by !>.
 * @return galloc'd path, or NULL.
 */
static char *autosavePath(const struct buffer_t *buf) {
  char *full;
  char *p;
  char *path;
  const char *home = os_home_dir();
  if (!home || !buf->fullname)
    return NULL;
  full = (char *)utf8ToString(buf->fullname);
  if (!full)
    return NULL;
  for (p = full; *p; p++)
    if (*p == '/' || *p == '\\' || *p == ':')
      *p = '!';
  path = utilFormat("%s/.beluga/autosave/%s", home, full);
  gfree(full);
  return path;
}

void autosaveWrite(struct buffer_t *buf) {
  char *path;
  char *tmp;
  char *dir;
  char *line;
  char err[256];
  FILE *f;
  int i;
  int ok;
  const char *home = os_home_dir();

  if (!autosaveApplies(buf) || !home)
    return;

  dir = utilFormat("%s/.beluga/autosave", home);
  if (dir) {
    os_mkdirs(dir, err, sizeof(err));
    gfree(dir);
  }

  path = autosavePath(buf);
  tmp = path ? utilFormat("%s.tmp", path) : NULL;
  if (!path || !tmp) {
    gfree(path);
    gfree(tmp);
    return;
  }

  f = fopen(tmp, "wb");
  ok = f != NULL;
  for (i = 0; ok && i < buf->numrows; i++) {
    line = (char *)utf8ToString(buf->row[i].characters);
    if (!line) {
      ok = 0;
      break;
    }
    ok = fputs(line, f) >= 0 && fputs(buf->crlf ? "\r\n" : "\n", f) >= 0;
    gfree(line);
  }
  if (f && fclose(f) != 0)
    ok = 0;

  // rename() over an existing file fails on Windows.
  if (ok) {
    remove(path);
    ok = rename(tmp, path) == 0;
  }
  if (!ok) {
    remove(tmp);
    appDebug("autosave: couldn't write %s\n", path);
  } else {
    buf->autosaved_dirty = buf->dirty;
  }
  buf->autosave_time = time(NULL);

  gfree(path);
  gfree(tmp);
}

void autosaveDiscard(struct buffer_t *buf) {
  char *path;
  if (!autosaveApplies(buf))
    return;
  path = autosavePath(buf);
  if (path) {
    remove(path);
    gfree(path);
  }
  buf->autosaved_dirty = 0;
  buf->autosave_time = 0;
}

void autosaveTick(void) {
  struct buffer_t *b;
  int first_id;
  const time_t now = time(NULL);
  const int interval = E.constantes.AUTOSAVE_INTERVAL;

  if (interval <= 0 || E.buffers == NULL || now == s_last_scan)
    return;
  s_last_scan = now;

  first_id = E.buffers->buffer_id;
  b = E.buffers;
  do {
    // dirty counts edits since the last save: a value different from the
    // one last autosaved means there is something new to copy.
    if (autosaveApplies(b) && b->dirty && b->dirty != b->autosaved_dirty &&
        now - b->autosave_time >= interval)
      autosaveWrite(b);
    b = b->next;
  } while (b->buffer_id != first_id);
}

void autosaveOfferRecovery(struct buffer_t *buf) {
  char *path;
  char *name;
  char *c;
  char when[32];
  char prompt[512];
  OsFileInfo saved_info;
  OsFileInfo file_info;
  char *file_str;
  int file_exists;
  time_t t;
  struct tm *tm;
  Utf8Char *answer;
  Utf8Char *upath;
  int yes;

  if (!autosaveApplies(buf))
    return;
  path = autosavePath(buf);
  if (!path)
    return;
  if (os_file_info(path, &saved_info) != 0) {
    gfree(path);
    return;
  }

  file_str = (char *)utf8ToString(buf->fullname);
  file_exists = file_str && os_file_info(file_str, &file_info) == 0;
  gfree(file_str);
  // Saved after the copy was taken (e.g. by another editor): the copy is
  // stale.
  if (file_exists && file_info.mtime > saved_info.mtime) {
    remove(path);
    gfree(path);
    return;
  }

  t = (time_t)saved_info.mtime;
  tm = localtime(&t);
  if (!tm || strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm) == 0)
    snprintf(when, sizeof(when), "an earlier session");

  // editorPrompt() takes the question as a format string with one %u for
  // the answer: keep the file name from adding its own %.
  name = (char *)utf8ToString(buf->filename);
  for (c = name; c && *c; c++)
    if (*c == '%')
      *c = '?';
  snprintf(prompt, sizeof(prompt),
           "%s has unsaved changes from %s. Recover them? (y/n): %%u",
           name ? name : "This file", when);
  gfree(name);

  answer = editorPrompt(prompt, NULL, 0);
  yes = answer && answer->length == 1 &&
        (answer->character[0] == 'y' || answer->character[0] == 'Y');
  gfree(answer);

  if (!yes) {
    remove(path);
    gfree(path);
    editorSetStatusMessage("Autosave discarded");
    return;
  }

  upath = stringToUtf8((unsigned char *)path);
  gfree(path);
  if (!upath)
    return;
  editorReloadFrom(buf, upath);
  gfree(upath);
  // Modified until saved: the file on disk is still the old version.
  buf->dirty = 1;
  buf->autosaved_dirty = 1;
  editorSetStatusMessage("Recovered unsaved changes - save to keep them");
}
