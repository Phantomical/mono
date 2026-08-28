# The profiler modules.  These are dlopen'ed into a running runtime, so they
# deliberately do not link against libmono. Pulling a second copy of the
# runtime's static data into the process runs library constructors twice,
# which breaks LLVM in particular.  Their undefined mono_* symbols resolve
# instead from the executable, which is linked --export-dynamic.
#
# A PE module cannot be left with undefined symbols: every import names the
# image it comes from.  So on Windows a module links the runtime DLL, which is
# also what the mono binaries link there -- MONO_STATIC_MONO is off for the
# same reason -- and the process still holds one copy of the runtime.

set(_prof_modules aot log coverage)
set(_prof_aot_sources      aot.c helper.c)
set(_prof_log_sources      log.c log-args.c helper.c)
set(_prof_coverage_sources coverage.c)

# Three artifacts per module, as libtool produced: the shared object that gets
# dlopen'ed, its static twin, and a separately-named "-static" archive for
# targets that cannot dlopen at all and link the profiler into the runtime.
#
# The static twin is what a PE host cannot have: a DLL's import library is a
# .lib too, so it already owns the name the twin would take.  The "-static"
# archive is the same objects under a name of its own, so nothing is lost.
set(_prof_kinds shared archive static)
if(WIN32)
  list(REMOVE_ITEM _prof_kinds archive)
endif()

foreach(_m IN LISTS _prof_modules)
  foreach(_kind IN LISTS _prof_kinds)
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
    # See the note at the top: on Windows an import has to name the image it
    # comes from, so a module cannot leave its mono_* undefined for the loader
    # to answer out of the executable.
    if(MONO_HOST_WINDOWS AND NOT _kind STREQUAL "static")
      target_link_libraries(${_name} PRIVATE mono${MONO_DEFAULT_GC_SUFFIX}-2.0)
    endif()
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
if(MONO_HOST_WINDOWS)
  target_link_libraries(proftest_pinvokes PRIVATE eglib)
endif()
target_include_directories(proftest_pinvokes PRIVATE "${CMAKE_SOURCE_DIR}")

install(FILES mono-profiler-coverage.suppression
        DESTINATION "${MONO_DATA_SUBDIR}/mono/profiler")
