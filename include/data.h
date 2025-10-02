#ifndef DATA_H_
#define DATA_H_

#include <stdio.h>
#include <termios.h>
#include <time.h>

#include "../lisp-interpreter/dist/lisp.h"

/**
 * \struct erow
 * \brief Store one editor row
 * \param
 * */

typedef struct erow {
  int size;     /**< Size of the line */
  int rsize;    /**< Size of the render line */
  char *chars;  /**< Characters of the line */
  char *render; /**< The actual line we will print */
} erow;


struct const_t {
  int TAB_LENGTH;
  int QUIT_TIMES;
};

struct keyBind_t {
  char *key_sequence;
  Lisp command;
  
};

/**
 * \struct editorConfig
 * \brief Containing our editor state.
 */
struct editorConfig {
  int cursor_x, cursor_y; /**< Cursor position */
  int rx;                 /**< Position in the render*/
  int row_offset;         /**< Position scroll of lines */
  int col_offset;         /**< Position scroll of colomns*/
  int screenrows;         /**< Terminal height*/
  int screencols;         /**< Terminal width*/
  int numrows;            /**< Number of rows contained */
  erow *row;              /**< Store all the rows printed */
  int dirty;
  char *filename;
  char status_msg[80];
  time_t status_msg_time;
  struct termios orig_termios; /**< Terminal communication interface */

  struct const_t constantes;
  int quit_times_buffer;

  FILE *fd_init_file;
  Lisp env;
  LispContext ctx;     /** Lisp context */
  Lisp ctx_data;       /** Lisp data context */
  LispError ctx_error; /** Lisp ctx error */

  struct keyBind_t* key_binds;
  int number_of_keybinds;
};

/**
 * \struct abuf
 * \brief Contains text to add before writing to screen.
 * */

struct abuf {
  char *b; /**< Text that will be printed */
  int len; /**< Length of the text */
};

extern struct editorConfig E;

#endif
