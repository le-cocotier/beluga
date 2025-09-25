project('beluga', 'c',
  version : '1.0.0',
  default_options : [
    'c_std=none',
  ]
)

cc = meson.get_compiler('c')
m = cc.find_library('m', required: false)            

# Include directory
inc_dir = include_directories('include', 'lisp-interpreter/dist')

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
)

# Executable
executable('beluga',
  src_files,
  include_directories : inc_dir,
  dependencies: [m]
)
