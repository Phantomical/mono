mono_declare_managed(
  NAME       SMDiagnostics.dll
  PROFILES   net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Xml System.ServiceModel.Internals System.Configuration
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
