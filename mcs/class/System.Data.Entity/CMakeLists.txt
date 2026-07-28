# Converted from class/System.Data.Entity/Makefile.

mono_declare_managed(
  NAME       System.Data.Entity.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System System.Core System.Data System.Xml System.Xml.Linq System.Transactions System.Runtime.Serialization System.ComponentModel.DataAnnotations System.Configuration
  FLAGS      -warn:2
)
