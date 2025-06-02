# Specify the minimum version of CMake required
cmake_minimum_required(VERSION 3.10)

# Define the project name and the programming language (C)
project(Beluga)

# Set the C standard (optional)
set(CMAKE_C_STANDARD 99)

set(CMAKE_C_COMPILER clang)

# Add the header files directory to the include path
include_directories(include)
include_directories(blisp/include)

# Add the source files for the project
set(SRCS
    main.c
    src/append_buffer.c
    src/file_io.c
    src/input.c
    src/row_op.c
    src/editor_op.c
    src/init.c
    src/output.c
    src/terminal.c
    src/builtins.c
    blisp/src/config_tools.c
    blisp/src/data.c
    blisp/src/lexer.c
    blisp/src/parser.c)

find_package(Doxygen)
if(DOXYGEN_FOUND)
  # set input and output for doxygen
  set(DOXYGEN_OUT ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile)

  add_custom_target(
    doc_doxygen ALL
    COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating documentation with Doxygen"
    VERBATIM)
else(DOXYGEN_FOUND)
  message("Doxygen not found")
endif(DOXYGEN_FOUND)

# we default to Release build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

set(CMAKE_C_FLAGS "-Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "-g")

# Create an executable target with the specified source files
add_executable(beluga ${SRCS})

# Optionally, you can set the output directory for the executable
set_target_properties(beluga PROPERTIES RUNTIME_OUTPUT_DIRECTORY bin)
