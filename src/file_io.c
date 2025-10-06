#include "../include/file_io.h"
#include "../include/input.h"
#include "../include/output.h"
#include "data.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char *strdup(const char *);
extern ssize_t getline(char **restrict lineptr, size_t *restrict n,
                       FILE *restrict stream);
extern int ftruncate(int fd, off_t length);
extern struct editorConfig E;

char *editorRowsToString(int *buffer_len) {
    int tot_len = 0;
    int j;
    char *buf;
    char *p;

    for (j = 0; j < E.numrows; ++j) {
        tot_len += E.row[j].size + 1;
    }
    *buffer_len = tot_len;
    buf = malloc(tot_len);
    p = buf;
    for (j = 0; j < E.numrows; ++j) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorCloseFile(void) {
    E.cursor_x = 0;
    E.cursor_y = 0;
    E.rx = 0;
    E.row_offset = 0;
    E.col_offset = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.status_msg[0] = '\0';
    E.status_msg_time = 0;
}

void editorOpen(char *filename) {
    FILE *fp;

    // Test if a file is already open
    if (E.filename != NULL) {
        editorCloseFile();
        E.state = READ_AND_WRITE;
    }

    free(E.filename);
    E.filename = strdup(filename);

    fp = fopen(filename, "a+");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 &&
               (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            --line_len;
        }
        editorInsertRow(E.numrows, line, line_len);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    int len;
    char *buf;
    int fd;
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)");
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }
    buf = editorRowsToString(&len);
    fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}
