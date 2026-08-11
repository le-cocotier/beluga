#ifndef COLOR_H_
#define COLOR_H_


// ============================================================================
// TEXT STYLES / ATTRIBUTES
// ============================================================================
#define ANSI_RESET          "\x1b[0m"      // Reset all attributes
#define ANSI_BOLD           "\x1b[1m"      // Bold text
#define ANSI_DIM            "\x1b[2m"      // Dim/faint text
#define ANSI_ITALIC         "\x1b[3m"      // Italic text
#define ANSI_UNDERLINE      "\x1b[4m"      // Underline text
#define ANSI_BLINK          "\x1b[5m"      // Blinking text
#define ANSI_REVERSE        "\x1b[7m"      // Reverse video (invert colors)
#define ANSI_HIDDEN         "\x1b[8m"      // Hidden/invisible text
#define ANSI_STRIKETHROUGH  "\x1b[9m"      // Strikethrough text

// ============================================================================
// FOREGROUND COLORS (30-37 standard, 90-97 bright)
// ============================================================================
#define ANSI_BLACK          "\x1b[30m"
#define ANSI_RED            "\x1b[31m"
#define ANSI_GREEN          "\x1b[32m"
#define ANSI_YELLOW         "\x1b[33m"
#define ANSI_BLUE           "\x1b[34m"
#define ANSI_MAGENTA        "\x1b[35m"
#define ANSI_CYAN           "\x1b[36m"
#define ANSI_WHITE          "\x1b[37m"

// Bright/Light foreground colors (90-97)
#define ANSI_BRIGHT_BLACK   "\x1b[90m"
#define ANSI_BRIGHT_RED     "\x1b[91m"
#define ANSI_BRIGHT_GREEN   "\x1b[92m"
#define ANSI_BRIGHT_YELLOW  "\x1b[93m"
#define ANSI_BRIGHT_BLUE    "\x1b[94m"
#define ANSI_BRIGHT_MAGENTA "\x1b[95m"
#define ANSI_BRIGHT_CYAN    "\x1b[96m"
#define ANSI_BRIGHT_WHITE   "\x1b[97m"

// ============================================================================
// BACKGROUND COLORS (40-47 standard, 100-107 bright)
// ============================================================================
#define ANSI_BG_BLACK       "\x1b[40m"
#define ANSI_BG_RED         "\x1b[41m"
#define ANSI_BG_GREEN       "\x1b[42m"
#define ANSI_BG_YELLOW      "\x1b[43m"
#define ANSI_BG_BLUE        "\x1b[44m"
#define ANSI_BG_MAGENTA     "\x1b[45m"
#define ANSI_BG_CYAN        "\x1b[46m"
#define ANSI_BG_WHITE       "\x1b[47m"

// Bright/Light background colors (100-107)
#define ANSI_BG_BRIGHT_BLACK   "\x1b[100m"
#define ANSI_BG_BRIGHT_RED     "\x1b[101m"
#define ANSI_BG_BRIGHT_GREEN   "\x1b[102m"
#define ANSI_BG_BRIGHT_YELLOW  "\x1b[103m"
#define ANSI_BG_BRIGHT_BLUE    "\x1b[104m"
#define ANSI_BG_BRIGHT_MAGENTA "\x1b[105m"
#define ANSI_BG_BRIGHT_CYAN    "\x1b[106m"
#define ANSI_BG_BRIGHT_WHITE   "\x1b[107m"

// ============================================================================
// 256-COLOR MODE (for more colors)
// ============================================================================
// Foreground: \x1b[38;5;Nm  where N is 0-255
// Background: \x1b[48;5;Nm  where N is 0-255
// Example macros:
#define ANSI_FG_256(N)      "\x1b[38;5;" #N "m"
#define ANSI_BG_256(N)      "\x1b[48;5;" #N "m"

// ============================================================================
// TRUE COLOR / 24-BIT COLOR (RGB)
// ============================================================================
// Foreground: \x1b[38;2;R;G;Bm
// Background: \x1b[48;2;R;G;Bm
// Example macros:
#define ANSI_FG_RGB(R,G,B)  "\x1b[38;2;" #R ";" #G ";" #B "m"
#define ANSI_BG_RGB(R,G,B)  "\x1b[48;2;" #R ";" #G ";" #B "m"

// ============================================================================
// CURSOR MOVEMENT
// ============================================================================
#define ANSI_CURSOR_UP(N)       "\x1b[" #N "A"    // Move up N lines
#define ANSI_CURSOR_DOWN(N)     "\x1b[" #N "B"    // Move down N lines
#define ANSI_CURSOR_RIGHT(N)    "\x1b[" #N "C"    // Move right N columns
#define ANSI_CURSOR_LEFT(N)     "\x1b[" #N "D"    // Move left N columns
#define ANSI_CURSOR_HOME        "\x1b[H"          // Move to home (0,0)
#define ANSI_CURSOR_HIDE        "\x1b[?25l"       // Hide cursor
#define ANSI_CURSOR_SHOW        "\x1b[?25h"       // Show cursor

// ============================================================================
// SCREEN CONTROL
// ============================================================================
#define ANSI_CLEAR_SCREEN   "\x1b[2J"      // Clear entire screen
#define ANSI_CLEAR_LINE     "\x1b[K"       // Clear from cursor to end of line
#define ANSI_CLEAR_UP       "\x1b[1J"      // Clear from cursor to start
#define ANSI_CLEAR_DOWN     "\x1b[0J"      // Clear from cursor to end

// ============================================================================
// EXAMPLE USAGE
// ============================================================================
/*
#include <stdio.h>

int main(void) {
    // Simple color example
    printf("%sHello in Red%s\n", ANSI_RED, ANSI_RESET);
    
    // Bold and colored
    printf("%s%sBold Green%s\n", ANSI_BOLD, ANSI_GREEN, ANSI_RESET);
    
    // Background color
    printf("%s%sYellow on Blue%s\n", ANSI_YELLOW, ANSI_BG_BLUE, ANSI_RESET);
    
    // Combine multiple styles
    printf("%s%s%sUnderlined Bold Cyan%s\n", 
           ANSI_UNDERLINE, ANSI_BOLD, ANSI_CYAN, ANSI_RESET);
    
    return 0;
}
*/

#endif
