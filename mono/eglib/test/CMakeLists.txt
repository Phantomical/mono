# eglib's own test suite.  Driven by test.c's little harness rather than by
# CTest directly: one binary runs every group and reports per-group results.

add_executable(test-eglib
  test.c
  driver.c
  hashtable.c
  string-util.c
  string.c
  slist.c
  sizes.c
  ptrarray.c
  list.c
  array.c
  fake.c
  path.c
  queue.c
  shell.c
  spawn.c
  timer.c
  file.c
  pattern.c
  dir.c
  markup.c
  unicode.c
  utf8.c
  endian.c
  module.c
  memory.c
  # The one C++ source: it checks that eglib's headers stay includable from
  # C++, which the runtime's own C++ translation units depend on.
  enum.cpp)

target_link_libraries(test-eglib PRIVATE mono::common eglib)
target_compile_definitions(test-eglib PRIVATE
  EGLIB_TESTS=1 DRIVER_NAME="EGlib" _FORTIFY_SOURCE=2)
target_compile_options(test-eglib PRIVATE -Wall)
set_target_properties(test-eglib PROPERTIES LINKER_LANGUAGE CXX)

# g_find_program_in_path2 looks for the test binary itself in ".", so the test
# has to run from the directory it was linked into.
add_test(NAME eglib COMMAND test-eglib
         WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# The suite opens its UTF-* fixture files relative to $srcdir.
set_tests_properties(eglib PROPERTIES
  ENVIRONMENT "srcdir=${CMAKE_CURRENT_SOURCE_DIR}")
