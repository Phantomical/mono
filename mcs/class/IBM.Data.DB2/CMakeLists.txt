# Converted from class/IBM.Data.DB2/Makefile.

mono_declare_managed(
  NAME       IBM.Data.DB2.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ibm.pub
  REFS       System System.Xml System.Data
  FLAGS      /unsafe -warn:1
)
