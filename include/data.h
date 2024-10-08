#ifndef DATA_H_
#define DATA_H_

#include <termios.h>
#include <time.h>

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
  char *filename;
  char status_msg[80];
  time_t status_msg_time;
  struct termios orig_termios; /**< Terminal communication interface */
};

/**
 * \struct abuf
 * \brief Contains text to add before writing to screen.
 * */

struct abuf {
  char *b; /**< Text that will be printed */
  int len; /**< Length of the text */
};

#endif
