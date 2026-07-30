mono_declare_managed(
  NAME       System.Data.Services.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.ServiceModel System.ServiceModel.Web System.Data.Services.Client System.ServiceModel.Activation
  FLAGS      -d:NET_3_5
  TEST_REFS  System.ServiceModel System.Core
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
