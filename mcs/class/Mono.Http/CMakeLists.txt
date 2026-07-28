# Converted from class/Mono.Http/Makefile.

mono_declare_managed(
  NAME       Mono.Http.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  REFS       System System.Xml System.Web ICSharpCode.SharpZipLib Mono.Security
  FLAGS      -nowarn:618
)
