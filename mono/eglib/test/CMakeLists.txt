# eglib's own test suite.
#
# Being ported from the hand-rolled driver below to gtest, one group at a time.
# Both binaries live in the same directory and run from it, so the two halves
# stay interchangeable while the move is in progress.

add_executable(test-eglib
  array.cpp
  endian.cpp
  enum.cpp
  hashtable.cpp
  list.cpp
  memory.cpp
  ptrarray.cpp
  queue.cpp
  sizes.cpp
  slist.cpp)

target_link_libraries(test-eglib PRIVATE
  mono::common eglib GTest::gtest GTest::gtest_main)
target_compile_definitions(test-eglib PRIVATE EGLIB_TESTS=1 _FORTIFY_SOURCE=2)
target_compile_options(test-eglib PRIVATE -Wall)

# g_find_program_in_path2 looks for the test binary itself in ".", so the tests
# have to run from the directory they were linked into.
gtest_discover_tests(test-eglib
  TEST_PREFIX eglib/
  DISCOVERY_MODE PRE_TEST
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  PROPERTIES LABELS eglib)

# ---------------------------------------------------------------------------
# The groups still on the 2006 driver.  One binary runs every remaining group
# and reports per-group results.
add_executable(test-eglib-driver
  test.c
  driver.c
  string-util.c
  string.c
  fake.c
  path.c
  shell.c
  spawn.c
  timer.c
  file.c
  pattern.c
  dir.c
  markup.c
  unicode.c
  utf8.c
  module.c)

target_link_libraries(test-eglib-driver PRIVATE mono::common eglib)
target_compile_definitions(test-eglib-driver PRIVATE
  EGLIB_TESTS=1 DRIVER_NAME="EGlib" _FORTIFY_SOURCE=2)
target_compile_options(test-eglib-driver PRIVATE -Wall)
set_target_properties(test-eglib-driver PROPERTIES LINKER_LANGUAGE CXX)

add_test(NAME eglib-driver COMMAND test-eglib-driver
         WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# The suite opens its UTF-* fixture files relative to $srcdir.
set_tests_properties(eglib-driver PROPERTIES
  LABELS eglib
  ENVIRONMENT "srcdir=${CMAKE_CURRENT_SOURCE_DIR}")
