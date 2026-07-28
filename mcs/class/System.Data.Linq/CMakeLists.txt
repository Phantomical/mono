# Converted from class/System.Data.Linq/Makefile.

mono_declare_managed(
  NAME       System.Data.Linq.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System System.Core System.Xml System.Xml.Linq System.Data System.Runtime.Serialization System.Transactions
  FLAGS      -d:ILGEN
)
