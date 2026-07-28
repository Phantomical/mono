# Converted from class/System.Web.Extensions.Design/Makefile.

mono_declare_managed(
  NAME       System.Web.Extensions.Design.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  REFS       System System.Design System.Drawing System.Windows.Forms
  FLAGS      -define:NET_3_5
)
