/**
 * @file daemon_frame.h
 * @brief The messages between the beluga daemon and its clients.
 * @details Each frame is one type byte, the payload length as 4 bytes
 * (big-endian), then the payload:
 *
 *   'H' hello   client -> daemon  "beluga-daemon 1\n<rows> <cols>\n<cwd>\n"
 *                                 then one line per file to open
 *   'I' input   client -> daemon  key bytes, as the terminal sent them
 *   'R' resize  client -> daemon  "<rows> <cols>"
 *   'O' output  daemon -> client  bytes for the terminal (VT sequences)
 *   'K' kill    client -> daemon  "force" or "": stop the daemon (refused
 *                                 with unsaved buffers unless forced)
 *   'X' exit    daemon -> client  a message to print once the terminal is
 *                                 restored (may be empty); the daemon then
 *                                 closes the connection
 */
#ifndef DAEMON_FRAME_H_
#define DAEMON_FRAME_H_

#include <stddef.h>

#include "os.h"

#define FRAME_HELLO 'H'
#define FRAME_INPUT 'I'
#define FRAME_RESIZE 'R'
#define FRAME_OUTPUT 'O'
#define FRAME_KILL 'K'
#define FRAME_EXIT 'X'

#define DAEMON_PROTOCOL "beluga-daemon 1"

// A frame larger than this is a broken peer, not a big screen.
#define FRAME_MAX_PAYLOAD (16 * 1024 * 1024)

// Sends one frame. 0, or -1 if the connection is gone.
int frameSend(OsConn *conn, char type, const void *payload, size_t len);

// Collects bytes as they arrive and cuts them into frames.
typedef struct
{
    unsigned char *buf;
    size_t len;
    size_t cap;
    size_t used; // bytes at the front already handed out by frameNext()
} FrameReader;

/**
 * @brief Reads what `conn` has (waiting at most `timeout_ms`) into `r`.
 * @return >0 bytes read, 0 nothing in time, -1 the connection has ended.
 */
long frameRead(FrameReader *r, OsConn *conn, int timeout_ms);

/**
 * @brief The next complete frame, if one has arrived.
 * @return 1 with *type, *payload and *len set (payload valid until the
 * next frameRead()), 0 if none yet, -1 if the stream is malformed.
 */
int frameNext(FrameReader *r, char *type, const unsigned char **payload, size_t *len);

void frameReaderFree(FrameReader *r);

#endif // DAEMON_FRAME_H_
