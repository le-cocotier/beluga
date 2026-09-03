#include "../include/terminal.h"

#include <ctype.h>

#include "../include/data.h"
#include "../include/define.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../include/buffer.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"

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


void die(const char* s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, CURSOR_TOP_LEFT, 3);
    lisp_shutdown(E.ctx);
    perror(s);
    exit(1);
}

void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    {
        die("tcsetattr");
    }
}

/**
 * @brief Hand the terminal back to the shell in a usable state.
 * @details Order matters: reset SGR *before* clearing, or the theme's
 * background color is what the clear paints the screen with and it stays
 * as the shell's background afterward. Also re-shows the cursor, which
 * editorRefreshScreen() hides on every frame — quitting mid-frame used to
 * leave an invisible cursor behind. Safe to call more than once.
 */
void terminalRestore(void)
{
    write(STDOUT_FILENO, "\x1b[0m", 4);   // reset colors/attributes
    write(STDOUT_FILENO, "\x1b[2J", 4);   // clear screen
    write(STDOUT_FILENO, CURSOR_TOP_LEFT, 3);
    write(STDOUT_FILENO, SHOW_CURSOR, 6);
    disableRawMode();
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    {
        die("tcgetattr");
    }

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    {
        die("tcgetattr");
    }
}

char* keyToString(Utf8Char* key)
{
    static char key_str[32];
    unsigned char c = key->character[0];

    if (c == '\r')
    {
        strcpy(key_str, "ENTER");
    }
    else if (c == 0x09)
    {
        strcpy(key_str, "TAB");
    }
    else if (c == 0x00)
    {
        // Ctrl+Space (and, on most terminals, indistinguishably Ctrl+@)
        // sends a literal NUL byte — Space is 0x20, and Ctrl clears bits
        // 5-6, landing on 0. Named separately from the CTRL-<letter> range
        // just below (which starts at 1, deliberately excluding this):
        // 0 isn't "CTRL-`" in any convention a user would actually type.
        strcpy(key_str, "CTRL-SPACE");
    }
    else if (c >= 1 && c <= 26)
    {
        snprintf(key_str, sizeof(key_str), "CTRL-%c", 'a' + c - 1);
    }
    else
    {
        switch (c)
        {
        case ARROW_UP:    strcpy(key_str, "ARROW-UP");    break;
        case ARROW_DOWN:  strcpy(key_str, "ARROW-DOWN");  break;
        case ARROW_LEFT:  strcpy(key_str, "ARROW-LEFT");  break;
        case ARROW_RIGHT: strcpy(key_str, "ARROW-RIGHT"); break;
        case PAGE_UP:     strcpy(key_str, "PAGE-UP");     break;
        case PAGE_DOWN:   strcpy(key_str, "PAGE-DOWN");   break;
        case DEL_KEY:     strcpy(key_str, "DEL");         break;
        case BACKSPACE:   strcpy(key_str, "BACKSPACE");   break;
        case BEG_LINE:    strcpy(key_str, "HOME");        break;
        case END_LINE:    strcpy(key_str, "END");         break;
        case ALT_X:       strcpy(key_str, "ALT-x");       break;
        case '\x1b':      strcpy(key_str, "ESCAPE");      break;
        default:
            if (key->length > 1)
            {
                /* Genuine multi-byte UTF-8 codepoint: bytes are already
                   stored raw in key->character, no re-encoding needed. */
                int n = key->length;
                memcpy(key_str, key->character, n);
                key_str[n] = '\0';
            }
            else if (isprint(c))
            {
                snprintf(key_str, sizeof(key_str), "%c", c);
            }
            else
            {
                snprintf(key_str, sizeof(key_str), "KEY-%d", c);
            }
        }
    }
    return key_str;
}

/**
 * @note Memory allocated for the return char
 */
Utf8Char * editorReadKey()
{
    Utf8Char* c;
    int nread;

    c = readUtf8Char();
    return c;
}

int getCursorPosition(int* rows, int* cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    {
        return -1;
    }

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
        {
            break;
        }
        if (buf[i] == 'R')
        {
            break;
        }
        ++i;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
    {
        return -1;
    }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    {
        return -1;
    }

    return 0;
}

int getWindowSize(int* rows, int* cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
        {
            return -1;
        }
        return getCursorPosition(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

void appDebug(const char* fmt, ...)
{
#ifdef APP_DEBUG
    va_list ap;
    char message[1024];
    va_start(ap, fmt);
    vsnprintf(message, 1024, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\r\n", message);
#endif
}
