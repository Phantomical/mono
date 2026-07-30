mono_declare_managed(
  NAME       System.Data.OracleClient.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Data System.EnterpriseServices System.Drawing
  TEST_REFS  System System.Xml System.Data
  TEST_FLAGS /nowarn:618
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
