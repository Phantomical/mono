mono_declare_managed(
  NAME       Mono.Profiler.Log.dll
  PROFILES   net_4_x
  KEYFILE    <topdir>/class/mono.snk
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core
  FLAGS      /unsafe /publicsign /nowarn:0618 -D:MONO_DATACONVERTER_STATIC_METHOD
  XTEST_REFS System System.Core Facades/System.Threading.Tasks
)

mono_declare_managed(
  NAME       Mono.Profiler.Log.dll
  PROFILES   unityjit
  KEYFILE    <topdir>/class/mono.snk
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core
  FLAGS      /unsafe /publicsign /nowarn:0618 -D:MONO_DATACONVERTER_STATIC_METHOD
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
