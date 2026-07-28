# Converted from class/System.Web.ApplicationServices/Makefile.

mono_declare_managed(
  NAME       System.Web.ApplicationServices.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME dummy-System.Web.ApplicationServices.dll
  KEYFILE    ../winfx.pub
  REFS       System System.Configuration
  FLAGS      -d:SYSTEM_WEB_APPLICATIONSERVICES
  NO_INSTALL
  NO_SIGN
)
