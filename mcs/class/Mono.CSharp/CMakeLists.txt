# Sets MCS_GENERATED_SOURCES and MCS_GENERATED_TARGETS.  Hand-written:
# it holds the jay or tool invocation the Makefile carried as a recipe.
include(${CMAKE_CURRENT_SOURCE_DIR}/generated-sources.cmake)

mono_declare_managed(
  NAME       Mono.CSharp.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System.Xml System
  FLAGS      -d:MONO_FEATURE_THREAD_ABORT -d:MONO_FEATURE_PROCESS_START
  TEST_REFS  System.Core
  TEST_FLAGS -d:MONO_FEATURE_PROCESS_START
  BUILT_SOURCES ${MCS_GENERATED_SOURCES}
  DEPENDS       ${MCS_GENERATED_TARGETS}
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
