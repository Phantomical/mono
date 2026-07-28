# Converted from class/System.Data/Makefile.

mono_declare_managed(
  NAME       System.Data.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System System.Xml System.Core System.Numerics System.Transactions System.Drawing System.EnterpriseServices System.Configuration Mono.Security
  FLAGS      -nowarn:219,414,649,619,436 -d:COREFX -d:PLATFORM_UNIX -d:USEOFFSET -d:MONO_PARTIAL_DATA_IMPORT -resource:../../../external/corefx/src/System.Data.SqlClient/src/Resources/System.Data.SqlClient.SqlMetaData.xml -unsafe
)
