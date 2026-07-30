mono_declare_managed(
  NAME       System.Design.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Windows.Forms System.Drawing Accessibility System.Data System.Configuration
  API_BIN_REFS System.Web
  FLAGS      -nowarn:436 -nowarn:612,618,649,67,672
  TEST_REFS  System System.Drawing System.Windows.Forms
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
