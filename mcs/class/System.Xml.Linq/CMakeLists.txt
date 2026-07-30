mono_declare_managed(
  NAME       System.Xml.Linq.dll
  PROFILES   build net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Xml
  FLAGS      -unsafe -nowarn:436
  TEST_REFS  System.Core System.Xml System
  XTEST_REFS System.Core System.Xml System Facades/System.Reflection Facades/System.Threading.Tasks
  XTEST_FLAGS -unsafe
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
