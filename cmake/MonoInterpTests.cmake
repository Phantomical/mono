# The interpreter whitebox test: a C driver that reaches into the interpreter's
# internals, so it links libmini rather than running under the mono binary.

if(NOT MONO_CORPUS_ENABLED)
  return()
endif()

set(MONO_CORPUS_OUTPUTS "")
mono_corpus_il(whitebox-snippets.exe whitebox-snippets.il)

add_executable(test-mono-interp-whitebox whitebox.c)
target_include_directories(test-mono-interp-whitebox PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_SOURCE_DIR}")
target_link_libraries(test-mono-interp-whitebox PRIVATE
  mono::common mono::hidden mono::eglib_headers)
foreach(_o IN LISTS MONO_SGEN_OBJECTS)
  target_link_libraries(test-mono-interp-whitebox PRIVATE ${_o})
endforeach()
if(ZLIB_FOUND)
  target_link_libraries(test-mono-interp-whitebox PRIVATE ZLIB::ZLIB)
endif()
target_link_libraries(test-mono-interp-whitebox PRIVATE mono::llvm m)
set_target_properties(test-mono-interp-whitebox PROPERTIES LINKER_LANGUAGE CXX)

# The driver is a program rather than a corpus, so name it alongside the
# snippets assembly it reads.
mono_corpus_target(interp-whitebox-snippets DEPENDS test-mono-interp-whitebox)

add_test(NAME interp-whitebox
         COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${MONO_CORPUS_CLASS_DIR}"
                 $<TARGET_FILE:test-mono-interp-whitebox>
                 "${CMAKE_CURRENT_BINARY_DIR}/whitebox-snippets.exe"
         WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# Disabled, not deleted.  automake had `interp-whitebox` as a target you ran
# by hand -- it was never part of `make check` -- and the driver currently
# segfaults on startup with both build systems, so running it by default
# would just paint the suite red for a pre-existing runtime bug.  Re-enable
# by clearing DISABLED once that is fixed.
set_tests_properties(interp-whitebox PROPERTIES
  DISABLED TRUE
  LABELS manual)
