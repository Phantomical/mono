# pedump: loads an assembly through the real metadata layer and reports what
# the verifier makes of it.  Needs SGen because it links the sgen runtime.

if(NOT MONO_ENABLE_EXECUTABLES OR NOT MONO_ENABLE_LIBRARIES OR NOT MONO_ENABLE_SGEN)
  return()
endif()

add_executable(pedump pedump.c)
target_include_directories(pedump PRIVATE
  "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/mono")
target_link_libraries(pedump PRIVATE
  mono::common mono::hidden mono::eglib_headers
  monoruntime_sgen monoruntime_config monoruntime_support monoculture
  monosgen monoutils eglib
  mono::llvm)
if(ZLIB_FOUND)
  target_link_libraries(pedump PRIVATE ZLIB::ZLIB)
endif()
set_target_properties(pedump PROPERTIES LINKER_LANGUAGE CXX)

install(TARGETS pedump RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
