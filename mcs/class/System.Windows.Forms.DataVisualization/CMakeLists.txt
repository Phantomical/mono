mono_declare_managed(
  NAME       System.Windows.Forms.DataVisualization.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Drawing System.Windows.Forms System.Core System.Data System.Xml
  FLAGS      -nowarn:67
  TEST_FLAGS -nowarn:67
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
