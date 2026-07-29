# Native unit tests.  Each one is a separate executable so that a skip (exit
# 77) or a crash is attributed to exactly one check.
#
# Each test is its body plus main.c compiled with -DMAIN=<function>, which is
# how the automake build shared a single entry point across all of them.

# The corpus-driven checks come first: each is a managed program run under the
# mono binary plus a Python script asserting on what a pass decided, so unlike
# everything below they link nothing and do not care which collector is built.
if(MONO_CORPUS_ENABLED)
  add_subdirectory(inliner-tags)
  add_subdirectory(devirt-tags)
  add_subdirectory(statics-symbols)
  add_subdirectory(il-offsets)
  add_subdirectory(seq-points)
endif()

if(NOT MONO_ENABLE_SGEN)
  # The tests link the SGen runtime.
  return()
endif()

# test-mono-callspec.c calls mono_set_assemblies_path ("../../mcs/class/lib/
# net_4_x"), which is relative to the working directory and so only resolved
# because automake built in the source tree.  Rather than patch the test, give
# the build tree an mcs/ that points at the real one.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/mcs")
  file(CREATE_LINK "${MONO_MCS_TOPDIR}" "${CMAKE_BINARY_DIR}/mcs" SYMBOLIC)
endif()

# RUNTIME means the test starts a runtime and therefore links the whole JIT;
# the others only need metadata and the collector.
function(mono_add_unit_test name)
  cmake_parse_arguments(ARG "RUNTIME" "ENTRY" "SOURCES" ${ARGN})

  set(_sources ${ARG_SOURCES})
  if(ARG_ENTRY)
    # Source properties are scoped to a directory, not to a target, so every
    # test needs its own copy of the shim to carry its own -DMAIN.  The shim
    # is also the only file that sees HAVE_SGEN_GC: mini.h refuses to compile
    # when a collector is selected, and several of the bodies include it.
    set(_shim "${CMAKE_CURRENT_BINARY_DIR}/main-${name}.c")
    configure_file(main.c "${_shim}" COPYONLY)
    set_source_files_properties("${_shim}" PROPERTIES
      COMPILE_DEFINITIONS "MAIN=${ARG_ENTRY};HAVE_SGEN_GC")
    list(APPEND _sources "${_shim}")
  endif()

  add_executable(${name} ${_sources})

  target_include_directories(${name} PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/mono"
    "${CMAKE_SOURCE_DIR}/samples"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src")
  target_compile_definitions(${name} PRIVATE
    MONO_BINDIR="${CMAKE_INSTALL_FULL_BINDIR}/"
    MONO_ASSEMBLIES="${MONO_ASSEMBLIES_DIR}"
    MONO_CFG_DIR="${MONO_CFG_DIR}")
  target_link_libraries(${name} PRIVATE mono::common mono::eglib_headers)

  # RUNTIME tests link the whole runtime archive; the rest only reach the
  # metadata and collector members, which archive semantics leave to the
  # linker to work out.
  if(ARG_RUNTIME)
    target_link_libraries(${name} PRIVATE monosgen-static)
  else()
    target_link_libraries(${name} PRIVATE
      monoruntime_sgen monoruntime_config monoruntime_support monoculture
      monosgen monoutils eglib)
  endif()

  # The LLVM libraries have to come after libmini, which references them.
  target_link_libraries(${name} PRIVATE mono::llvm m)
  if(ZLIB_FOUND)
    target_link_libraries(${name} PRIVATE ZLIB::ZLIB)
  endif()
  # libmini contains C++, so the test binaries need the C++ runtime.
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)

  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "MONO_PATH=${MONO_MCS_TOPDIR}/class/lib/build")
endfunction()

mono_add_unit_test(test-sgen-qsort
  SOURCES test-sgen-qsort.c            ENTRY test_sgen_qsort_main)
mono_add_unit_test(test-memfuncs
  SOURCES test-memfuncs.c              ENTRY test_memfuncs_main)
mono_add_unit_test(test-mono-linked-list-set
  SOURCES test-mono-linked-list-set.c  ENTRY test_mono_linked_list_set_main)
mono_add_unit_test(test-conc-hashtable
  SOURCES test-conc-hashtable.c        ENTRY test_conc_hashtable_main)
mono_add_unit_test(test-mono-handle
  SOURCES test-mono-handle.c           ENTRY test_mono_handle_main)
mono_add_unit_test(test-mono-callspec
  SOURCES test-mono-callspec.c         ENTRY test_mono_callspec_main       RUNTIME)
mono_add_unit_test(test-mono-string
  SOURCES test-mono-string.c           ENTRY test_mono_string_main         RUNTIME)
mono_add_unit_test(test-mono-embed
  SOURCES test-mono-embed.c            ENTRY test_mono_embed_main          RUNTIME)
mono_add_unit_test(test-embed-invoke
  SOURCES test-embed-invoke.c          ENTRY test_mono_embed_invoke_main   RUNTIME)
# test-path brings its own main rather than using the shared shim.
mono_add_unit_test(test-path SOURCES test-path.c RUNTIME)

if(MONO_ENABLE_LLVM)
  # These are C++: they include mono/mini/llvm/*.hpp and drive the engine, the
  # .mono_lsda parser and the class-init analysis through their C++ interfaces,
  # so they cannot live inside libmono's C surface.
  mono_add_unit_test(test-llvm-engine
    SOURCES test-llvm-engine.cpp     ENTRY test_llvm_engine_main      RUNTIME)
  # The relocation-width check gets a program of its own: it is the one check
  # that can legitimately not run (its classifier covers x86-64 ELF only), and
  # a result is reported per program -- folded into test-llvm-engine it would
  # show a green board while testing nothing.
  mono_add_unit_test(test-llvm-reloc-widths
    SOURCES test-llvm-engine.cpp     ENTRY test_llvm_reloc_widths_main RUNTIME)
  mono_add_unit_test(test-llvm-ehframe
    SOURCES test-llvm-ehframe.c      ENTRY test_llvm_ehframe_main     RUNTIME)
  mono_add_unit_test(test-llvm-ehtable
    SOURCES test-llvm-ehtable.cpp    ENTRY test_llvm_ehtable_main     RUNTIME)
  mono_add_unit_test(test-llvm-jitdump
    SOURCES test-llvm-jitdump.cpp    ENTRY test_llvm_jitdump_main     RUNTIME)
  mono_add_unit_test(test-llvm-class-init
    SOURCES test-llvm-class-init.cpp ENTRY test_llvm_class_init_main  RUNTIME)
  # C++ for the same reason again: it includes llvm/passes/wbarrier.hpp and
  # drives the write-barrier lowering over IR it assembles by hand.
  mono_add_unit_test(test-llvm-wbarrier
    SOURCES test-llvm-wbarrier.cpp   ENTRY test_llvm_wbarrier_main    RUNTIME)
  # C++ again: it includes llvm/engine.hpp and JITs modules whose debug info has
  # the shape the translator produces, to check what comes back out of it.
  mono_add_unit_test(test-llvm-il-debug
    SOURCES test-llvm-il-debug.cpp   ENTRY test_llvm_il_debug_main    RUNTIME)
endif()

# The managed side of the tests that need one.
if(MONO_ENABLE_MCS_BUILD)
  set(_class_dir "${MONO_MCS_TOPDIR}/class/lib/${MONO_DEFAULT_PROFILE}")
  set(_build_dir "${MONO_MCS_TOPDIR}/class/lib/build")
  mono_tools_runtime(_host MONO_PATH "${_build_dir}")
  set(_mcs ${_host}
           "${MONO_CSC}" -debug:portable -noconfig -nologo -unsafe
           -nowarn:0162 -nowarn:0168 -nowarn:0219 -nowarn:0414 -nowarn:0618
           -nowarn:0169 -nowarn:1690 -nowarn:0649 -nowarn:0612 -nowarn:3021
           -nowarn:0197
           "-r:${_class_dir}/System.dll"
           "-r:${_class_dir}/System.Xml.dll"
           "-r:${_class_dir}/System.Core.dll")

  function(_mono_test_assembly out source)
    add_custom_command(
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
      COMMAND ${_mcs} "-out:${CMAKE_CURRENT_BINARY_DIR}/${out}" "${source}"
      DEPENDS "${source}"
      COMMENT "CSC ${out}"
      VERBATIM)
  endfunction()

  _mono_test_assembly(callspec.exe "${CMAKE_CURRENT_SOURCE_DIR}/callspec.cs")
  _mono_test_assembly(test-embed.exe "${CMAKE_SOURCE_DIR}/samples/embed/test.cs")
  _mono_test_assembly(test-embed-invoke-cs.exe "${CMAKE_SOURCE_DIR}/samples/embed/invoke.cs")

  add_custom_target(unit-test-assemblies DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/callspec.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed-invoke-cs.exe")
  mono_tools_runtime_depends(unit-test-assemblies)

  # Same trick as the mini corpora: CTest cannot build, so building the managed
  # inputs is itself a fixture the tests that need them depend on.
  add_test(NAME unit-test-assemblies
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                   --target unit-test-assemblies)
  set_tests_properties(unit-test-assemblies PROPERTIES FIXTURES_SETUP unit_assemblies LABELS fixture)
  foreach(_t test-mono-callspec test-mono-embed test-embed-invoke)
    set_tests_properties(${_t} PROPERTIES
      FIXTURES_REQUIRED unit_assemblies
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  endforeach()
endif()
