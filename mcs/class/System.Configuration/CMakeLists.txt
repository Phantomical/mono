# Converted from class/System.Configuration/Makefile.

mono_declare_managed(
  NAME       System.Configuration.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System.Security System System.Xml
  FLAGS      -nowarn:618
)
