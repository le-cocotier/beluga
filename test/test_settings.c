/**
 * @file test_settings.c
 * @brief Unit tests for reading the init.lisp settings (settings.c):
 * defaults when a setting isn't defined, booleans, the integer minimums,
 * and the names reported as invalid.
 */

#include "test.h"
#include "../include/galloc.h"
#include "../include/lisp.h"
#include "../include/lisp_lib.h"
#include "../include/settings.h"

static void define(const char *code) {
  LispError error = LISP_ERROR_NONE;
  lisp_eval(lisp_read(code, &error, E.ctx), &error, E.ctx);
  if (error != LISP_ERROR_NONE) {
    fprintf(stderr, "%s: %s\n", code, lisp_error_string(error));
    test_failures++;
  }
}

static void testDefaults(void) {
  struct const_t out;
  char invalid[256] = "";

  memset(&out, 0x7f, sizeof(out));
  settingsRead(lisp_env(E.ctx), &out, invalid, sizeof(invalid));
  CHECK_INT(out.TAB_LENGTH, 2);
  CHECK_INT(out.QUIT_TIMES, 1);
  CHECK_INT(out.AUTOSAVE_INTERVAL, 15);
  CHECK_INT(out.LSP, 1);
  CHECK_INT(out.LINE_NUMBERS, 1);
  CHECK_INT(out.MOUSE, 1);
  CHECK_INT(out.EXTENDED_KEYS, 1);
  CHECK_STR(invalid, "");
}

static void testValues(void) {
  struct const_t out;
  char invalid[256] = "";

  define("(define TAB-LENGTH 4)");
  define("(define QUIT-TIMES 0)");         // 0 is allowed here
  define("(define AUTOSAVE-INTERVAL 0)");  // and here: autosave off
  define("(define LSP #f)");
  define("(define MOUSE '())");            // anything but #f is on
  define("(define LINE-NUMBERS 0)");
  settingsRead(lisp_env(E.ctx), &out, invalid, sizeof(invalid));
  CHECK_INT(out.TAB_LENGTH, 4);
  CHECK_INT(out.QUIT_TIMES, 0);
  CHECK_INT(out.AUTOSAVE_INTERVAL, 0);
  CHECK_INT(out.LSP, 0);
  CHECK_INT(out.MOUSE, 1);
  CHECK_INT(out.LINE_NUMBERS, 1);
  CHECK_INT(out.WHICH_KEY, 1); // untouched settings keep their default
  CHECK_STR(invalid, "");
}

static void testInvalid(void) {
  struct const_t out;
  char invalid[256] = "";
  char tiny[12] = "";

  define("(define TAB-LENGTH 0)");         // below its minimum of 1
  define("(define QUIT-TIMES \"three\")"); // not an integer
  define("(define AUTOSAVE-INTERVAL -5)");
  settingsRead(lisp_env(E.ctx), &out, invalid, sizeof(invalid));
  // Invalid values fall back to the default and are named.
  CHECK_INT(out.TAB_LENGTH, 2);
  CHECK_INT(out.QUIT_TIMES, 1);
  CHECK_INT(out.AUTOSAVE_INTERVAL, 15);
  CHECK_STR(invalid, "TAB-LENGTH, QUIT-TIMES, AUTOSAVE-INTERVAL");

  // A short report buffer is cut, not overrun.
  settingsRead(lisp_env(E.ctx), &out, tiny, sizeof(tiny));
  CHECK_INT(strlen(tiny), sizeof(tiny) - 1);
  CHECK(strncmp(tiny, "TAB-LENGTH,", 11) == 0);

  // No report buffer at all is fine too.
  settingsRead(lisp_env(E.ctx), &out, NULL, 0);
  CHECK_INT(out.TAB_LENGTH, 2);
}

static void testTemplate(void) {
  FILE *f = tmpfile();
  char text[8192];
  size_t n;

  E.global_constantes.TAB_LENGTH = 8;
  E.global_constantes.LSP = 0;
  E.global_constantes.MOUSE = 1;
  settingsWriteTemplate(f);
  rewind(f);
  n = fread(text, 1, sizeof(text) - 1, f);
  text[n] = '\0';
  fclose(f);
  // Every setting is there, commented out, with init.lisp's value.
  CHECK(strstr(text, ";; (define TAB-LENGTH 8)\n") != NULL);
  CHECK(strstr(text, ";; (define LSP #f)\n") != NULL);
  CHECK(strstr(text, ";; (define MOUSE #t)\n") != NULL);
  CHECK(strstr(text, ";; Spaces TAB inserts") != NULL);
  CHECK(strstr(text, "\n(define") == NULL);
}

int main(void) {
  testInit();
  E.ctx = lisp_init();
  lisp_set_stderr(tmpfile(), E.ctx);
  lisp_lib_load(E.ctx);
  testDefaults();
  testValues();
  testInvalid();
  testTemplate();
  lisp_shutdown(E.ctx);
  return testDone();
}
