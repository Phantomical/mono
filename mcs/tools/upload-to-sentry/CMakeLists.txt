# Converted from tools/upload-to-sentry/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       upload-to-sentry.exe
  PROFILES   build
  TARGET_NET_REFERENCE v4.6
  REFS       System System.Core System.Data System.Runtime.Serialization System.Xml System.Xml.Linq Mono.Posix System Mono.Cecil
  INTERMEDIATE
  NO_INSTALL
)
