project('beluga', 'c',
  version : '3.4',
  default_options : [
    'c_std=c99'
  ]
)

cc = meson.get_compiler('c')
m = cc.find_library('m', required: false)
is_windows = host_machine.system() == 'windows'

add_project_arguments('-g', language : 'c')

# House rule: every variable is declared at the top of its function. The
# compiler can only enforce part of it (a declaration after a statement in
# the same block) — declarations opening a nested block or a for-loop
# header aren't flagged, so keep to the rule by hand there too.
add_project_arguments('-Wdeclaration-after-statement', language : 'c')

# Fuzzing builds instrument all the code for libFuzzer's coverage feedback.
if get_option('fuzzers')
  add_project_arguments('-fsanitize=fuzzer-no-link', language : 'c')
endif

# ─── The OS layer ─────────────────────────────────────────────────────────────
# Everything beluga needs from the operating system is declared in
# include/os.h and implemented by exactly one of two libraries:
#   libbeluga-os-posix  src/os/os_posix.c  Linux, macOS, BSD
#   libbeluga-os-win32  src/os/os_win32.c  Windows 10 1809+ / 11
# They're the only code allowed OS headers or POSIX/Win32 calls
# (tools/check_os_boundary.sh checks it).
if is_windows
  # MinGW-w64 gcc or clang (llvm-mingw, MSYS2 CLANG64); not MSVC or clang-cl.
  if cc.get_argument_syntax() == 'msvc'
    error('Windows builds need MinGW-w64 gcc or clang (MSYS2 UCRT64/CLANG64, WinLibs, llvm-mingw, or a cross-compiler); MSVC is not supported.')
  endif
  os_lib = static_library('beluga-os-win32',
    'src/os/os_win32.c',
    c_args : ['-D_WIN32_WINNT=0x0A00'],
    dependencies : [cc.find_library('user32'),    # clipboard
                    cc.find_library('shell32'),   # file operations
                    cc.find_library('advapi32'),  # the daemon pipe's permissions
                    cc.find_library('ws2_32')])   # debug adapters over TCP
else
  os_lib = static_library('beluga-os-posix',
    'src/os/os_posix.c',
    # forkpty, realpath, ...: this library is the POSIX one.
    c_args : ['-D_GNU_SOURCE'],
    # forkpty() lives in libutil on Linux; on macOS/BSD it is in libc and
    # this lookup simply finds nothing, which is why it is not `required`.
    dependencies : [cc.find_library('util', required : false)])
endif

# ─── The editor ───────────────────────────────────────────────────────────────
# Plain C99: no _GNU_SOURCE or other feature-test macros, so the C library
# declares only standard C (glibc hides strdup, popen, fork, ... under
# -std=c99), and calling anything else is an error rather than a warning.
# What the editor needs beyond standard C comes from os.h, or from its own
# utils.c.
core_args = [
  '-Werror=implicit-function-declaration',
  # lisp.h maps files with mmap() unless told not to; stdio is portable.
  '-DLISP_NO_MMAP',
]
link_args = []
extra_sources = []
if is_windows
  # C99 printf (%zu, %lld, ...) instead of msvcrt.dll's, which lacks them.
  core_args += ['-D__USE_MINGW_ANSI_STDIO=1']
  # The manifest sets the process code page to UTF-8, so the C runtime's
  # fopen() takes UTF-8 paths, as on Unix (see windows/beluga.manifest).
  extra_sources += import('windows').compile_resources('windows/beluga.rc',
    depend_files : files('windows/beluga.manifest'))
  # One self-contained beluga.exe: no libgcc DLLs to ship next to it.
  # Not for a sanitizer build (clang's AddressSanitizer on Windows only
  # comes as a DLL, libclang_rt.asan_dynamic-x86_64.dll).
  if get_option('b_sanitize') == 'none'
    link_args += ['-static']
  endif
endif

src_files = files(
  'src/append_buffer.c',
  'src/editor_op.c',
  'src/syntax_highlighter.c',
  'src/file_io.c',
  'src/autosave.c',
  'src/init.c',
  'src/config_load.c',
  'src/input.c',
  'src/output.c',
  'src/terminal.c',
  'src/builtins.c',
  'src/builtins_edit.c',
  'src/builtins_debug.c',
  'src/buffer.c',
  'src/split_screen.c',
  'src/utf8.c',
  'src/completion.c',
  'src/jsonrpc_stream.c',
  'src/breakpoints.c',
  'src/dap.c',
  'src/debug_ui.c',
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
  'src/project.c',
  'src/project_files.c',
  'src/project_search.c',
  'src/file_finder.c',
  'src/packages.c',
  'src/replace.c',
  'src/macro.c',
  'src/screen_diff.c',
  'src/describe.c',
  'src/settings.c',
  'src/lisp_gc.c',
  'src/daemon.c',
  'src/daemon_client.c',
  'src/daemon_frame.c'
)

# Everything but main(), so the unit tests link the same code.
core_lib = static_library('beluga-core',
  src_files,
  c_args : core_args,
  link_with : os_lib,
  dependencies : [m]
)

beluga_exe = executable('beluga',
  files('main.c') + extra_sources,
  c_args : core_args,
  link_with : [core_lib, os_lib],
  dependencies : [m],
  link_args : link_args
)

# The OS boundary, checked: no file outside src/os/ includes an OS header.
test('os-boundary', find_program('tools/check_os_boundary.sh'),
  args : [meson.current_source_dir()])

# Every allocation goes through galloc, so the memory inspector sees it.
test('galloc', find_program('tools/check_galloc.sh'),
  args : [meson.current_source_dir()])

# Unit tests (test/): each is a small program linked against the editor's
# code; test/test.c stands in for main.c's globals. On Windows they get
# beluga.exe's manifest too, so file names are UTF-8 for them as well.
foreach t : ['utf8', 'buffer', 'ansi_parser', 'os', 'completion', 'packages', 'highlight', 'lsp_symbols', 'breakpoints', 'lisp_lib']
  test(t, executable('test_' + t,
    files('test/test_' + t + '.c', 'test/test.c') + extra_sources,
    c_args : core_args,
    link_with : [core_lib, os_lib],
    dependencies : [m],
    link_args : link_args))
endforeach

# Headless tests (POSIX), beluga in a pseudo-terminal. The smoke test opens
# a file, types, saves and quits; under a sanitizer build it fails on any
# report, leaks included.
python3 = find_program('python3', required : false)
if python3.found() and not is_windows
  test('smoke', python3,
    args : [files('tools/smoke_test.py'), beluga_exe, meson.current_source_dir()],
    timeout : 60)
  # What's on screen, through pyte (tools/pty_harness.py): TAB completion,
  # and full-screen programs (less, vim) in a terminal pane. Skipped
  # without pyte (python3-pyte).
  test('pty', python3,
    args : [files('tools/pty_tests.py'), beluga_exe, meson.current_source_dir()],
    timeout : 300)
endif

# Fuzz targets (test/fuzz/). By default each is built with a small main()
# that runs it over its seed corpus, as a regression test. With clang and
# -Dfuzzers=true they're libFuzzer programs instead; see test/fuzz/README.md.
foreach f : ['ansi_parser', 'lsp']
  if get_option('fuzzers')
    executable('fuzz_' + f,
      files('test/fuzz/fuzz_' + f + '.c', 'test/test.c'),
      c_args : core_args,
      link_with : [core_lib, os_lib],
      dependencies : [m],
      link_args : ['-fsanitize=fuzzer'])
  else
    test('fuzz-corpus-' + f, executable('fuzz_' + f,
      files('test/fuzz/fuzz_' + f + '.c', 'test/fuzz/run_corpus.c', 'test/test.c'),
      c_args : core_args,
      link_with : [core_lib, os_lib],
      dependencies : [m],
      link_args : link_args),
      args : [meson.current_source_dir() / 'test/fuzz/corpus' / f])
  endif
endforeach

# The package manager end to end (test/test_packages.sh): beluga
# --sync-packages against throwaway package repositories. A shell script
# with file:// git remotes, so not on Windows.
if not is_windows
  test('package-manager', find_program('test/test_packages.sh'),
    args : [beluga_exe.full_path(), meson.current_source_dir()],
    depends : beluga_exe,
    timeout : 300)
endif
