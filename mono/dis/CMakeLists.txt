# monodis, the IL disassembler.

if(NOT MONO_ENABLE_EXECUTABLES OR NOT MONO_ENABLE_LIBRARIES)
  return()
endif()

if(MONO_ENABLE_SGEN)
  set(_dis_runtime monoruntime_sgen monosgen)
else()
  set(_dis_runtime monoruntime_boehm monogc)
endif()

add_executable(monodis
  get.c
  dis-cil.c
  util.c
  dump.c
  main.c
  declsec.c)

target_include_directories(monodis PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(monodis PRIVATE
  mono::common mono::eglib_headers
  ${_dis_runtime} monoruntime_config monoruntime_support monoculture
  monoutils eglib
  mono::llvm)
if(ZLIB_FOUND)
  target_link_libraries(monodis PRIVATE ZLIB::ZLIB)
endif()
set_target_properties(monodis PROPERTIES LINKER_LANGUAGE CXX)

install(TARGETS monodis RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES monodis.1 DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
