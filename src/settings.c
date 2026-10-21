/**
 * @file settings.c
 * @brief The table of init.lisp settings, read in one place
 * @details Every setting used to be read on its own in init.c, some with
 * lisp_read + lisp_eval (which reports an error and yields junk when the
 * symbol isn't defined, so a config predating a setting misbehaved) and
 * some with a defensive lookup. Now each has an entry here, read the
 * defensive way, with its default and documentation next to it; the same
 * table answers describe-variable and the settings list.
 */

#include "../include/settings.h"
#include "../include/editor_op.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

typedef enum { SETTING_BOOL, SETTING_INT } SettingType;

typedef struct {
  const char *name;
  SettingType type;
  int fallback;
  int min; // SETTING_INT only
  int *target;
  const char *doc;
} Setting;

static Setting s_settings[] = {
    {"TAB-LENGTH", SETTING_INT, 2, 1, &E.constantes.TAB_LENGTH,
     "Spaces TAB inserts, and the width tabs in a file are expanded to."},
    {"QUIT-TIMES", SETTING_INT, 1, 0, &E.constantes.QUIT_TIMES,
     "Extra CTRL-x CTRL-c presses needed to quit with unsaved changes."},
    {"LSP", SETTING_BOOL, 1, 0, &E.constantes.LSP,
     "Offer to start the language server registered for a file's extension."},
    {"LINE-NUMBERS", SETTING_BOOL, 1, 0, &E.constantes.LINE_NUMBERS,
     "Show the line-number gutter."},
    {"WHICH-KEY", SETTING_BOOL, 1, 0, &E.constantes.WHICH_KEY,
     "Show the keys under a pending prefix (CTRL-x ...) and M-x matches."},
    {"LSP-SIGNATURE-HELP", SETTING_BOOL, 1, 0, &E.constantes.LSP_SIGNATURE_HELP,
     "Show the called function's signature when \"(\" or \",\" is typed."},
    {"AUTOSAVE-INTERVAL", SETTING_INT, 15, 0, &E.constantes.AUTOSAVE_INTERVAL,
     "Seconds between autosave copies of modified files (~/.beluga/autosave/); 0 turns it off."},
    {"STICKY-SCROLL", SETTING_BOOL, 1, 0, &E.constantes.STICKY_SCROLL,
     "Pin the definitions enclosing the top of a pane (function, struct) to its first rows."},
    {"BREADCRUMBS", SETTING_BOOL, 1, 0, &E.constantes.BREADCRUMBS,
     "Show the definitions the cursor is in (struct > function) in the status bar."},
    {"INLINE-DIAGNOSTICS", SETTING_BOOL, 1, 0, &E.constantes.INLINE_DIAGNOSTICS,
     "Show a line's error or warning message after its text."},
    {"INLAY-HINTS", SETTING_BOOL, 1, 0, &E.constantes.INLAY_HINTS,
     "Show the server's inlay hints (parameter names, deduced types) inside lines."},
};

#define SETTING_COUNT ((int)(sizeof(s_settings) / sizeof(s_settings[0])))

static char s_invalid[256]; // names settingsLoad() rejected, for the report

static void formatValue(const Setting *s, int value, char *out, size_t size) {
  if (s->type == SETTING_BOOL)
    snprintf(out, size, "%s", value ? "#t" : "#f");
  else
    snprintf(out, size, "%d", value);
}

void settingsLoad(void) {
  int i;
  int present;
  Lisp val;
  Setting *s;
  size_t len;

  s_invalid[0] = '\0';
  for (i = 0; i < SETTING_COUNT; i++) {
    s = &s_settings[i];
    *s->target = s->fallback;
    val = lisp_env_lookup(lisp_env(E.ctx), lisp_make_symbol(s->name, E.ctx), &present);
    if (!present)
      continue;
    if (s->type == SETTING_BOOL) {
      // Anything but #f counts as on, as in Lisp itself.
      *s->target = lisp_is_true(val);
    } else if (lisp_type(val) == LISP_INT && lisp_int(val) >= s->min) {
      *s->target = (int)lisp_int(val);
    } else {
      len = strlen(s_invalid);
      snprintf(s_invalid + len, sizeof(s_invalid) - len, "%s%s", len ? ", " : "", s->name);
    }
  }
}

void settingsReportProblems(void) {
  if (s_invalid[0])
    editorSetStatusMessage("init.lisp: invalid %s, using the default (CTRL-x c s)", s_invalid);
}

static const Setting *findSetting(const char *name) {
  int i;
  for (i = 0; i < SETTING_COUNT; i++)
    if (strcmp(s_settings[i].name, name) == 0)
      return &s_settings[i];
  return NULL;
}

Lisp describeVariable(Lisp args, LispError *e, LispContext ctx) {
  const char *name;
  const Setting *s;
  Lisp lines;
  Lisp val;
  int present;
  char text[512];
  char current[32];
  char fallback[32];
  (void)e;

  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING)
    return lisp_null();
  name = lisp_string(lisp_car(args));
  s = findSetting(name);
  lines = lisp_null();
  if (s) {
    formatValue(s, *s->target, current, sizeof(current));
    formatValue(s, s->fallback, fallback, sizeof(fallback));
    snprintf(text, sizeof(text), "%s is a setting. %s", s->name, s->doc);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    lines = lisp_cons(lisp_make_string2("", ctx), lines, ctx);
    snprintf(text, sizeof(text), "Value: %s   Default: %s", current, fallback);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    snprintf(text, sizeof(text), "Set it in personal-config: (define %s %s)", s->name,
             s->type == SETTING_BOOL ? (*s->target ? "#f" : "#t") : current);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    lines = lisp_cons(lisp_make_string2("Settings are read at startup.", ctx), lines, ctx);
    return lisp_list_reverse(lines);
  }
  val = lisp_env_lookup(lisp_env(ctx), lisp_make_symbol(name, ctx), &present);
  if (!present) {
    snprintf(text, sizeof(text), "%s is not defined.", name);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
  } else {
    snprintf(text, sizeof(text), "%s is a variable (not one of the editor's settings).", name);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    if (lisp_type(val) == LISP_INT) {
      snprintf(text, sizeof(text), "Value: %lld", (long long)lisp_int(val));
      lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    } else if (lisp_type(val) == LISP_STRING) {
      snprintf(text, sizeof(text), "Value: \"%s\"", lisp_string(val));
      lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    } else if (lisp_type(val) == LISP_BOOL) {
      lines = lisp_cons(lisp_make_string2(lisp_is_true(val) ? "Value: #t" : "Value: #f", ctx),
                        lines, ctx);
    }
  }
  return lisp_list_reverse(lines);
}

Lisp settingsList(Lisp args, LispError *e, LispContext ctx) {
  Lisp lines;
  int i;
  const Setting *s;
  char text[512];
  char current[32];
  char fallback[32];
  (void)args;
  (void)e;

  lines = lisp_null();
  snprintf(text, sizeof(text), "%-20s %-6s %-8s %s", "SETTING", "VALUE", "DEFAULT", "");
  lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
  for (i = 0; i < SETTING_COUNT; i++) {
    s = &s_settings[i];
    formatValue(s, *s->target, current, sizeof(current));
    formatValue(s, s->fallback, fallback, sizeof(fallback));
    snprintf(text, sizeof(text), "%-20s %-6s %-8s %s", s->name, current, fallback, s->doc);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
  }
  lines = lisp_cons(lisp_make_string2("", ctx), lines, ctx);
  lines = lisp_cons(lisp_make_string2("Change one with (define NAME value) in personal-config; "
                                      "they're read at startup.", ctx),
                    lines, ctx);
  return lisp_list_reverse(lines);
}
