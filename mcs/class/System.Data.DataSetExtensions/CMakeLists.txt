# Converted from class/System.Data.DataSetExtensions/Makefile.

mono_declare_managed(
  NAME       System.Data.DataSetExtensions.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System.Core System System.Data System.Xml
  FLAGS      -nowarn:436 -d:COREFX
)
