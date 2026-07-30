mono_declare_managed(
  PROGRAM
  NAME       upload-to-sentry.exe
  PROFILES   build
  TARGET_NET_REFERENCE v4.6
  REFS       System System.Core System.Data System.Runtime.Serialization System.Xml System.Xml.Linq Mono.Posix System Mono.Cecil
  INTERMEDIATE
  NO_INSTALL
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
