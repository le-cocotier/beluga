project('editor', 'c',
  version : '1.0.0',
  default_options : [
    'warning_level=2',
  ]
)

# Check if we're using Clang and add Clang-specific options
cc = meson.get_compiler('c')
m = cc.find_library('m', required: true)
if cc.get_id() == 'clang'
  add_project_arguments([
    '-Wextra',
    '-Wpedantic',
    '-Wno-unused-parameter',
    '-fcolor-diagnostics'  # Colored output
  ], language : 'c')
  
  # Add debug options for debug builds
  if get_option('buildtype') == 'debug'
    add_project_arguments([
      '-fsanitize=address',      # AddressSanitizer
      '-fsanitize=undefined',    # UndefinedBehaviorSanitizer
      '-g3',                     # Full debug info
      '-O0'                      # No optimization
    ], language : 'c')
    
    add_project_link_arguments([
      '-fsanitize=address',
      '-fsanitize=undefined'
    ], language : 'c')
  endif
endif

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
executable('editor',
  src_files,
  include_directories : inc_dir,
  install : true,
  dependencies: [m]
)
