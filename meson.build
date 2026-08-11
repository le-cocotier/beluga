project('beluga', 'c',
  version : '2.3',
  default_options : [
    'c_std=c99'
  ]
)

cc = meson.get_compiler('c')
m = cc.find_library('m', required: false)
thread_dep = dependency('threads')

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
  'src/ansi_parser.c'
)

# Executable
executable('beluga',
  src_files,
  dependencies: [m, thread_dep]
)
