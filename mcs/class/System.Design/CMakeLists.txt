# Converted from class/System.Design/Makefile.

mono_declare_managed(
  NAME       System.Design.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System System.Xml System.Windows.Forms System.Drawing Accessibility System.Data System.Configuration
  API_BIN_REFS System.Web
  FLAGS      -nowarn:436 -nowarn:612,618,649,67,672
)
