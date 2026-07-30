mono_declare_managed(
  NAME       System.IO.Compression.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System
  FLAGS      -unsafe
  TEST_REFS  System System.Core
  TEST_FLAGS -resource:Test/resources/archive.zip,Test/resources/archive.zip -resource:Test/resources/test.nupkg,Test/resources/test.nupkg
  XTEST_REFS System System.Core Facades/System.Threading.Tasks
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
