/**
 * @file buffer.c
 * @brief Buffer management implementation for multiple open files
 */

#include "../include/buffer.h"
#include "../include/file_io.h"
#include "../include/editor_op.h"
#include "../include/data.h"
#include "../include/split_screen.h"
#include "../include/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/utf8.h"
#include "../include/completion.h"
#include "../include/input.h"
#include "../include/append_buffer.h"
#include "../include/terminal.h"
#include "../include/pty_session.h"
#include "../include/builtins.h"

/***********************************************************************************************************************
 *
 *                                                 extern definitions
 *
 **********************************************************************************************************************/

extern EditorConfig E;
extern int buffer_id;

/***********************************************************************************************************************
 *
 *                                                 functions definitions
 *
 **********************************************************************************************************************/


/**
 * @brief Finds a buffer by filename
 * @param filename The filename to search for
 * @return Buffer ID if found, -1 if not found
 */
static int bufferFindByFilename(Utf8Char* filename)
{
    if (E.buffers == NULL) return -1;
    int first_buffer_id = E.buffers->buffer_id;
    Buffer* buffer = E.buffers;
    do
    {
        if (buffer->filename != NULL &&
            utf8StringCmp(buffer->filename, filename) == 0)
        {
            return buffer->buffer_id;
        }
        buffer = buffer->next;
    } while (buffer->buffer_id != first_buffer_id);
    return -1;
}

/**
 * @brief Finds another FILE_BUFF buffer that's already LSP-synced
 * (b_lsp_open) for the same file, by full path
 * @details Now that the same file can be open in more than one buffer
 * (see bufferCreate()), this is what keeps LSP sync correct: a server
 * only expects one textDocument/didOpen per URI (and, symmetrically,
 * exactly one didClose once every buffer viewing it is gone) — it has
 * no notion of "beluga has this file open twice." Both
 * bufferMaybeSyncLsp() and lspShutdown()'s didClose loop use this to
 * only actually notify the server on the first open / last close of a
 * given URI, regardless of how many buffers are viewing it or what
 * order they're processed in.
 * @param buf The buffer to find a sibling for (excluded from the search)
 * @return buffer_id of the other buffer, or -1 if none
 */
int bufferFindOtherLspOpen(struct buffer_t* buf)
{
    if (E.buffers == NULL || !buf->fullname) return -1;
    int first_buffer_id = E.buffers->buffer_id;
    struct buffer_t* b = E.buffers;
    do
    {
        if (b->buffer_id != buf->buffer_id && b->type == FILE_BUFF &&
            b->b_lsp_open && b->fullname &&
            utf8StringCmp(b->fullname, buf->fullname) == 0)
        {
            return b->buffer_id;
        }
        b = b->next;
    } while (b->buffer_id != first_buffer_id);
    return -1;
}

/**
 * @brief Finds a buffer by ID
 * @param buffer_id The buffer ID to find
 * @return Pointer to buffer, NULL if not found
 */
struct buffer_t* bufferFindById(int buffer_id)
{
    if (E.buffers == NULL) return NULL;
    int first_buffer_id = E.buffers->buffer_id;
    Buffer* buffer = E.buffers;
    do
    {
        if (buffer->buffer_id == buffer_id)
        {
            return buffer;
        }
        buffer = buffer->next;
    } while (buffer->buffer_id != first_buffer_id);
    return NULL;
}

/**
 * @brief Creates a new buffer for a file or terminal
 * @details Allocates buffer slot, loads file content (file buffers only),
 * saves buffer metadata. If a file buffer for this path is already open,
 * switches to the existing buffer instead.
 * @param path Absolute/relative file path (FILE_BUFF) or plain display name (TERMINAL_BUFF)
 * @param state Read-only / read-write state
 * @param type FILE_BUFF or TERMINAL_BUFF
 * @return Buffer ID on success, -1 on failure
 */
int bufferCreate(const Utf8Char* path, enum bufferStatus_e state, enum buffer_type type)
{
    appDebug("Creating new buffer");

    Utf8Char* filename = NULL;
    char * string_filename = NULL;

    if (type == FILE_BUFF)
    {
        Utf8Char* last_slash = lastSlashUtf8(path);
        const Utf8Char* name_start = last_slash ? last_slash + 1 : path;

        filename = (Utf8Char*) calloc(utf8StringLen(name_start), sizeof(Utf8Char));
        if (!filename)
            return -1;
        utf8Cpy(filename, name_start);
    }
    else if (type == TERMINAL_BUFF) // path is already just a plain name, no splitting needed
    {
        filename = (Utf8Char*) stringToUtf8("term");
        if (!filename)
            return -1;
    }
    else // EXPLORER_BUFF: `path` IS the buffer's display name directly
         // (e.g. "explorer", "*tree*") — not a real filesystem path, so
         // no slash-splitting either. Distinct names let multiple kinds
         // of scratch buffer (explorer sidebar, tree dump, ...) coexist
         // without bufferFindByFilename() treating them as the same one.
    {
        filename = (Utf8Char*) calloc(utf8StringLen(path), sizeof(Utf8Char));
        if (!filename)
            return -1;
        utf8Cpy(filename, path);
    }

    // FILE_BUFF deliberately always creates a fresh, independent buffer —
    // the same file can be open in several buffers at once (each with its
    // own cursor/scroll/dirty state, editorOpen() re-reading the file
    // into its own row array), rather than jumping back to whichever
    // buffer already has it open. TERMINAL_BUFF/EXPLORER_BUFF keep the
    // dedup: reusing the one "term" buffer or a singleton scratch buffer
    // like "*explorer*"/"*tree*" by name is the whole point there.
    if (type != FILE_BUFF)
    {
        const int existing_id = bufferFindByFilename(filename);
        if (existing_id != -1)
        {
            free(filename);
            return bufferSwitch(existing_id);
        }
    }

    if (E.number_of_buffer >= 64)
    {
        editorSetStatusMessage("Error: maximum buffers reached (64)");
        free(filename);
        return -1;
    }

    struct buffer_t* new_buf = calloc(1, sizeof(struct buffer_t));
    new_buf->buffer_id = buffer_id++;
    new_buf->type = type;
    new_buf->state = state;
    new_buf->x = 0;
    new_buf->y = 0;
    new_buf->dirty = 0;
    new_buf->b_lsp_open = 0;

    new_buf->filename = calloc(utf8StringLen(filename), sizeof(Utf8Char));
    if (!new_buf->filename)
    {
        free(filename);
        return -1;
    }
    utf8Cpy(new_buf->filename, filename);
    free(filename); // done with the local copy either way

    E.number_of_buffer++;

    if (type == FILE_BUFF)
    {
        char fullname[PATH_MAX + 1];
        char* path_str = (char*) utf8ToString(path);

        if (realpath(path_str, fullname) == NULL)
        {
            // File doesn't exist yet, or path invalid — fall back to the raw path
            snprintf(fullname, sizeof(fullname), "%s", path_str);
        }
        free(path_str);

        new_buf->fullname = stringToUtf8((unsigned char*) fullname);
        new_buf->path = stringToUtf8((unsigned char*) dirname(fullname));

        // bufferMaybeStartLsp() checks itself whether a server is
        // registered for this extension — no need to gate the call here too.
        bufferMaybeStartLsp(new_buf);

        appDebug("opening file");
        editorOpen(new_buf);
        appDebug("buffer opened");

        // Only meaningful once the server's already running for an earlier
        // file — needs the text just loaded above, so it can't run any
        // earlier than this.
        bufferMaybeSyncLsp(new_buf);

        // Callers traditionally do `active->buffer_id = bufferCreate(...)`
        // themselves, but only after this function returns — too late for
        // a "buffer-opened" hook. Anything the hook calls that resolves
        // "the current buffer" via the active pane (line-text, cursor-row,
        // editor-move-cursor-*, ...) needs this buffer to already be
        // active, or it silently acts on whatever was open before.
        splitScreenGetActivePane()->buffer_id = new_buf->buffer_id;

        char *fullname_str = (char*) utf8ToString(new_buf->fullname);
        Lisp hook_args = lisp_cons(lisp_make_string2(fullname_str, E.ctx),
                                   lisp_cons(lisp_make_int(new_buf->buffer_id), lisp_null(), E.ctx),
                                   E.ctx);
        runHooks("buffer-opened", hook_args);
        free(fullname_str);
    }
    else if (type == TERMINAL_BUFF) // no real filesystem path — don't touch realpath/dirname
    {
        new_buf->child_pid = -1;
           new_buf->pty_master_fd = -1;
           new_buf->path = NULL;
           new_buf->fullname = stringToUtf8("term");

           EditorPane* active = splitScreenGetActivePane();
           if (ptySessionSpawn(new_buf, NULL, active->height, active->width) != 0)
           {
               editorSetStatusMessage("Error: failed to start shell");
               E.number_of_buffer--; // undo the early increment above — this buffer never came up
               return -1;
           }
           struct prefix_t prefix = find_prefix("term");
           E.prefix_state = prefix.prefix_id;
    }
    else // EXPLORER_BUFF: no real filesystem path, no rows yet — Lisp
         // populates content via editor-scratch-set-lines. Doesn't touch
         // the active pane or prefix state itself; callers that need the
         // sidebar split or "explorer" keybind routing set those up
         // explicitly (see editor-explorer-sidebar-open), since a scratch
         // buffer can also just be opened plainly (e.g. a tree dump).
    {
        new_buf->child_pid = -1;
        new_buf->pty_master_fd = -1;
        new_buf->path = NULL;
        new_buf->fullname = NULL;
        new_buf->row = NULL;
        new_buf->numrows = 0;
    }

    filename = utf8ToString(new_buf->filename);
    editorSetStatusMessage("Opened: %s (buffer %d)",
                           (char*) filename, new_buf->buffer_id);
    free(filename);
    if (E.buffers != NULL)
    {
        // Splice new_buf in right after E.buffers. The node that used to
        // be there (old_next) needs its ->prev fixed up too, or walking
        // backward from it forever finds the node that's no longer
        // actually its predecessor — without this, the very first buffer
        // ever created ends up with a ->prev that permanently points to
        // itself instead of wrapping around to the last one.
        struct buffer_t* old_next = E.buffers->next;
        new_buf->prev = E.buffers;
        new_buf->next = old_next;
        E.buffers->next = new_buf;
        old_next->prev = new_buf;
        E.buffers = new_buf;
    } else {
        new_buf->next = new_buf;
        new_buf->prev = new_buf;
        E.buffers = new_buf;
    }
    return new_buf->buffer_id;
}

/**
 * @brief Switches to a specific buffer by ID
 * @details Saves current buffer state, loads target buffer content and state.
 * @param buffer_id The buffer ID to switch to
 * @return 0 on success, -1 on failure
 */
int bufferSwitch(int buffer_id)
{
    struct buffer_t* target = NULL;
    target = bufferFindById(buffer_id);
    if (target == NULL)
    {
        editorSetStatusMessage("Error: buffer not found");
        return -1;
    }
    E.layout.panes[E.layout.active_pane].buffer_id = buffer_id;

    editorSetStatusMessage("Switched to: %u (buffer %d)",
                           target->filename, buffer_id);
    return 0;
}

/**
 * @brief Closes a buffer by ID
 * @details Frees buffer resources and removes from buffer list.
 * Prompts to save unsaved changes. If closing current buffer,
 * switches to next available buffer.
 * @param buffer_id The buffer ID to close
 * @return 0 on success, -1 on failure
 */
int bufferClose(int buffer_id)
{
    struct buffer_t* buf = bufferFindById(buffer_id);
    if (buf == NULL)
    {
        editorSetStatusMessage("Error: buffer not found");
        return -1;
    }

    EditorPane* active = splitScreenGetActivePane();

    // If this is the current buffer, find next buffer to switch to
    if (active->buffer_id == buffer_id)
    {
        int next_id = -1;
        if (buf->next != NULL)
        {
            next_id = buf->next->buffer_id;
        }

        if (next_id != -1)
        {
            bufferSwitch(next_id);
        }
        else
        {
            // No other buffers, clear editor
            editorCloseFile();
        }
    }

    // Free buffer resources
    free(buf->filename);
    buf->filename = NULL;
    buf->buffer_id = -1;

    buf->next->prev = buf->prev;
    buf->prev->next = buf->next;

    E.number_of_buffer--;

    if (buf->type == TERMINAL_BUFF)
    {
        ptySessionClose(buf);
    }

    editorSetStatusMessage("Closed buffer %d", buffer_id);
    return 0;
}

/**
 * @brief Gets the current active buffer
 * @return Pointer to current buffer_t, NULL if none
 */
struct buffer_t* bufferGetCurrent(void)
{
    EditorPane* active = splitScreenGetActivePane();
    return bufferFindById(active->buffer_id);
}

/**
 * @brief Finds the registered lsp-server config matching a filename's
 * extension (see the Lisp `lsp-server` builtin), or NULL if none was
 * registered for it.
 * @note Guards against short/empty filenames rather than assuming a
 * minimum length, since utf8StringLen(...) - 3 would underflow otherwise.
 */
struct lspServerConfig_t* lspServerConfigFor(Utf8Char* filename)
{
    int len = utf8StringLen(filename); // includes terminator
    if (len < 3) return NULL; // need at least "x.e" -> 3 real chars + terminator

    int dot = -1;
    for (int i = 0; i < len - 1; i++)
        if (filename[i].length == 1 && filename[i].character[0] == '.')
            dot = i;
    if (dot < 0) return NULL;

    char ext[16] = {0};
    int j = 0;
    for (int i = dot + 1; i < len - 1 && j < (int)sizeof(ext) - 1; i++)
    {
        if (filename[i].length != 1) return NULL; // non-ASCII extension: no registered server can match
        ext[j++] = (char) filename[i].character[0];
    }

    for (int i = 0; i < E.number_of_lsp_servers; i++)
        if (strcmp(E.lsp_servers[i].extension, ext) == 0)
            return &E.lsp_servers[i];
    return NULL;
}

/**
 * @brief On the first file opened in a session whose extension has a
 * registered lsp-server, ask the user whether to start it and where the
 * project root is. Only asks once — a decline (or ESC) sets state to
 * LSP_SHUTDOWN so it won't ask again.
 * @note Called before editorOpen() loads the buffer's content — it only
 * needs the filename, not the text. See bufferMaybeSyncLsp() for buffers
 * created *after* the server's already running, which does need the text
 * and so must run after editorOpen().
 */
void bufferMaybeStartLsp(struct buffer_t* buf)
{
    if (!E.constantes.LSP) return;
    if (E.lsp_client->state != LSP_NOT_STARTED) return; // already asked, already running, or shut down

    struct lspServerConfig_t* cfg = lspServerConfigFor(buf->filename);
    if (!cfg) return; // no server registered for this extension

    Utf8Char* answer = editorPrompt("Start LSP server for this project? (y/n): %u", NULL, 0);
    if (!answer || answer->length != 1 ||
        (answer->character[0] != 'y' && answer->character[0] != 'Y'))
    {
        free(answer);
        E.lsp_client->state = LSP_SHUTDOWN; // declined — don't ask again this session
        return;
    }
    free(answer);

    Utf8Char* default_root = buf->path ? buf->path : buf->fullname;
    Utf8Char* root = editorPrompt("Project root: %u", default_root, 1); // bPathMode=1: Tab completes paths
    if (!root)
    {
        E.lsp_client->state = LSP_SHUTDOWN;
        return;
    }

    char* root_str = (char*) utf8ToString(root);
    free(root);

    strncpy(E.lsp_client->language_id, cfg->language_id, sizeof(E.lsp_client->language_id) - 1);

    if (lspStart(E.lsp_client, root_str, cfg->argv) != 0)
        editorSetStatusMessage("Failed to start LSP server");

    free(root_str);
}

/**
 * @brief Syncs a newly-opened buffer with an LSP server that's already
 * running (e.g. a second file opened after the first one started it).
 * @note Must run after editorOpen() — it sends the buffer's actual text,
 * so the rows need to already be loaded. The one-time startup flow in
 * bufferMaybeStartLsp() runs *before* editorOpen() instead, since the
 * retroactive didOpen loop in lsp_on_initialize_response() (which fires
 * once the handshake completes, well after this buffer's own
 * bufferCreate() call has fully returned) covers the very first buffer.
 */
void bufferMaybeSyncLsp(struct buffer_t* buf)
{
    if (!E.constantes.LSP || !E.lsp_client) return;
    if (E.lsp_client->state != LSP_READY) return; // not ready yet — the retroactive didOpen loop will cover it
    if (buf->b_lsp_open) return; // already synced

    if (!lspServerConfigFor(buf->filename)) return; // no server registered for this extension

    // Same file already open (and synced) in another buffer — the server
    // only expects one didOpen per URI, so just piggyback on that
    // buffer's already-open document instead of sending a second one.
    if (bufferFindOtherLspOpen(buf) != -1)
    {
        buf->b_lsp_open = 1;
        return;
    }

    lspDidOpen(E.lsp_client, buf);
    buf->b_lsp_open = 1;
}


/**
 * @brief Lists all open buffers
 * @details Prints buffer information to status message
 * @return Number of open buffers
 */
int bufferListAll(void)
{
    if (E.number_of_buffer == 0)
    {
        editorSetStatusMessage("No buffers open");
        return 0;
    }

    char buf[256] = "";
    int offset = 0;
    EditorPane* active = splitScreenGetActivePane();
    int first_buffer_id = E.buffers->buffer_id;
    struct buffer_t* b = E.buffers;
    do
    {
        char marker = (b->buffer_id == active->buffer_id) ? '*' : ' ';
        #if 0 // TODO: Remake
        offset += utf8Printf(&buf[offset], sizeof(buf) - offset,
                           "%c%d:%s ", marker, b->buffer_id,
                           b->filename ? b->filename : "[No Name]");
        b = b->next;
        #endif

    } while (b->buffer_id != first_buffer_id);
    editorSetStatusMessage("Buffers: %s", buf);
    return E.number_of_buffer;
}

/**
 * @brief Saves current buffer to disk
 * @return 0 on success, -1 on failure
 */
int bufferSave(void)
{
    EditorPane* active = splitScreenGetActivePane();
    if (active->buffer_id == -1)
    {
        editorSetStatusMessage("Error: no buffer active");
        return -1;
    }

    editorSave();
    return 0;
}

/**
 * @brief Saves all buffers to disk
 * @details Iterates through all buffers, saves each one
 * @return 0 on success, -1 on failure
 */
int bufferSaveAll(void)
{
    int saved = 0;
    int failed = 0;

    int first_buffer_id = E.buffers[0].buffer_id;
    struct buffer_t* b = E.buffers;

    do
    {
        if (bufferSwitch(b->buffer_id) == 0)
        {
            if (E.dirty && bufferSave() == 0)
            {
                saved++;
            }
        }
        else
        {
            failed++;
        }
        b = b->next;
    } while (b->buffer_id != first_buffer_id);

    editorSetStatusMessage("Saved %d buffers (%d failed)", saved, failed);
    return (failed == 0) ? 0 : -1;
}

/**
 * @brief Maps a byte offset into a row's utf8ToString() rendering back to
 * a character index into row->characters[] — needed because strstr()
 * matches are byte offsets, but buf->x is a character index, and the two
 * only coincide for pure-ASCII content.
 */
static int bufferByteOffsetToCharIndex(Row* row, int byte_offset)
{
    int bytes = 0;
    for (int i = 0; i < row->size; i++)
    {
        if (bytes == byte_offset) return i;
        bytes += row->characters[i].length;
    }
    return row->size;
}

/**
 * @brief Finds the last occurrence of needle in haystack starting before
 * byte offset max_byte (exclusive) — strstr() only ever finds the first,
 * which is the wrong direction for a reverse search.
 */
static char* bufferFindLastMatchBefore(const char* haystack, const char* needle, int max_byte)
{
    char* result = NULL;
    const char* search_from = haystack;
    while (1)
    {
        char* match = strstr(search_from, needle);
        if (!match || (int)(match - haystack) >= max_byte) break;
        result = match;
        search_from = match + 1;
    }
    return result;
}

/**
 * @brief Searches for a string in the document
 * @details Prompts user for a search query, then searches forward from current
 * cursor position (starting just after the cursor on the current line, so
 * repeated searches step to the next match instead of re-matching in
 * place). Updates cursor position to the first match found. Does not wrap
 * around past the end of the buffer.
 * @note Updates global editor state E (cursor position, row_offset)
 * @note Search is case-sensitive
 * @see editorPrompt()
 */
void bufferFind(struct buffer_t* buf)
{
    appDebug("searching\n");
    Utf8Char* query = editorPrompt("Search: %u (ESC to cancel)", NULL, 0);
    if (!query)
        return;

    char* query_str = (char*) utf8ToString(query);
    free(query);
    if (query_str[0] == '\0') { free(query_str); return; }

    for (int i = buf->y; i < buf->numrows; i++)
    {
        Row* row = &buf->row[i];
        char* row_str = (char*) utf8ToString(row->characters);

        int start_char = (i == buf->y) ? buf->x + 1 : 0;
        int start_byte = 0;
        for (int c = 0; c < start_char && c < row->size; c++)
            start_byte += row->characters[c].length;
        int row_bytes = (int) strlen(row_str);
        if (start_byte > row_bytes) start_byte = row_bytes;

        char* match = strstr(row_str + start_byte, query_str);
        if (match)
        {
            buf->y = i;
            buf->x = bufferByteOffsetToCharIndex(row, (int)(match - row_str));
            free(row_str);
            break;
        }
        free(row_str);
    }
    free(query_str);
}

/**
 * @brief Searches backward for a string in the document
 * @details Mirrors bufferFind() but scans from the current cursor position
 * back toward the start of the buffer, stopping at the last match before
 * the cursor on the current line. Does not wrap around past the start.
 * @see bufferFind()
 */
void bufferFindReverse(struct buffer_t* buf)
{
    appDebug("searching\n");
    Utf8Char* query = editorPrompt("Reverse search: %u (ESC to cancel)", NULL, 0);
    if (!query)
        return;

    char* query_str = (char*) utf8ToString(query);
    free(query);
    if (query_str[0] == '\0') { free(query_str); return; }

    for (int i = buf->y; i >= 0; i--)
    {
        Row* row = &buf->row[i];
        char* row_str = (char*) utf8ToString(row->characters);

        int end_char = (i == buf->y) ? buf->x : row->size;
        int end_byte = 0;
        for (int c = 0; c < end_char && c < row->size; c++)
            end_byte += row->characters[c].length;

        char* match = bufferFindLastMatchBefore(row_str, query_str, end_byte);
        if (match)
        {
            buf->y = i;
            buf->x = bufferByteOffsetToCharIndex(row, (int)(match - row_str));
            free(row_str);
            break;
        }
        free(row_str);
    }
    free(query_str);
}

void bufferInsertRow(struct buffer_t* buffer, int at, Utf8Char* string, int len)
{
    if (at < 0 || at > buffer->numrows)
        return;
    Row* tmp = recalloc(buffer->row, sizeof(Row) * buffer->numrows, sizeof(Row) * (buffer->numrows + 1));
    if (!tmp)
        return;
    buffer->row = tmp;

    /* Shift existing rows to make room at 'at' — no at++ */
    if (at < buffer->numrows)
    {
        memmove(&buffer->row[at + 1], &buffer->row[at],
                sizeof(Row) * (buffer->numrows - at)); /* not -at+1 */
    }

    buffer->row[at].size = (int)len;
    buffer->row[at].characters = calloc((len + 1), sizeof(Utf8Char));
    buffer->row[at].attrs = NULL;

    if (!buffer->row[at].characters)
        return;
    memcpy(buffer->row[at].characters, string, len * sizeof(Utf8Char));
    /* always NUL-terminate */
    END_OF_STRING(buffer->row[at].characters[len]);

    buffer->numrows++;
    if (buffer->type != TERMINAL_BUFF)
    {
        buffer->dirty++;
    }
}

void bufferFreeRow(Row* row) {
    free(row->characters);
    free(row->attrs);
}

/**
 * \fn editorRowInsertChar(erow *row, int at, int c)
 * \param at Index of where we want to insert the char */

void bufferRowInsertChar(struct buffer_t* buffer, Row* row, int at,
                          Utf8Char* character)
{
    if (buffer->state == READ_ONLY)
            return;
        if (at < 0 || at > row->size)
            return;

        Utf8Char *new_chars = (Utf8Char*) recalloc(
            row->characters,
            row->size * sizeof(Utf8Char),
            (row->size + 2) * sizeof(Utf8Char)); // +1 new char, +1 terminator
        if (!new_chars) {
            return;
        }
        row->characters = new_chars;

        memmove(row->characters + at + 1,
                row->characters + at,
                (row->size - at) * sizeof(Utf8Char));

        memcpy(row->characters + at, character, sizeof(Utf8Char));
        row->size += 1;

        END_OF_STRING(row->characters[row->size]);
        if (buffer->type != TERMINAL_BUFF)
        {
            ++buffer->dirty;
        }

}

/**
 * \fn bufferRowDelChar(struct bufferConfig *E, frow *frow, int at)
 * \brief Delete the a char at the chosen position on the given row
 * \param at Index of the char to delete
 * \param row Row on operation is made */
void bufferRowDelChar(struct buffer_t* buffer, Row* row, int at)
{
    if (buffer->state == READ_ONLY)
        return;
    if (at < 0 || at >= row->size)
        return;
    memmove(row->characters + at, row->characters + (at + 1), (row->size - at - 1) * sizeof(Utf8Char));
    row->size -= 1;
    END_OF_STRING(row->characters[row->size]);
    buffer->x -= 1;
}


void bufferInsertChar(Utf8Char * character)
{
    EditorPane* active = splitScreenGetActivePane();
    struct buffer_t* buf = bufferFindById(active->buffer_id);
    if (buf->y == buf->numrows)
    {
        Utf8Char end_string;
        END_OF_STRING(end_string);
        bufferInsertRow(buf, buf->numrows, &end_string, 0);
    }
    bufferRowInsertChar(buf, &buf->row[buf->y], buf->x, character);
    buf->x += 1;
    buf->b_has_changed = 1;
}

void bufferDelChar(void)
{
    EditorPane* active = splitScreenGetActivePane();
    struct buffer_t* buf = bufferFindById(active->buffer_id);
    int prev_char_count;

    /* Nothing to delete */
    if (buf->numrows == 0) return;
    if (buf->x == 0 && buf->y == 0) return;

    /* Use row_offset, not col_offset, for row indexing */
    Row* r = &buf->row[buf->y];

    if (buf->x > 0)
    {

        bufferRowDelChar(buf, r, buf->x - 1);
    }
    else
    {
        /* Merge current row into the previous one */
        Row* prev = &buf->row[buf->y - 1]; // FIX: was buf->y (same as r)
        prev_char_count = prev->size;
        for (int i = 0; i<r->size; ++i) {
            bufferRowInsertChar(buf, prev, prev->size, &r->characters[i]);
        }

        free(r->characters);
        r->characters = NULL;

        memmove(&buf->row[buf->y],
                &buf->row[buf->y + 1],
                sizeof(Row) * (buf->numrows - buf->y - 1));
        buf->numrows--;
        active->cursor_x = prev_char_count;
        buf->x = prev_char_count;
        active->cursor_y--;
        buf->y--;
    }
    if (buf->type != TERMINAL_BUFF)
    {
        buf->dirty++;
    }
}

void bufferInsertNewLine(void)
{
    EditorPane* active = splitScreenGetActivePane();
    struct buffer_t* buf = bufferFindById(active->buffer_id);

    // Unlike character insert/delete (bufferRowInsertChar/bufferRowDelChar),
    // this one never checked READ_ONLY at all — harmless for a normal
    // read-only file buffer (ENTER isn't a destructive edit to undo), but
    // an EXPLORER_BUFF's rows are meant to be set only by Lisp
    // (editor-scratch-set-lines), never mutated from the keyboard.
    if (buf->state == READ_ONLY)
        return;

    if (buf->y >= buf->numrows)
    {
        /* Cursor is past the last row: just append a blank line */
        Utf8Char end_string;
        END_OF_STRING(end_string);
        bufferInsertRow(buf, buf->numrows, &end_string, 0);
    }
    else
    {
        Row* row = &buf->row[buf->y];
        /* Insert the tail (from cursor to end) as a new row below */
        bufferInsertRow(buf, buf->y + 1, &row->characters[buf->x], row->size - buf->x);

        /* Re-fetch: bRealloc inside bufferInsertRow may have moved the array */
        row = &buf->row[buf->y];

        /* Truncate the current row at the cursor */
        row->size = buf->x;
        END_OF_STRING(row->characters[row->size])
        /* Do NOT touch row->cap — the allocation is still valid */
    }
    buf->y++;
    buf->x = 0;
}

char* bufferToText(struct buffer_t* buf)
{
    size_t total = 0;
    for (int i = 0; i < buf->numrows; i++)
    {
        Row* row = &buf->row[i];
        for (int j = 0; j < row->size; j++)
            total += row->characters[j].length;
        total += 1; // '\n'
    }

    char* text = malloc(total + 1);
    if (!text) return NULL;

    char* p = text;
    for (int i = 0; i < buf->numrows; i++)
    {
        Row* row = &buf->row[i];
        for (int j = 0; j < row->size; j++)
        {
            memcpy(p, row->characters[j].character, row->characters[j].length);
            p += row->characters[j].length;
        }
        *p++ = '\n';
    }
    *p = '\0';
    return text;
}
