mono_declare_managed(
  NAME       WebMatrix.Data.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Data System.Core System.Configuration
  TEST_REFS  System System.Core System.Data Mono.Data.Sqlite Microsoft.CSharp
  TEST_FLAGS -resource:Test/resources/testsqlite.db,Test/resources/testsqlite.db
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
