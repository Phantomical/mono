mono_declare_managed(
  NAME       System.Data.Services.Client.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Xml.Linq System.Data System.Xml WindowsBase
  FLAGS      -d:NET_3_5 -warn:2
  RESOURCE_DEFS System.Data.Services.Client,Client/System.Data.Services.Client.txt
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
