# sgen-grep-binprot: filters SGen's binary protocol dumps.
#
# The reader is compiled three times because a dump can come from a runtime
# with a different pointer width than the tool: once for the native layout, and
# once each for the explicit 32- and 64-bit-pointer forms that carry a header.

if(NOT MONO_ENABLE_EXECUTABLES)
  return()
endif()

add_executable(sgen-grep-binprot
  sgen-grep-binprot-main.c
  sgen-entry-stream.c
  sgen-grep-binprot.c)

target_include_directories(sgen-grep-binprot PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(sgen-grep-binprot PRIVATE
  mono::common mono::eglib_headers eglib)

foreach(_ptr 4 8)
  math(EXPR _bits "${_ptr} * 8")
  add_library(sgen-grep-binprot${_bits}p OBJECT sgen-grep-binprot.c)
  target_include_directories(sgen-grep-binprot${_bits}p PRIVATE "${CMAKE_SOURCE_DIR}")
  target_link_libraries(sgen-grep-binprot${_bits}p PRIVATE
    mono::common mono::eglib_headers)
  target_compile_definitions(sgen-grep-binprot${_bits}p PRIVATE
    BINPROT_SIZEOF_VOID_P=${_ptr} BINPROT_HAS_HEADER)
  target_link_libraries(sgen-grep-binprot PRIVATE sgen-grep-binprot${_bits}p)
endforeach()

install(TARGETS sgen-grep-binprot RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
