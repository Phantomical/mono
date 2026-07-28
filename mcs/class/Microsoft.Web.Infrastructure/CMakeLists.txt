# Converted from class/Microsoft.Web.Infrastructure/Makefile.

mono_declare_managed(
  NAME       Microsoft.Web.Infrastructure.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME dummy-Microsoft.Web.Infrastructure.dll
  KEYFILE    ../winfx.pub
  REFS       System System.Configuration System.Web
  NO_INSTALL
  NO_SIGN
)
