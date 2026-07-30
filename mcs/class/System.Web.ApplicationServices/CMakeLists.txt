mono_declare_managed(
  NAME       System.Web.ApplicationServices.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Configuration
  FLAGS      -d:SYSTEM_WEB_APPLICATIONSERVICES
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
