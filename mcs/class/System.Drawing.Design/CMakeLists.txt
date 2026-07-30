mono_declare_managed(
  NAME       System.Drawing.Design.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Drawing System.Windows.Forms
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
