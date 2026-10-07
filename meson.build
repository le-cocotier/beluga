project('beluga', 'c',
  version : '3.2',
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

# House rule: every variable is declared at the top of its function. The
# compiler can only enforce part of it (a declaration after a statement in
# the same block) — declarations opening a nested block or a for-loop
# header aren't flagged, so keep to the rule by hand there too.
add_project_arguments('-Wdeclaration-after-statement', language : 'c')

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
  'src/gutils.c',
  'src/memory_inspector.c',
  'src/pty_session.c',
  'src/term_render.c',
  'src/ansi_parser.c',
  'src/which_key.c',
  'src/popup_ui.c',
  'src/subprocess.c',
  'src/async_process.c',
  'src/project_search.c',
  'src/file_finder.c'
)

# Windows (native, built with MinGW-w64 gcc). The Unix sources compile
# unchanged against compat/win32/ — stand-ins for termios.h, sys/select.h,
# poll.h, ... — and include/platform_win32.h, force-included into every
# file; src/platform_win32.c implements them. See platform_win32.h.
inc_dirs = []
extra_sources = []
link_args = []
extra_deps = []
if host_machine.system() == 'windows'
  if cc.get_id() != 'gcc'
    error('Windows builds need MinGW-w64 gcc (MSYS2 UCRT64, WinLibs, or a cross-compiler); MSVC is not supported.')
  endif
  inc_dirs += include_directories('compat/win32')
  add_project_arguments(
    '-include', meson.current_source_dir() / 'include' / 'platform_win32.h',
    '-D_WIN32_WINNT=0x0A00',
    # C99 printf (%zu, %lld, ...) instead of msvcrt.dll's, which lacks them.
    '-D__USE_MINGW_ANSI_STDIO=1',
    language : 'c')
  extra_sources += files('src/platform_win32.c')
  # The manifest sets the process code page to UTF-8 (see windows/beluga.manifest).
  extra_sources += import('windows').compile_resources('windows/beluga.rc',
    depend_files : files('windows/beluga.manifest'))
  extra_deps += cc.find_library('user32') # clipboard
  extra_deps += cc.find_library('shell32') # file operations (dired)
  # One self-contained beluga.exe: no libgcc/libwinpthread DLLs to ship next to it.
  link_args += ['-static']
endif

# Executable
executable('beluga',
  src_files + extra_sources,
  include_directories : inc_dirs,
  dependencies : [m, thread_dep, util_dep] + extra_deps,
  link_args : link_args
)
