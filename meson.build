project('beluga', 'c',
  version : '3.1',
  default_options : [
    'c_std=c99'
  ]
)

cc = meson.get_compiler('c')
m = cc.find_library('m', required: false)
thread_dep = dependency('threads')

# glibc hides POSIX/GNU declarations when __STRICT_ANSI__ is set, which
# -std=c99 does. The editor leans on memmem, asprintf, strdup, getline,
# popen, realpath, kill, usleep, setenv and forkpty — all of which would
# fall back to implicit declarations on Linux, and implicit declarations
# are hard errors in modern gcc/clang, so the build simply fails there.
# Darwin exposes these regardless, so defining this unconditionally costs
# nothing on macOS.
add_project_arguments('-D_GNU_SOURCE', language : 'c')
add_project_arguments('-g', language : 'c')

# forkpty() lives in libutil on Linux; on macOS/BSD it is in libc and this
# lookup simply finds nothing, which is why it is not `required`.
util_dep = cc.find_library('util', required : false)

# Source files
src_files = files(
  'main.c',
  'src/append_buffer.c',
  'src/editor_op.c',
  'src/syntax_highlighter.c',
  'src/file_io.c',
  'src/init.c',
  'src/input.c',
  'src/output.c',
  'src/row_op.c',
  'src/terminal.c',
  'src/builtins.c',
  'src/buffer.c',
  'src/split_screen.c',
  'src/utf8.c',
  'src/completion.c',
  'src/lsp_ui.c',
  'src/buffer_switcher.c',
  'src/list_popup.c',
  'src/cJSON.c',
  'src/utils.c',
  'src/pty_session.c',
  'src/term_render.c',
  'src/ansi_parser.c',
  'src/which_key.c',
  'src/popup_ui.c'
)

# Executable
executable('beluga',
  src_files,
  dependencies: [m, thread_dep, util_dep]
)
