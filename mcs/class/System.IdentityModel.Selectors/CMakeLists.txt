mono_declare_managed(
  NAME       System.IdentityModel.Selectors.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Security System.Runtime.Serialization System.IdentityModel
  FLAGS      /d:NET_3_0
  TEST_FLAGS /d:NET_3_0
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
