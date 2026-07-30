mono_declare_managed(
  NAME       System.Data.Entity.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Data System.Xml System.Xml.Linq System.Transactions System.Runtime.Serialization System.ComponentModel.DataAnnotations System.Configuration
  FLAGS      -warn:2
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
