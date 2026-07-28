# Converted from class/WindowsBase/Makefile.

mono_declare_managed(
  NAME       WindowsBase.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx3.pub
  REFS       System System.Xml
  FLAGS      -unsafe -nowarn:67,618
)
