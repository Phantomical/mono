mono_declare_managed(
  NAME       System.ServiceModel.Internals.dll
  PROFILES   net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Xml
  FLAGS      /unsafe -d:MONO_FEATURE_MULTIPLE_APPDOMAINS
  TEST_FLAGS /unsafe -d:MONO_FEATURE_MULTIPLE_APPDOMAINS
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
