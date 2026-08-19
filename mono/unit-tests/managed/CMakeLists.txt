# Managed tests written as test_N_<name> methods rather than as programs with a
# Main of their own, so that many of them share one assembly.
#
# Each method becomes its own ctest test, run in its own process by runner.cpp,
# once per engine arm.  MONO_MERGED_TESTS makes an arm one ctest test instead,
# and runner.cpp runs that arm's methods in one process.
#
# discover.cmake lists those methods when ctest starts rather than at configure
# time, so writing a new test means editing a source and building, with no cmake
# step in between.

if(NOT MONO_CORPUS_ENABLED)
  # No class-library toolchain, so nothing to compile the suites with.
  return()
endif()
if(NOT MONO_ENABLE_INTERPRETER OR NOT MONO_ENABLE_SGEN OR NOT MONO_ENABLE_LIBRARIES)
  # No interpreter to run the arms on, or no monosgen-static for the runner.
  return()
endif()

# ---------------------------------------------------------------------------
# The runner
# ---------------------------------------------------------------------------
# Native methods for the P/Invoke tests.
add_executable(interp-test-runner runner.cpp native.cpp)
set_target_properties(interp-test-runner PROPERTIES ENABLE_EXPORTS TRUE)

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
# Registers the test methods of an assembly as ctest tests, one per method --
# or one per call under MONO_MERGED_TESTS.  Use the name prefix and the
# environment to tell apart different runs of the same tests.
#
#   ASSEMBLY     the .dll whose test methods to register.
#   NAME_PREFIX  and NAME_SUFFIX go around "Class:method" in the ctest name.
#                A merged call puts the assembly's own name between them.
#   TIMEOUT      seconds the enumeration gets, not the tests.  A test takes its
#                own through PROPERTIES.  A merged call enumerates nothing and
#                ignores this.
#   ONLY         takes only the methods whose "Class:method" matches this regex.
#   ARM          takes only the classes carrying that arm's attribute.
#   PROPERTIES   set on every test this call registers.
#   ENVIRONMENT  VAR=value, for the tests and for the enumeration.
#   XFAIL        "Class:method" entries to register with WILL_FAIL.  Per
#                registration: a divergence is usually wrong on one engine only.
#
# discover.cmake does the enumeration and says what ONLY and ARM select from.
function(mono_add_test_methods)
  cmake_parse_arguments(ARG ""
    "ASSEMBLY;NAME_PREFIX;NAME_SUFFIX;TIMEOUT;ONLY;ARM" "PROPERTIES;ENVIRONMENT;XFAIL" ${ARGN})

  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 60)
  endif()

  # Distinguishes the same assembly under different conditions.
  cmake_path(GET ARG_ASSEMBLY STEM _stem)

  # Under MONO_MERGED_TESTS the runner takes over what discover.cmake does with
  # the listing: it applies ONLY and ARM itself, and XFAIL becomes --xfail
  # rather than WILL_FAIL, because one ctest test now covers every method.
  if(MONO_MERGED_TESTS)
    set(_name "${ARG_NAME_PREFIX}${_stem}${ARG_NAME_SUFFIX}")
    set(_filters "")
    if(ARG_ONLY)
      list(APPEND _filters --only "${ARG_ONLY}")
    endif()
    if(ARG_ARM)
      list(APPEND _filters --arm "${ARG_ARM}")
    endif()
    foreach(_x IN LISTS ARG_XFAIL)
      list(APPEND _filters --xfail "${_x}")
    endforeach()

    set(_env "")
    if(ARG_ENVIRONMENT)
      set(_env "${CMAKE_COMMAND}" -E env ${ARG_ENVIRONMENT} --)
    endif()

    add_test(NAME "${_name}" COMMAND ${_env}
             "$<TARGET_FILE:interp-test-runner>" --all "${ARG_ASSEMBLY}" ${_filters})
    set_tests_properties("${_name}" PROPERTIES
                         WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" ${ARG_PROPERTIES})
    return()
  endif()

  string(REGEX REPLACE "[^A-Za-z0-9_.-]" "_" _key
         "${ARG_NAME_PREFIX}${_stem}${ARG_NAME_SUFFIX}${ARG_ONLY}${ARG_ARM}")
  set(_include "${CMAKE_CURRENT_BINARY_DIR}/${_key}_include.cmake")
  set(_tests   "${CMAKE_CURRENT_BINARY_DIR}/${_key}_tests.cmake")
  # One per assembly, not per arm: this is the file the runtime start pays for.
  set(_list    "${CMAKE_CURRENT_BINARY_DIR}/${_stem}_methods.txt")

  set(_args "")
  foreach(_a PROPERTIES ENVIRONMENT XFAIL ONLY ARM)
    if(ARG_${_a})
      string(APPEND _args "      ${_a} [==[${ARG_${_a}}]==]\n")
    endif()
  endforeach()

  file(GENERATE OUTPUT "${_include}" CONTENT
"if(EXISTS \"$<TARGET_FILE:interp-test-runner>\" AND EXISTS \"${ARG_ASSEMBLY}\")
  include(\"${CMAKE_CURRENT_SOURCE_DIR}/discover.cmake\")
  mono_list_test_methods_impl(
    CMAKE_EXE [==[${CMAKE_COMMAND}]==]
    RUNNER [==[$<TARGET_FILE:interp-test-runner>]==]
    ASSEMBLY [==[${ARG_ASSEMBLY}]==]
    LIST_FILE [==[${_list}]==]
    WORKING_DIR [==[${CMAKE_CURRENT_BINARY_DIR}]==]
    TIMEOUT [==[${ARG_TIMEOUT}]==]
    ENVIRONMENT [==[${ARG_ENVIRONMENT}]==])
  if(NOT EXISTS \"${_tests}\"
     OR NOT \"${_tests}\" IS_NEWER_THAN \"${_list}\"
     OR NOT \"${_tests}\" IS_NEWER_THAN \"\${CMAKE_CURRENT_LIST_FILE}\")
    mono_discover_test_methods_impl(
      CMAKE_EXE [==[${CMAKE_COMMAND}]==]
      RUNNER [==[$<TARGET_FILE:interp-test-runner>]==]
      ASSEMBLY [==[${ARG_ASSEMBLY}]==]
      LIST_FILE [==[${_list}]==]
      CTEST_FILE [==[${_tests}]==]
      NAME_PREFIX [==[${ARG_NAME_PREFIX}]==]
      NAME_SUFFIX [==[${ARG_NAME_SUFFIX}]==]
      WORKING_DIR [==[${CMAKE_CURRENT_BINARY_DIR}]==]
${_args}    )
  endif()
  include(\"${_tests}\")
else()
  add_test([==[${ARG_NAME_PREFIX}!not-built:${_stem}${ARG_NAME_SUFFIX}]==]
           [==[${CMAKE_COMMAND}]==] [==[-E]==] [==[false]==])
  set_tests_properties([==[${ARG_NAME_PREFIX}!not-built:${_stem}${ARG_NAME_SUFFIX}]==]
                       PROPERTIES ${ARG_PROPERTIES})
endif()
")

  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_include}")
endfunction()

# ---------------------------------------------------------------------------
# The suites
# ---------------------------------------------------------------------------
set(MONO_CORPUS_OUTPUTS "")
set(_modules "")

file(GLOB _sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" *.cs)
mono_corpus_cs(interp-tests-cs.netmodule MODULE DEBUG SOURCES ${_sources})
list(APPEND _modules "${CMAKE_CURRENT_BINARY_DIR}/interp-tests-cs.netmodule")

# IL shapes C# does not emit.
file(GLOB _il_sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" *.il)

# These set their own assembly names, so they cannot be merged with the others.
set(_own_assembly folding.il magic-conv.il)
set(_assemblies "")

foreach(_il IN LISTS _il_sources)
  cmake_path(GET _il STEM _stem)
  if("${_il}" IN_LIST _own_assembly)
    mono_corpus_il("${_stem}.dll" "${_il}" LIBRARY DEBUG)
    list(APPEND _assemblies "${CMAKE_CURRENT_BINARY_DIR}/${_stem}.dll")
  else()
    mono_corpus_il("${_stem}.netmodule" "${_il}" MODULE DEBUG)
    list(APPEND _modules "${CMAKE_CURRENT_BINARY_DIR}/${_stem}.netmodule")
  endif()
endforeach()

mono_corpus_link(interp-tests.dll MODULES ${_modules})
list(APPEND _assemblies "${CMAKE_CURRENT_BINARY_DIR}/interp-tests.dll")

# Expected failures
set(MONO_INTERP_TESTS_XFAIL
  # A store to a remote field goes through managed code, and the exception it
  # raises is lost crossing the interpreted frame back.
  "OpcodesTail:test_1_proxy_write_that_fails"
  "OpcodesTail:test_1_proxy_write_vt_that_fails")

set(MONO_INTERP_TESTS_JIT_XFAIL
  # ECMA-335 Table III.9 fills a native unsigned int with zeroes, where the
  # backend sign extends as it does for a native int.
  "NativeInt:test_1_stsfld_zero_extends"
  "NativeInt:test_1_call_argument_zero_extends"
  "NativeInt:test_1_ret_zero_extends"
  # IL ECMA-335 does not allow, which the translator refuses and the
  # interpreter runs anyway.
  "Refusals:test_5_constrained_on_call"
  "Refusals:test_9_localloc_with_stack"
  "Refusals:test_7_unaligned_three"
  "Merge:test_1_merge_i4_into_i8"
  "Merge:test_1_branch_type_mismatch"
  # An unchecked float to integer conversion whose source is out of range.
  # ECMA-335 III.3.27 leaves the result unspecified: the interpreter reproduces
  # cvttsd2si and the backend saturates.
  "Conversions:test_1_i4_from_nan_r8_is_int_min"
  "Conversions:test_1_i4_from_positive_infinity_is_int_min"
  "Conversions:test_1_u4_from_out_of_range_r8_keeps_the_low_word"
  "Conversions:test_1_u4_from_negative_r8_wraps"
  "Conversions:test_1_i8_from_out_of_range_r8_is_long_min"
  # The backend loses an exception raised inside a filter.
  "Exceptions:test_2_an_exception_in_a_filter_continues_the_search"
  # Compiled code polls for nothing, so a thread running it takes an abort only
  # when it next calls something that does.
  "Threading:test_2_abort_in_an_interpreted_loop"
  # A conversion whose operand is a managed pointer.  ECMA-335 Table III.8
  # leaves those cells empty, so the translator is right to refuse.
  "ConvIL:test_1_conv_u4_from_mp_truncates"
  "ConvIL:test_1_conv_i4_from_mp_truncates"
  "ConvIL:test_1_conv_ovf_u4_from_mp"
  # The interpreter addresses a local with 16 bits and refuses a body whose
  # locals reach 64 KB. Compiled code has no such limit, and in the default
  # configuration the refusal is what sends the method to be compiled.
  "Limits:test_1_locals_size_too_big")

set(MONO_INTERP_TESTS_MIXED_XFAIL "")

foreach(_assembly IN LISTS _assemblies)
  # These run purely on the interpreter.
  mono_add_test_methods(
    ASSEMBLY "${_assembly}"
    NAME_PREFIX "interp/"
    PROPERTIES LABELS interp-unit
    XFAIL ${MONO_INTERP_TESTS_XFAIL}
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
                "MONO_LLVM_JIT_TIER1_THRESHOLD=0")

  # These run purely on the tier1 jit
  mono_add_test_methods(
    ASSEMBLY "${_assembly}"
    NAME_PREFIX "interp-jit/"
    PROPERTIES LABELS interp-jit
    XFAIL ${MONO_INTERP_TESTS_JIT_XFAIL}
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
                "MONO_LLVM_JIT_TIER0=0")

  # First call on the interpreter, which promotes immediately.
  #
  # This has some test failures and so is not included by default.
  mono_add_test_methods(
    ASSEMBLY "${_assembly}"
    NAME_PREFIX "mixed/"
    PROPERTIES LABELS interp-mixed
    XFAIL ${MONO_INTERP_TESTS_MIXED_XFAIL}
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
                "MONO_LLVM_JIT_TIER1_THRESHOLD=2")
endforeach()

foreach(_assembly IN LISTS _assemblies)
  # Test with all debug instrumenting enabled.
  mono_add_test_methods(
    ASSEMBLY "${_assembly}"
    NAME_PREFIX "instrumented/"
    ARM instrumented
    PROPERTIES LABELS interp-unit
    XFAIL ${MONO_INTERP_TESTS_XFAIL}
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
                "MONO_LLVM_JIT_TIER1_THRESHOLD=0"
                "MONO_DEBUG=gen-seq-points"
                # The transform's tracing decodes each opcode's operands from a
                # table it keeps apart from the handlers, so it is worth running
                # even where nothing reads what it prints.  A bare name matches
                # that method in every class, and the constructors a process
                # transforms cover most of the operand kinds.
                "MONO_VERBOSE_METHOD=.ctor"
                "MONO_INTERP_TESTS_TRACE=all"
                "MONO_INTERP_TESTS_COVERAGE=1")

  # Run with interpreter optimizations disabled.
  mono_add_test_methods(
    ASSEMBLY "${_assembly}"
    NAME_PREFIX "noopt/"
    ARM noopt
    PROPERTIES LABELS interp-unit
    XFAIL ${MONO_INTERP_TESTS_XFAIL}
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
                "MONO_LLVM_JIT_TIER1_THRESHOLD=0"
                "MONO_INTERP_TESTS_OPTS=-all")
endforeach()

# Special handling for wrapper tests.
mono_add_test_methods(
  ASSEMBLY "${CMAKE_CURRENT_BINARY_DIR}/interp-tests.dll"
  NAME_PREFIX "wrappers/"
  ONLY "^WrappersIL:"
  PROPERTIES LABELS interp-unit
  ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
              "MONO_LLVM_JIT_TIER1_THRESHOLD=0"
              "MONO_INTERP_TESTS_OPTS=interp-only=WrappersIL")

mono_corpus_target(interp-test-suites DEPENDS interp-test-runner)
