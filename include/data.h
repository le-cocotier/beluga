#ifndef DATA_H_
#define DATA_H_

#include <termios.h>
#include <time.h>
#include "../blisp/include/data.h"

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
  int quit_times;
  char *filename;
  char status_msg[80];
  time_t status_msg_time;
  struct termios orig_termios; /**< Terminal communication interface */
  config_t *config;
};

/**
 * \struct abuf
 * \brief Contains text to add before writing to screen.
 * */

struct abuf {
  char *b; /**< Text that will be printed */
  int len; /**< Length of the text */
};

// Enhanced key sequence handling for multi-key bindings like "CTRL-f o"
typedef struct {
  char sequence[64];
  int sequence_len;
  int last_key_time; // You might want to add timing if needed
} key_sequence_t;


#endif
