# The interpreter's managed tests.
#
# Each `.cs` here compiles to an assembly of static int methods named
# test_<n>_<what>, the convention mono/mini's --regression harness reads, and
# each of those methods becomes its own ctest test run in its own process by
# runner.cpp.  A failure therefore names the method and the engine it ran on,
# and --rerun-failed re-runs only what broke.
#
# Which methods exist is decided when ctest starts rather than at configure time
# -- see discover.cmake -- so writing a new test means editing a `.cs` and
# building, with no cmake step in between.

if(NOT MONO_CORPUS_ENABLED)
  # No class-library toolchain, so nothing to compile the suites with.
  return()
endif()
if(NOT MONO_ENABLE_SGEN OR NOT MONO_ENABLE_LIBRARIES)
  # No monosgen-static for the runner to link.
  return()
endif()

# ---------------------------------------------------------------------------
# The runner
# ---------------------------------------------------------------------------
add_executable(interp-test-runner runner.cpp)

target_include_directories(interp-test-runner PRIVATE
  "${CMAKE_BINARY_DIR}"
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/mono/mini"
  "${CMAKE_BINARY_DIR}/mono/mini"
  "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
  "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src")

target_compile_definitions(interp-test-runner PRIVATE
  MONO_INTERP_TESTS_ASSEMBLIES="${MONO_CORPUS_CLASS_DIR}")

target_link_libraries(interp-test-runner PRIVATE
  monosgen-static mono::common mono::eglib_headers mono::llvm m)
if(ZLIB_FOUND)
  target_link_libraries(interp-test-runner PRIVATE ZLIB::ZLIB)
endif()
set_target_properties(interp-test-runner PROPERTIES LINKER_LANGUAGE CXX)

# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------
# mono_add_test_methods(
#     ASSEMBLY <path> NAME_PREFIX <str> [NAME_SUFFIX <str>] [TIMEOUT <seconds>]
#     [PROPERTIES <prop> <value>...] [ENVIRONMENT <VAR=value>...])
#
# The name parts and the environment go together: registering one assembly twice
# under two of each is what would run the same methods on a second engine.
function(mono_add_test_methods)
  cmake_parse_arguments(ARG ""
    "ASSEMBLY;NAME_PREFIX;NAME_SUFFIX;TIMEOUT" "PROPERTIES;ENVIRONMENT" ${ARGN})

  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 60)
  endif()

  # The generated files are named after the arm, which keeps a second
  # registration of the same assembly from overwriting the first.
  string(REGEX REPLACE "[^A-Za-z0-9_.-]" "_" _key "${ARG_NAME_PREFIX}${ARG_NAME_SUFFIX}")
  set(_include "${CMAKE_CURRENT_BINARY_DIR}/${_key}_include.cmake")
  set(_tests   "${CMAKE_CURRENT_BINARY_DIR}/${_key}_tests.cmake")

  set(_args "")
  foreach(_a PROPERTIES ENVIRONMENT)
    if(ARG_${_a})
      string(APPEND _args "      ${_a} [==[${ARG_${_a}}]==]\n")
    endif()
  endforeach()

  # Listed again when the runner or the assembly is newer than the last listing.
  file(GENERATE OUTPUT "${_include}" CONTENT
"if(EXISTS \"$<TARGET_FILE:interp-test-runner>\" AND EXISTS \"${ARG_ASSEMBLY}\")
  if(NOT EXISTS \"${_tests}\"
     OR NOT \"${_tests}\" IS_NEWER_THAN \"$<TARGET_FILE:interp-test-runner>\"
     OR NOT \"${_tests}\" IS_NEWER_THAN \"${ARG_ASSEMBLY}\"
     OR NOT \"${_tests}\" IS_NEWER_THAN \"\${CMAKE_CURRENT_LIST_FILE}\")
    include(\"${CMAKE_CURRENT_SOURCE_DIR}/discover.cmake\")
    mono_discover_test_methods_impl(
      CMAKE_EXE [==[${CMAKE_COMMAND}]==]
      RUNNER [==[$<TARGET_FILE:interp-test-runner>]==]
      ASSEMBLY [==[${ARG_ASSEMBLY}]==]
      CTEST_FILE [==[${_tests}]==]
      NAME_PREFIX [==[${ARG_NAME_PREFIX}]==]
      NAME_SUFFIX [==[${ARG_NAME_SUFFIX}]==]
      WORKING_DIR [==[${CMAKE_CURRENT_BINARY_DIR}]==]
      TIMEOUT [==[${ARG_TIMEOUT}]==]
${_args}    )
  endif()
  include(\"${_tests}\")
else()
  add_test([==[${ARG_NAME_PREFIX}!not-built${ARG_NAME_SUFFIX}]==]
           [==[${CMAKE_COMMAND}]==] [==[-E]==] [==[false]==])
endif()
")

  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_include}")
endfunction()

# ---------------------------------------------------------------------------
# The suites
# ---------------------------------------------------------------------------
# One assembly out of every `.cs` here.  The class a method sits in is what
# groups it, so a file per subject costs nothing and helpers can be shared
# between them.
set(MONO_CORPUS_OUTPUTS "")
file(GLOB _sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" *.cs)
mono_corpus_cs(interp-tests.dll LIBRARY SOURCES ${_sources})

# A threshold of zero never promotes, so a method entered at tier 0 is still
# interpreted however many times a test calls it.
mono_add_test_methods(
  ASSEMBLY "${CMAKE_CURRENT_BINARY_DIR}/interp-tests.dll"
  NAME_PREFIX "interp-tests/"
  PROPERTIES LABELS interp-unit
  ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
              "MONO_LLVM_JIT_TIER1_THRESHOLD=0")

mono_corpus_target(interp-test-suites DEPENDS interp-test-runner)
