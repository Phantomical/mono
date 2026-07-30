mono_declare_managed(
  PROGRAM
  NAME       aprofutil.exe
  PROFILES   net_4_x
  REFS       System System.Core Mono.Profiler.Log
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
