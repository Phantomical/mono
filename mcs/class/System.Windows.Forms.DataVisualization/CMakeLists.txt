# Converted from class/System.Windows.Forms.DataVisualization/Makefile.

mono_declare_managed(
  NAME       System.Windows.Forms.DataVisualization.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME dummy-System.Windows.Forms.DataVisualization.dll
  KEYFILE    ../winfx.pub
  REFS       System System.Drawing System.Windows.Forms System.Core System.Data System.Xml
  FLAGS      -nowarn:67
  NO_INSTALL
  NO_SIGN
)
