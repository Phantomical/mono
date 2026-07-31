# Building and running managed test corpora.
#
# A corpus is a C#/IL program compiled by the class-library toolchain and run
# under the runtime this build produces.  Both mono/mini (the regression and
# tiering suites) and the per-pass checks under mono/unit-tests build them, so
# the compiler invocation, the fixture they hang off and the shape of a tiered
# test all live here rather than being spelled twice.
#
# Corpora are part of `all`, ordered after `mcs`: a finished build has every
# test input on disk, so running ctest never builds anything.

set(MONO_CORPUS_ENABLED FALSE)
if(NOT MONO_ENABLE_MCS_BUILD OR NOT MONO_ENABLE_EXECUTABLES)
  return()
endif()
set(MONO_CORPUS_ENABLED TRUE)

# The per-pass checks are Python; find the interpreter once, here, rather than
# in each directory that registers one.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MONO_CORPUS_CLASS_DIR "${MONO_MCS_TOPDIR}/class/lib/${MONO_DEFAULT_PROFILE}")
set(MONO_CORPUS_BUILD_DIR "${MONO_MCS_TOPDIR}/class/lib/build")
set(MONO_CORPUS_WRAPPER   "${CMAKE_BINARY_DIR}/runtime/mono-wrapper")
set(MONO_CORPUS_CSFLAGS   -unsafe -nowarn:0219,0169,0414,0649,0618)

# TestDriver.dll is shared by every corpus but built with the mini ones, so a
# corpus living anywhere else has to be pointed at it: mono resolves a
# reference from the running assembly's own directory, which is no longer the
# same directory.
set(MONO_CORPUS_MINI_DIR "${CMAKE_BINARY_DIR}/mono/mini")
set(MONO_CORPUS_MONO_PATH "${MONO_CORPUS_CLASS_DIR}:${MONO_CORPUS_MINI_DIR}")

# csc and ilasm run on whichever mono MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS selected;
# the corpora themselves are always compiled -nostdlib against this tree's
# mscorlib, so the host makes no difference to what comes out.
mono_tools_runtime(_host MONO_PATH "${MONO_CORPUS_BUILD_DIR}")
set(MONO_CORPUS_CSC ${_host}
    "${MONO_CSC}" -langversion:8.0 -nostdlib -unsafe -nowarn:0162 -nologo -noconfig
    "-r:${MONO_CORPUS_CLASS_DIR}/mscorlib.dll"
    "-r:${MONO_CORPUS_CLASS_DIR}/System.dll"
    "-r:${MONO_CORPUS_CLASS_DIR}/System.Core.dll")
set(MONO_CORPUS_ILASM ${_host} "${MONO_CORPUS_BUILD_DIR}/ilasm.exe")

# A tiered run is not a one-core test: besides the mutator it starts a pool of
# background compile workers, sized by tiered_compile_thread_count () in
# mono/mini/llvm/tiered.cpp.  Mirror that rule here so `ctest -j N` schedules
# against the cores these tests really use.  Without it ctest counts each run as
# one processor and packs N of them onto N cores, oversubscribing by the pool
# size -- and since every assertion about promotion is a bounded wait for work
# that happens on those very workers, the suite then fails on machine load
# rather than on anything it is testing.
#
# The count only has to be right on the machine that runs the tests, which is
# the one configuring: it is a scheduling hint, not a correctness contract.
include(ProcessorCount)
ProcessorCount(_ncpu)
if(_ncpu EQUAL 0)
  set(_ncpu 1)
endif()
math(EXPR _workers "${_ncpu} / 4")
if(_workers LESS 1)
  set(_workers 1)
elseif(_workers GREATER 4)
  set(_workers 4)
endif()
math(EXPR MONO_TIERED_PROCESSORS "${_workers} + 1")   # + the mutator

# Compile one C# corpus into the calling directory's binary dir, appending it to
# MONO_CORPUS_OUTPUTS in the caller's scope.
#
#   mono_corpus_cs(<out> [LIBRARY] SOURCES <src>... [REFS <dll>...])
function(mono_corpus_cs out)
  cmake_parse_arguments(ARG "LIBRARY" "" "SOURCES;REFS" ${ARGN})
  set(_extra "")
  if(ARG_LIBRARY)
    set(_extra -target:library)
  endif()
  set(_srcs "")
  foreach(_s IN LISTS ARG_SOURCES)
    list(APPEND _srcs "${CMAKE_CURRENT_SOURCE_DIR}/${_s}")
  endforeach()
  # A reference built in this same directory has to be a dependency too, or a
  # parallel build compiles against a file that does not exist yet.  References
  # into the class libraries are covered by the mcs dependency below; references
  # built in another directory are covered by a target-level dependency, which
  # is the only kind that crosses directories reliably.
  set(_refs "")
  set(_ref_deps "")
  foreach(_r IN LISTS ARG_REFS)
    list(APPEND _refs "-r:${_r}")
    if(_r MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/")
      list(APPEND _ref_deps "${_r}")
    endif()
  endforeach()
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
    COMMAND ${MONO_CORPUS_CSC} ${_extra} ${MONO_CORPUS_CSFLAGS}
            "-out:${CMAKE_CURRENT_BINARY_DIR}/${out}" ${_srcs} ${_refs}
    DEPENDS ${_srcs} ${_ref_deps} mcs
    COMMENT "CSC ${out}"
    VERBATIM)
  set(MONO_CORPUS_OUTPUTS
      "${MONO_CORPUS_OUTPUTS};${CMAKE_CURRENT_BINARY_DIR}/${out}" PARENT_SCOPE)
endfunction()

# The same for an IL corpus.
#
#   mono_corpus_il(<out> <source.il> [LIBRARY])
function(mono_corpus_il out source)
  cmake_parse_arguments(ARG "LIBRARY" "" "" ${ARGN})
  set(_extra "")
  if(ARG_LIBRARY)
    set(_extra -dll)
  endif()
  get_filename_component(_outdir "${CMAKE_CURRENT_BINARY_DIR}/${out}" DIRECTORY)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_outdir}"
    COMMAND ${MONO_CORPUS_ILASM} ${_extra}
            "-output=${CMAKE_CURRENT_BINARY_DIR}/${out}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" mcs
    COMMENT "ILASM ${out}"
    VERBATIM)
  set(MONO_CORPUS_OUTPUTS
      "${MONO_CORPUS_OUTPUTS};${CMAKE_CURRENT_BINARY_DIR}/${out}" PARENT_SCOPE)
endfunction()

# Attach this directory's corpora to `mini-corpora`, which is what carries them
# into `all`.  mono/mini owns that target and is processed first, so directories
# added later hang their own target off it.
#
#   mono_corpus_target(<name> [DEPENDS <target>...])
function(mono_corpus_target name)
  cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
  add_custom_target(${name} DEPENDS ${MONO_CORPUS_OUTPUTS})
  if(ARG_DEPENDS)
    add_dependencies(${name} ${ARG_DEPENDS})
  endif()
  add_dependencies(mini-corpora ${name})
endfunction()

# One tiered run of one corpus at one promotion threshold.  Named
# tiered-<group>-<threshold> so a failure names the configuration that broke
# rather than just "tiered".
#
#   mono_add_tiered_test(<group> <corpus.exe> <threshold> [ARGS <arg>...] [ENV <k=v>])
function(mono_add_tiered_test group corpus threshold)
  cmake_parse_arguments(ARG "" "ENV" "ARGS" ${ARGN})
  set(_env "MONO_PATH=${MONO_CORPUS_MONO_PATH}" "MONO_TIERED=1"
           "MONO_TIERED_CALL_THRESHOLD=${threshold}")
  if(ARG_ENV)
    list(APPEND _env "${ARG_ENV}")
  endif()
  add_test(NAME tiered-${group}-${threshold}
           COMMAND "${CMAKE_COMMAND}" -E env ${_env}
                   "${MONO_CORPUS_WRAPPER}" --llvm ${ARG_ARGS} ${corpus}
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  # Threshold 0 is eager: it promotes inline on the mutator and never starts the
  # pool, so it really is a one-core run.
  set(_procs ${MONO_TIERED_PROCESSORS})
  if(threshold EQUAL 0)
    set(_procs 1)
  endif()
  set_tests_properties(tiered-${group}-${threshold} PROPERTIES
    LABELS tiered PROCESSORS ${_procs})
endfunction()

# A check that runs a corpus and asserts on what a pass decided, rather than on
# what the corpus computed.  The script is Python under mono/unit-tests; it takes
# the runtime, the corpus, and for the tag checks the source carrying the
# expectations.
#
#   mono_add_corpus_check(<name> SCRIPT <py> CORPUS <exe> [SOURCE <cs>] [LABELS <l>...])
function(mono_add_corpus_check name)
  cmake_parse_arguments(ARG "" "SCRIPT;CORPUS;SOURCE" "LABELS" ${ARGN})
  set(_source "")
  if(ARG_SOURCE)
    set(_source "${ARG_SOURCE}")
  endif()
  if(NOT ARG_LABELS)
    set(ARG_LABELS tiered)
  endif()
  # The scripts import a module next to them, and a test run has no business
  # leaving a __pycache__ behind in the source tree.
  add_test(NAME ${name}
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${MONO_CORPUS_MONO_PATH}"
                   "PYTHONDONTWRITEBYTECODE=1"
                   "${Python3_EXECUTABLE}" "${ARG_SCRIPT}"
                   "${MONO_CORPUS_WRAPPER}" "${ARG_CORPUS}" ${_source}
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}")
endfunction()
