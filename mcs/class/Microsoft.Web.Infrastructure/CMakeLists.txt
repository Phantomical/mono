mono_declare_managed(
  NAME       Microsoft.Web.Infrastructure.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Configuration System.Web
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
