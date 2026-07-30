mono_declare_managed(
  NAME       System.Runtime.DurableInstancing.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Core System.Xml.Linq System.Transactions System.ServiceModel.Internals System.Runtime.Serialization
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
