project('beluga', 'c',
  version : '1.1',
  default_options : [
    'c_std=none',
  ]
)

cc = meson.get_compiler('c')
m = cc.find_library('m', required: false)            

# Source files
src_files = files(
  'main.c',
  'src/append_buffer.c',
  'src/editor_op.c',
  'src/file_io.c',
  'src/init.c',
  'src/input.c',
  'src/output.c',
  'src/row_op.c',
  'src/terminal.c',
  'src/builtins.c',
)

# Executable
executable('beluga',
  src_files,
  dependencies: [m]
)
