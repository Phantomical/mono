# Sets MCS_GENERATED_SOURCES and MCS_GENERATED_TARGETS.  Hand-written:
# it holds the jay or tool invocation the Makefile carried as a recipe.
include(${CMAKE_CURRENT_SOURCE_DIR}/generated-sources.cmake)

mono_declare_managed(
  PROGRAM
  NAME       mcs.exe
  PROFILES   net_4_x
  REFS       System.Core System.Xml System
  FLAGS      -d:STATIC,NO_SYMBOL_WRITER,NO_AUTHENTICODE -d:MONO_FEATURE_THREAD_ABORT -d:MONO_FEATURE_PROCESS_START
  BUILT_SOURCES ${MCS_GENERATED_SOURCES}
  DEPENDS       ${MCS_GENERATED_TARGETS}
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
