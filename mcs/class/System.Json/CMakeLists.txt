mono_declare_managed(
  NAME       System.Json.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Core
  FLAGS      -nowarn:3021
  TEST_FLAGS -nowarn:3021
  XTEST_REFS System System.Core System.Xml Facades/System.Threading.Tasks
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
