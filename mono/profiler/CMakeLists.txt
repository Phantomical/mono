# The profiler modules.  These are dlopen'ed into a running runtime, so they
# deliberately do NOT link against libmono: pulling a second copy of the
# runtime's static data into the process runs library constructors twice, which
# breaks LLVM in particular.  The undefined mono_* symbols are resolved from the
# executable, which is linked --export-dynamic for exactly this reason.

set(_prof_modules aot log coverage)
set(_prof_aot_sources      aot.c helper.c)
set(_prof_log_sources      log.c log-args.c helper.c)
set(_prof_coverage_sources coverage.c)

# Three artifacts per module, as libtool produced: the shared object that gets
# dlopen'ed, its static twin, and a separately-named "-static" archive for
# targets that cannot dlopen at all and link the profiler into the runtime.
foreach(_m IN LISTS _prof_modules)
  foreach(_kind shared archive static)
    if(_kind STREQUAL "shared")
      set(_name "mono-profiler-${_m}")
      add_library(${_name} SHARED ${_prof_${_m}_sources})
      # libtool's default -version-info 0:0:0.
      set_target_properties(${_name} PROPERTIES VERSION 0.0.0 SOVERSION 0)
    elseif(_kind STREQUAL "archive")
      set(_name "mono-profiler-${_m}-archive")
      add_library(${_name} STATIC ${_prof_${_m}_sources})
      set_target_properties(${_name} PROPERTIES OUTPUT_NAME "mono-profiler-${_m}")
    else()
      set(_name "mono-profiler-${_m}-static")
      add_library(${_name} STATIC ${_prof_${_m}_sources})
    endif()
    target_link_libraries(${_name} PRIVATE
      mono::common mono::hidden mono::eglib_headers eglib)
    target_compile_definitions(${_name} PRIVATE
      SUPPRESSION_DIR="${CMAKE_INSTALL_FULL_DATADIR}/mono-${MONO_API_VERSION}/mono/profiler")
    target_include_directories(${_name} PRIVATE "${CMAKE_SOURCE_DIR}")
    if(_m STREQUAL "log" AND ZLIB_FOUND)
      target_link_libraries(${_name} PRIVATE ZLIB::ZLIB)
    endif()
    install(TARGETS ${_name}
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  endforeach()
endforeach()

if(MONO_ENABLE_EXECUTABLES)
  add_executable(mprof-report mprof-report.c)
  target_link_libraries(mprof-report PRIVATE
    mono::common mono::hidden mono::eglib_headers eglib)
  target_include_directories(mprof-report PRIVATE "${CMAKE_SOURCE_DIR}")
  if(ZLIB_FOUND)
    target_link_libraries(mprof-report PRIVATE ZLIB::ZLIB)
  endif()
  install(TARGETS mprof-report RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# The P/Invoke target the profiler's own managed tests call into.
add_library(proftest_pinvokes SHARED proftest-pinvokes.c)
target_link_libraries(proftest_pinvokes PRIVATE mono::common mono::eglib_headers)
target_include_directories(proftest_pinvokes PRIVATE "${CMAKE_SOURCE_DIR}")

install(FILES mono-profiler-coverage.suppression
        DESTINATION "${MONO_DATA_SUBDIR}/mono/profiler")
