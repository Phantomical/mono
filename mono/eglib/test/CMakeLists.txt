# eglib's own test suite, on gtest.

add_executable(test-eglib
  array.cpp
  dir.cpp
  endian.cpp
  enum.cpp
  file.cpp
  hashtable.cpp
  list.cpp
  markup.cpp
  memory.cpp
  module.cpp
  path.cpp
  pattern.cpp
  ptrarray.cpp
  queue.cpp
  shell.cpp
  sizes.cpp
  slist.cpp
  spawn.cpp
  string-util.cpp
  string.cpp
  timer.cpp
  unicode.cpp
  utf8.cpp)

target_link_libraries(test-eglib PRIVATE
  mono::common eglib GTest::gtest GTest::gtest_main)
# The UTF-* fixtures and the machine.config the markup parser is pointed at are
# read at runtime; naming both here keeps them off the working directory.
target_compile_definitions(test-eglib PRIVATE
  EGLIB_TESTS=1 _FORTIFY_SOURCE=2
  EGLIB_TEST_DATA_SRCDIR="${CMAKE_CURRENT_SOURCE_DIR}"
  EGLIB_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
target_compile_options(test-eglib PRIVATE -Wall)

# path/find_program_in_path_cwd blanks $PATH and then looks for the test binary
# itself in ".", so the tests have to run from the directory they were linked
# into.
gtest_discover_tests(test-eglib
  TEST_PREFIX eglib/
  DISCOVERY_MODE PRE_TEST
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  PROPERTIES LABELS eglib)
