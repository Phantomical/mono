mono_declare_managed(
  NAME       System.Web.Extensions.Design.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Design System.Drawing System.Windows.Forms
  FLAGS      -define:NET_3_5
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
