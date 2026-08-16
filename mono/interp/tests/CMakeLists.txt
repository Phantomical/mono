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
#     [PROPERTIES <prop> <value>...] [ENVIRONMENT <VAR=value>...]
#     [XFAIL <Class:method>...])
#
# The name parts and the environment go together: registering one assembly twice
# under two of each is what would run the same methods on a second engine.
#
# A method named in XFAIL is registered with WILL_FAIL, so it reports as passing
# while it returns the wrong answer and starts failing the day it is fixed.  The
# list is per registration: a divergence is usually wrong on one engine only.
function(mono_add_test_methods)
  cmake_parse_arguments(ARG ""
    "ASSEMBLY;NAME_PREFIX;NAME_SUFFIX;TIMEOUT" "PROPERTIES;ENVIRONMENT;XFAIL" ${ARGN})

  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 60)
  endif()

  # The generated files are named after the assembly and the arm together, so
  # neither a second assembly nor a second engine overwrites the first.
  cmake_path(GET ARG_ASSEMBLY STEM _stem)
  string(REGEX REPLACE "[^A-Za-z0-9_.-]" "_" _key
         "${ARG_NAME_PREFIX}${_stem}${ARG_NAME_SUFFIX}")
  set(_include "${CMAKE_CURRENT_BINARY_DIR}/${_key}_include.cmake")
  set(_tests   "${CMAKE_CURRENT_BINARY_DIR}/${_key}_tests.cmake")

  set(_args "")
  foreach(_a PROPERTIES ENVIRONMENT XFAIL)
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

# IL shapes C# does not emit -- a merge point whose predecessors leave different
# widths, `jmp`, an explicit `tail.`, the type-unsafe prefixes.  ilasm takes one
# source at a time, so each of these is an assembly of its own.
file(GLOB _il_sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" il/*.il)
set(_il_assemblies "")
foreach(_il IN LISTS _il_sources)
  cmake_path(GET _il STEM _stem)
  mono_corpus_il("${_stem}.dll" "${_il}" LIBRARY)
  list(APPEND _il_assemblies "${CMAKE_CURRENT_BINARY_DIR}/${_stem}.dll")
endforeach()

# Methods each engine answers wrongly, with the defect named against each.  A
# fix turns the test red, which is why they are listed rather than deleted.
set(MONO_INTERP_TESTS_XFAIL
  # An int32 is never widened into a native-int location.
  "NativeInt:test_5_stsfld_widens"
  "NativeInt:test_5_stind_i_widens"
  "NativeInt:test_1_stind_i_sign_extends"
  # A native-int operand is read 32 bits wide.
  "NativeInt:test_1_wide_array_index_throws"
  "NativeInt:test_7_wide_switch_index_falls_through"
  "Arrays:test_1_a_long_index_past_the_end_throws"
  # Special-static access does not run the class initializer.
  "ClassInit:test_22_thread_static_runs_the_cctor"
  "ClassInit:test_23_context_static_runs_the_cctor"
  # An initonly static is folded into the bytecode that reads it.
  "Statics:test_99_initonly_write_is_seen")

set(MONO_INTERP_TESTS_JIT_XFAIL
  # IL ECMA-335 does not allow, which the translator refuses and the
  # interpreter runs anyway.
  "Refusals:test_5_constrained_on_call"
  "Refusals:test_9_localloc_with_stack"
  "Refusals:test_7_unaligned_three"
  "Merge:test_1_merge_i4_into_i8"
  # An unchecked float to integer conversion whose source is out of range.
  # ECMA-335 III.3.27 leaves the result unspecified: the interpreter reproduces
  # cvttsd2si and the backend saturates.
  "Conversions:test_1_i4_from_nan_r8_is_int_min"
  "Conversions:test_1_i4_from_positive_infinity_is_int_min"
  "Conversions:test_1_u4_from_out_of_range_r8_keeps_the_low_word"
  "Conversions:test_1_u4_from_negative_r8_wraps"
  "Conversions:test_1_i8_from_out_of_range_r8_is_long_min"
  # The backend loses an exception raised inside a filter.
  "Exceptions:test_2_an_exception_in_a_filter_continues_the_search")

foreach(_assembly "${CMAKE_CURRENT_BINARY_DIR}/interp-tests.dll" ${_il_assemblies})
  # A threshold of zero never promotes, so a method entered at tier 0 is still
  # interpreted however many times a test calls it.
  mono_add_test_methods(
    ASSEMBLY "${_assembly}"
    NAME_PREFIX "interp/"
    PROPERTIES LABELS interp-unit
    XFAIL ${MONO_INTERP_TESTS_XFAIL}
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
                "MONO_LLVM_JIT_TIER1_THRESHOLD=0")

  # The same methods with no interpreter under them, so each is compiled at tier
  # 1 on the synchronous path.  A method that answers differently here than it
  # does above says the engines disagree about its IL.  This arm is a second
  # opinion on the same programs rather than more coverage of the interpreter,
  # so `check-interp` leaves it out.
  mono_add_test_methods(
    ASSEMBLY "${_assembly}"
    NAME_PREFIX "jit/"
    PROPERTIES LABELS interp-jit
    XFAIL ${MONO_INTERP_TESTS_JIT_XFAIL}
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
                "MONO_LLVM_JIT_TIER0=0")
endforeach()

mono_corpus_target(interp-test-suites DEPENDS interp-test-runner)
