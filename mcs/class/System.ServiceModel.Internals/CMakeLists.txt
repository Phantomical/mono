# Converted from class/System.ServiceModel.Internals/Makefile.

mono_declare_managed(
  NAME       System.ServiceModel.Internals.dll
  PROFILES   net_4_x unityjit
  REFS       System System.Core System.Xml
  FLAGS      /unsafe -d:MONO_FEATURE_MULTIPLE_APPDOMAINS
)
