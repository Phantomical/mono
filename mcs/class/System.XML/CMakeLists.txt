# Converted from class/System.XML/Makefile.

mono_declare_managed(
  NAME       System.Xml.dll
  PROFILES   build net_4_x unityjit
  REFS       System
  API_BIN_REFS System.Configuration
  FLAGS      -nowarn:219,414,618,649,1717 -unsafe -d:ASYNC -d:CONFIGURATION_DEP
)
