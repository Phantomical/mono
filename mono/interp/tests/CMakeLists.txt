# The interpreter's managed tests.
#
# Each test method in the built assemblies becomes its own ctest test, run in
# its own process by runner.cpp, so a failure names the method and the engine it
# ran on, and --rerun-failed re-runs only what broke.
#
# discover.cmake lists those methods when ctest starts rather than at configure
# time, so writing a new test means editing a source and building, with no cmake
# step in between.

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
# native.cpp holds what the P/Invoke tests call.  ENABLE_EXPORTS puts its names
# in the dynamic symbol table, which is where DllImport ("__Internal") looks.
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
# Registers one ctest test per method of an assembly.  Registering the same
# assembly again under another name prefix and environment is what runs those
# methods on a second engine.
#
# A method named in XFAIL is registered with WILL_FAIL, so it reports as passing
# while it returns the wrong answer and starts failing the day it is fixed.  The
# list is per registration: a divergence is usually wrong on one engine only.
#
# A suite whose assembly is not on disk registers one failing test carrying the
# same properties, so an unbuilt suite reports under its own label rather than
# under whatever ran it.
function(mono_add_test_methods)
  cmake_parse_arguments(ARG ""
    "ASSEMBLY;NAME_PREFIX;NAME_SUFFIX;TIMEOUT;ONLY;ARM" "PROPERTIES;ENVIRONMENT;XFAIL" ${ARGN})

  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 60)
  endif()

  # The generated files are named after the assembly and the arm together, so
  # neither a second assembly nor a second engine overwrites the first.
  cmake_path(GET ARG_ASSEMBLY STEM _stem)
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
# One assembly, linked out of a netmodule per source: all of cs/ through csc,
# and each `.il` through ilasm, which takes one source at a time.  A test can
# then call a helper written in the other language, and a whole suite is one
# discovery rather than one per file.
set(MONO_CORPUS_OUTPUTS "")
set(_modules "")

file(GLOB _sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" cs/*.cs)
mono_corpus_cs(interp-tests-cs.netmodule MODULE DEBUG SOURCES ${_sources})
list(APPEND _modules "${CMAKE_CURRENT_BINARY_DIR}/interp-tests-cs.netmodule")

# IL shapes C# does not emit.
file(GLOB _il_sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" il/*.il)

# Mono recognizes a magic type by its assembly's name, so these two cannot be
# modules: a module carries no Assembly row, and a type in one is never magic.
# Merging them leaves interp_handle_magic_type_intrinsics () unreached and every
# test still passing, since the managed bodies are real implementations.
set(_own_assembly il/folding.il il/magic-conv.il)
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

# Methods each engine answers wrongly, with the defect named against each.  A
# fix turns the test red, which is why they are listed rather than deleted.
set(MONO_INTERP_TESTS_XFAIL
  # A store to a remote field goes through managed code, and the exception it
  # raises is lost crossing the interpreted frame back.
  "OpcodesTail:test_1_proxy_write_that_fails"
  "OpcodesTail:test_1_proxy_write_vt_that_fails"
  # Reflection over a P/Invoke that returns a struct loses half of it. Both
  # engines do, so it is a marshalling defect rather than a tier seam.
  "EntryTail:test_4_reflection_invoke_of_pinvoke_struct_return"
  "EntryTail:test_4_dynamic_invoke_over_pinvoke_struct_return")

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
  # Reflection over a P/Invoke that returns a struct loses half of it. Both
  # engines do, so it is a marshalling defect rather than a tier seam.
  "EntryTail:test_4_reflection_invoke_of_pinvoke_struct_return"
  "EntryTail:test_4_dynamic_invoke_over_pinvoke_struct_return"
  # The interpreter addresses a local with 16 bits and refuses a body whose
  # locals reach 64 KB. Compiled code has no such limit, and in the default
  # configuration the refusal is what sends the method to be compiled.
  "Limits:test_1_locals_size_too_big")

set(MONO_INTERP_TESTS_MIXED_XFAIL "")

foreach(_assembly IN LISTS _assemblies)
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
  # does above says the engines disagree about its IL.
  mono_add_test_methods(
    ASSEMBLY "${_assembly}"
    NAME_PREFIX "jit/"
    PROPERTIES LABELS interp-jit
    XFAIL ${MONO_INTERP_TESTS_JIT_XFAIL}
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
                "MONO_LLVM_JIT_TIER0=0")

  # Promotion on the second call, so a method is interpreted and then compiled
  # while the process runs.  Both engines are live in the one process, which is
  # the only way an interpreted frame calls a body that already has code.
  #
  # This arm is red, and on one defect rather than many: a compile taken from
  # the promotion path trips a SelectionDAG assertion in LLVM and the process
  # aborts.  The same methods under MONO_LLVM_JIT_TIER0=0, which compiles them
  # on the calling thread, are clean.
  mono_add_test_methods(
    ASSEMBLY "${_assembly}"
    NAME_PREFIX "mixed/"
    PROPERTIES LABELS interp-mixed
    XFAIL ${MONO_INTERP_TESTS_MIXED_XFAIL}
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
                "MONO_LLVM_JIT_TIER1_THRESHOLD=2")
endforeach()

# The two arms below change what the transform emits rather than which engine
# runs it, so both ask one question of a whole suite: does the program still
# answer the same.  A class answers that question only where the thing the arm
# varies could reach it, so each takes the classes marked with its attribute --
# cs/arms.cs holds both and says what marking one means.
#
# The instrumented arm turns on everything that instruments the interpreter,
# which nothing else here does.  Sequence points and the transform's tracing
# splice instructions into the stream, which moves every code offset an EH
# clause and a branch are written in terms of.
foreach(_assembly IN LISTS _assemblies)
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

  # The interpreter with its own optimizations turned off, so the transform
  # emits what it read rather than what it worked out.
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

# A wrapper is compiled whatever tier its target runs in, so the opcodes only a
# wrapper carries never reach the transform.  `interp-only=<class>` hands every
# method of a class to the interpreter, wrappers included, and it names one
# class -- so this arm runs the methods of that class and nothing else.
mono_add_test_methods(
  ASSEMBLY "${CMAKE_CURRENT_BINARY_DIR}/interp-tests.dll"
  NAME_PREFIX "wrappers/"
  ONLY "^WrappersIL:"
  PROPERTIES LABELS interp-unit
  ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc"
              "MONO_LLVM_JIT_TIER1_THRESHOLD=0"
              "MONO_INTERP_TESTS_OPTS=interp-only=WrappersIL")

mono_corpus_target(interp-test-suites DEPENDS interp-test-runner)
