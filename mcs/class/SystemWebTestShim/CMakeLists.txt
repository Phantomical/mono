# Converted from class/SystemWebTestShim/Makefile.

mono_declare_managed(
  NAME       SystemWebTestShim.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  REFS       System.Web System
  NO_INSTALL
  NO_SIGN
)
