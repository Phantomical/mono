# The backend's tests.  They link the backend and drive it directly rather than
# running managed code through the mono binary.

if(NOT MONO_ENABLE_LLVM OR NOT MONO_ENABLE_SGEN OR NOT MONO_ENABLE_LIBRARIES)
  # No backend to test, or no monosgen-static to link it against.
  return()
endif()

# Most of the cases drive method_to_llvm () over real metadata. Each one is a
# method in a `.il` file under il/ that ilasm assembles into an assembly the
# test loads. The sources compile either way. A fixture with no corpus to load
# skips itself instead of vanishing from `ctest -N`. A missing case there
# reads the same as a passed one.
set(MONO_LLVM_TEST_SOURCES
    thunk-tests.cpp jitlink-memory-tests.cpp mono-lsda-tests.cpp gdb-jit-tests.cpp
    harness.cpp translator-tests.cpp jit-tests.cpp compiler-tests.cpp
    compile-queue-tests.cpp interp-entry-tests.cpp perf-eh-frame-tests.cpp
    unwind-records-tests.cpp unwind-transcode-tests.cpp inline-copies-tests.cpp
    profile-counters-tests.cpp profile-counter-promoter-tests.cpp
    rgctx-fetch-tests.cpp rgctx-dedup-tests.cpp clamp-frame-align-tests.cpp
    array-address-tests.cpp array-shape-tests.cpp ir-dump-tests.cpp
    cast-func-tests.cpp fold-delegate-tests.cpp inline-heat-tests.cpp
    gc-barrier-tests.cpp operand-class-tests.cpp dead-alloc-tests.cpp
    escape-tests.cpp promote-alloc-tests.cpp constant-values-tests.cpp
    one-file-vfs-tests.cpp hidden-return-tests.cpp inline-save-lmf-tests.cpp
    fold-empty-finally-tests.cpp)

if(MONO_CORPUS_ENABLED)
  # RELATIVE, because mono_corpus_il resolves its source against the current
  # source directory itself.
  file(GLOB MONO_LLVM_TEST_IL CONFIGURE_DEPENDS
       RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" il/*.il)
  foreach(ilfile IN LISTS MONO_LLVM_TEST_IL)
    cmake_path(GET ilfile STEM testname)
    mono_corpus_il("${testname}.dll" "${ilfile}" LIBRARY)
  endforeach()

  add_custom_target(test-llvm-data ALL DEPENDS ${MONO_CORPUS_OUTPUTS})
  mono_tools_runtime_depends(test-llvm-data)
endif()

mono_gtest_binary(test-llvm
  SOURCES ${MONO_LLVM_TEST_SOURCES}
  LIBRARIES monosgen-static
  INCLUDES
    "${CMAKE_SOURCE_DIR}/mono/llvm"
    "${CMAKE_SOURCE_DIR}/mono/mini"
    "${CMAKE_BINARY_DIR}/mono/mini"
  DEFINITIONS
    MONO_LLVM_TESTS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    MONO_LLVM_TESTS_ASSEMBLIES="${MONO_CORPUS_CLASS_DIR}"
    MONO_LLVM_TESTS_CFG_DIR="${CMAKE_BINARY_DIR}/runtime/etc"
    # The `.ll` shapes a pass test parses. Nothing builds them, so they are read
    # where they are written rather than copied.
    MONO_LLVM_TESTS_IR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ir")

# Tier 2 off, which is not the runtime's default. These cases read the shape
# of a tier-1 body and link one against a jit of their own. With tier 2 on,
# the instrumentation puts a counter block in front of every body and a call
# to a symbol only the runtime defines. Then a case that counts blocks or
# materializes a module reads tier 2's shape and not the shape it is about.
# JitProfile below is the suite that wants it on.
mono_gtest_tests(test-llvm
  PREFIX test-llvm
  FILTER "-JitProfile.*"
  SKIP_REGEX "\\[  SKIPPED \\]"
  PROPERTIES
    LABELS llvm
    ENVIRONMENT "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2=0")

# The JitProfile cases read the counters a tier-1 body carries. A body
# promoted while a case reads it answers for code that no longer counts. So
# they run apart from the rest, under a threshold no case can reach, that
# holds every method at tier 1 for the process. options.cpp reads the
# threshold once and caches it, so it cannot be set per case.
mono_gtest_tests(test-llvm
  PREFIX test-llvm-profile
  FILTER "JitProfile.*"
  SKIP_REGEX "\\[  SKIPPED \\]"
  PROPERTIES
    LABELS llvm
    ENVIRONMENT "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=1000000000")
