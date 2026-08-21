# Builds a gtest binary against the runtime.
#
#   mono_gtest_binary(<name>
#     SOURCES     <file>...   the test bodies
#     LIBRARIES   <target>... the runtime to link, either monosgen-static or the
#                             pieces of it a suite can get away with
#     INCLUDES    <dir>...    on top of the source root, the build root and the
#                             collector's headers, which every suite gets
#     DEFINITIONS <def>...    on the compile line as given, so a path a test
#                             needs is spelled FOO="..." here
#
# The suite still has to register its cases. This only produces the binary.
function(mono_gtest_binary name)
  cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES;DEFINITIONS;LIBRARIES" ${ARGN})

  add_executable(${name} ${ARG_SOURCES})

  target_include_directories(${name} PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src"
    ${ARG_INCLUDES})

  target_compile_definitions(${name} PRIVATE ${ARG_DEFINITIONS})

  target_link_libraries(${name} PRIVATE
    ${ARG_LIBRARIES} mono::common mono::eglib_headers mono::llvm m
    GTest::gtest GTest::gtest_main)
  if(ZLIB_FOUND)
    target_link_libraries(${name} PRIVATE ZLIB::ZLIB)
  endif()

  # The runtime contains C++, which the C driver cannot link.
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
endfunction()

add_subdirectory(runtime)
add_subdirectory(llvm)
add_subdirectory(interp)
