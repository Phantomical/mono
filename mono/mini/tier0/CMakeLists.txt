option(MONO_ENABLE_TIER0_CLASSIC "Compile mono/mini/tier0/, the restored classic compiler" OFF)

if(NOT MONO_ENABLE_TIER0_CLASSIC)
  return()
endif()

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cpu-amd64.h"
  COMMAND Python3::Interpreter
          "${CMAKE_CURRENT_SOURCE_DIR}/genmdesc.py"
          TARGET_AMD64 "${CMAKE_CURRENT_SOURCE_DIR}"
          "${CMAKE_CURRENT_BINARY_DIR}/cpu-amd64.h"
          amd64_desc
          "${CMAKE_CURRENT_SOURCE_DIR}/arch/amd64/cpu-amd64.md"
  DEPENDS genmdesc.py mini-ops.h arch/amd64/cpu-amd64.md
  COMMENT "Generating tier0 cpu-amd64.h"
  VERBATIM)
add_custom_target(mono-tier0-cpu-desc DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/cpu-amd64.h")

set(MONO_TIER0_SOURCES
  abcremoval.c
  alias-analysis.c
  arch-stubs.c
  arch/amd64/codegen.c
  branch-opts.c
  calls.c
  cfold.c
  debug-mini.c
  decompose.c
  dominators.c
  driver.c
  gc-maps.c
  helpers.c
  intrinsics.c
  linear-scan.c
  liveness.c
  local-propagation.c
  memory-access.c
  method-to-ir.c
  mini-codegen.c
  native-types.c
  profiler-emit.c
  seq-points.c
  ssa.c
  type-checking.c)
target_sources(mini PRIVATE
  ${MONO_TIER0_SOURCES}
  "${CMAKE_CURRENT_BINARY_DIR}/cpu-amd64.h")
target_include_directories(mini PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")
# mini.c calls mono_tier0_init () behind this.
target_compile_definitions(mini PRIVATE MONO_ENABLE_TIER0_CLASSIC)
# simd-intrinsics.c is not in this tree, so tier0's intrinsic call sites have
# nothing to call. DISABLE_SIMD is upstream mono's own knob for that, scoped
# here to tier0/'s sources alone so the rest of mini keeps its intrinsics.
#
# A source-file property scopes down from the directory it is set in, never up.
# mini is add_library()'d in the parent, so without DIRECTORY this reaches
# nothing and the flag goes missing with no diagnostic.
set_source_files_properties(${MONO_TIER0_SOURCES}
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
  PROPERTIES COMPILE_DEFINITIONS DISABLE_SIMD)
add_dependencies(mini mono-tier0-cpu-desc)
