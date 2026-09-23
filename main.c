/**
 * \file main.c
 * \author Arthur Barraux
 * \brief Base file of Beluga text editor. Contain functions for terminal
 * interactions. \version 0.1 \date 21 septembre 2024
 */

#include <stdlib.h>
#include <libgen.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "include/define.h"
#include "include/galloc.h"
#include "include/utf8.h"
#include "include/data.h"
#include "include/init.h"
#include "include/editor_op.h"
#include "include/terminal.h"
#include "include/builtins.h"
#include "include/buffer.h"
#include "include/split_screen.h"

EditorConfig E;
int buffer_id = 0;

int main(const int argc, char *argv[]) {
    const char *home;
    Utf8Char* splash_screen;
    EditorPane *active;
    Utf8Char* filename;
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("Beluga %s\n", BELUGA_VERSION);
        return 0;
    }

    // Get HOME with NULL check
    home = getenv("HOME");
    splash_screen = gcalloc(PATH_MAX, sizeof(Utf8Char));
    active = NULL;

    if (!home) {
        fprintf(stderr, "Error: HOME environment variable not set\n");
        return 1;
    }
    // Allocate and build splash screen path safely
    utf8Printf(splash_screen, PATH_MAX, "%s/.beluga/assets/beluga.txt", home);

    signal(SIGPIPE, SIG_IGN); // writing to a dead PTY must not kill the editor
    enableRawMode();
    initEditor();

    active = splitScreenGetActivePane();
    active->buffer_id = bufferCreate(splash_screen, READ_ONLY, FILE_BUFF);

    if (argc >= 2) {
        filename = stringToUtf8((unsigned char *)argv[1]);
        active->buffer_id = bufferCreate(filename, READ_AND_WRITE, FILE_BUFF);
        gfree(filename);
    }

    gfree(splash_screen);  // Now guaranteed safe
    editorSetStatusMessage("HELP: Ctrl-x Ctrl-s = save | Ctrl-x Ctrl-c = quit");

    while (!E.exit_requested) {
        editorRunLoop();
    }

    // Only reached once editorRunLoop() has actually returned — no Lisp
    // evaluation is on the stack at this point, so it's safe to tear down
    // the interpreter here (editorQuit() used to do this while still
    // inside a live lisp_apply call, which was a use-after-free).
    deInitEditor();
    bFree_structs();
    return 0;
}
