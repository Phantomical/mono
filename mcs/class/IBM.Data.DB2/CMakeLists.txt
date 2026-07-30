mono_declare_managed(
  NAME       IBM.Data.DB2.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ibm.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Data
  FLAGS      /unsafe -warn:1
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
