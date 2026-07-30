mono_declare_managed(
  NAME       System.Data.Linq.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Xml System.Xml.Linq System.Data System.Runtime.Serialization System.Transactions
  FLAGS      -d:ILGEN
  TEST_REFS  System System.Core System.Data System.Xml
  TEST_FLAGS -d:MONO_STRICT
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
