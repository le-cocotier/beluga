/**
 * @file describe.c
 * @brief Self-documentation: describe-key, key-bindings, describe-function
 * @details Names come from the which-key panel's own resolution (a
 * binding's command is stored evaluated, so its name is found by looking
 * the value up in the global environment), so what these report matches
 * what the panel shows.
 */

#include "../include/describe.h"
#include "../include/builtins.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/output.h"
#include "../include/terminal.h"
#include "../include/which_key.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

// Defined by lisp.h's implementation (not declared in its interface): a
// lambda's parameter list, for printing it back as source.
Lisp lambda_args_(Lisp l);

#define DESCRIBE_MAX_KEYS 8

static const char *prefixName(const int prefix_id) {
  if (prefix_id < 0 || prefix_id >= E.number_of_prefix)
    return "?";
  return E.prefix[prefix_id].prefix_name;
}

/**
 * @brief The binding executeKeyBind() would run for `key` with `prefix_id`
 * pending: the first one for that key under the prefix or under "*".
 */
static int findBinding(const char *key, const int prefix_id, const int star_id) {
  int i;
  for (i = 0; i < E.number_of_keybinds; i++) {
    if (strcmp(E.key_binds[i].key_sequence, key) != 0)
      continue;
    if (E.key_binds[i].prefix_id == prefix_id || E.key_binds[i].prefix_id == star_id)
      return i;
  }
  return -1;
}

Lisp describeKey(Lisp args, LispError *e, LispContext ctx) {
  char keys[256];
  char prefix[64]; // find_prefix() takes a full 64-byte name
  const char *name;
  const char *command;
  Utf8Char *c;
  int prefix_id;
  int star_id;
  int index;
  int count;
  size_t len;
  (void)args;
  (void)e;
  (void)ctx;

  memset(prefix, 0, sizeof(prefix));
  prefix[0] = '*';
  star_id = find_prefix(prefix).prefix_id;
  // Whatever prefix got this command here (M-x, or CTRL-x <key>) is done.
  E.prefix_state = 0;
  prefix_id = 0; // "no-prefix"
  keys[0] = '\0';
  editorSetStatusMessage("Describe key: ");
  editorRefreshScreen();

  for (count = 0; count < DESCRIBE_MAX_KEYS;) {
    c = editorReadKey();
    if (!c)
      break;
    if (c->character[0] == 0) { // read timed out, no key
      gfree(c);
      continue;
    }
    name = keyToString(c);
    count++;
    len = strlen(keys);
    snprintf(keys + len, sizeof(keys) - len, "%s%s", len ? " " : "", name);
    index = findBinding(name, prefix_id, star_id);
    gfree(c);

    if (index < 0) {
      editorSetStatusMessage("%s is not bound", keys);
      return lisp_null();
    }
    command = whichKeyBindingName(index);
    if (command && strncmp(command, "editor-set-prefix ", 18) == 0 &&
        strcmp(command + 18, "no-prefix") != 0) {
      // A prefix key: keep reading under the prefix it sets.
      memset(prefix, 0, sizeof(prefix));
      snprintf(prefix, sizeof(prefix), "%s", command + 18);
      prefix_id = find_prefix(prefix).prefix_id;
      editorSetStatusMessage("Describe key: %s", keys);
      editorRefreshScreen();
      continue;
    }
    editorSetStatusMessage("%s runs %s%s", keys, command ? command : "?",
                           E.key_binds[index].prefix_id == star_id ? " (in every prefix)" : "");
    return lisp_null();
  }
  editorSetStatusMessage("%s: too many keys", keys);
  return lisp_null();
}

Lisp keyBindings(Lisp args, LispError *e, LispContext ctx) {
  Lisp list;
  Lisp entry;
  const char *command;
  int i;
  (void)args;
  (void)e;

  list = lisp_null();
  for (i = E.number_of_keybinds - 1; i >= 0; i--) {
    command = whichKeyBindingName(i);
    entry = lisp_cons(lisp_make_string2(prefixName(E.key_binds[i].prefix_id), ctx),
                      lisp_cons(lisp_make_string2(strcmp(E.key_binds[i].key_sequence, " ") == 0
                                                      ? "SPACE" : E.key_binds[i].key_sequence, ctx),
                                lisp_cons(lisp_make_string2(command ? command : "?", ctx),
                                          lisp_null(), ctx),
                                ctx),
                      ctx);
    list = lisp_cons(entry, list, ctx);
  }
  return list;
}

/**
 * @brief Appends the printed form of `value` to `lines`, one string per
 * line (lisp_printf only writes to a FILE).
 */
static Lisp appendPrinted(Lisp lines, Lisp value, LispContext ctx) {
  FILE *f;
  char line[512];
  size_t n;
  f = tmpfile();
  if (!f)
    return lines;
  lisp_printf(f, value);
  fputc('\n', f);
  rewind(f);
  while (fgets(line, sizeof(line), f)) {
    n = strlen(line);
    if (n > 0 && line[n - 1] == '\n')
      line[n - 1] = '\0';
    lines = lisp_cons(lisp_make_string2(line, ctx), lines, ctx);
  }
  fclose(f);
  return lines;
}

Lisp describeFunction(Lisp args, LispError *e, LispContext ctx) {
  const char *name;
  Lisp value;
  Lisp lines;
  int present;
  int i;
  int bound;
  const char *command;
  char text[512];
  (void)e;

  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING)
    return lisp_null();
  name = lisp_string(lisp_car(args));
  value = lisp_env_lookup(lisp_env(ctx), lisp_make_symbol(name, ctx), &present);

  lines = lisp_null();
  if (!present) {
    snprintf(text, sizeof(text), "%s is not defined.", name);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    return lisp_list_reverse(lines);
  }
  if (lisp_type(value) == LISP_FUNC) {
    snprintf(text, sizeof(text), "%s is a built-in function (C).", name);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
  } else if (lisp_type(value) == LISP_LAMBDA) {
    snprintf(text, sizeof(text), "%s is a Lisp function:", name);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    lines = lisp_cons(lisp_make_string2("", ctx), lines, ctx);
    lines = appendPrinted(lines,
                          lisp_cons(lisp_make_symbol("lambda", ctx),
                                    lisp_cons(lambda_args_(value),
                                              lisp_cons(lisp_lambda_body(value), lisp_null(), ctx),
                                              ctx),
                                    ctx),
                          ctx);
  } else {
    snprintf(text, sizeof(text), "%s is a variable, not a function. Its value:", name);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    lines = appendPrinted(lines, value, ctx);
  }

  lines = lisp_cons(lisp_make_string2("", ctx), lines, ctx);
  bound = 0;
  for (i = 0; i < E.number_of_keybinds; i++) {
    command = whichKeyBindingName(i);
    if (!command || strcmp(command, name) != 0)
      continue;
    snprintf(text, sizeof(text), "  %-12s %s", prefixName(E.key_binds[i].prefix_id),
             E.key_binds[i].key_sequence);
    if (!bound)
      lines = lisp_cons(lisp_make_string2("Bound to (prefix, key):", ctx), lines, ctx);
    lines = lisp_cons(lisp_make_string2(text, ctx), lines, ctx);
    bound = 1;
  }
  if (!bound)
    lines = lisp_cons(lisp_make_string2("Not bound to any key (ALT-x runs it).", ctx), lines, ctx);
  return lisp_list_reverse(lines);
}
