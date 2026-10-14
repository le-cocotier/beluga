# Fuzz targets

Two parsers read bytes nobody controls: `ansi_parser.c` (whatever a
program in a terminal pane prints) and the LSP client in `completion.c`
(whatever a language server sends). Each has a libFuzzer target here:

| Target                | Feeds                                                   |
|-----------------------|---------------------------------------------------------|
| `fuzz_ansi_parser.c`  | `ansiParserFeed()` on a terminal buffer, in two reads   |
| `fuzz_lsp.c`          | `lspFeed()`: framing, cJSON, notifications, server requests, edits to one open buffer |

`corpus/<target>/` holds the seeds. A normal build runs every target over
its corpus as a `meson test`, so a crash input saved there becomes a
regression test.

## Fuzzing

Needs clang with its runtime libraries (Debian/Ubuntu: `clang` and
`libclang-rt-dev`):

```sh
CC=clang meson setup build-fuzz -Dfuzzers=true -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build-fuzz
mkdir -p /tmp/lsp-corpus && cp test/fuzz/corpus/lsp/* /tmp/lsp-corpus/
build-fuzz/fuzz_lsp -max_total_time=600 /tmp/lsp-corpus
```

The first byte of each input picks where it is split into two reads, so
sequences cut across reads get covered too. When the fuzzer reports a
crash, copy the `crash-*` file it wrote into the target's corpus and fix
the bug; the test keeps it fixed.
