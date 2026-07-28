# Converted from class/Facades/System.Memory/Makefile.

mono_declare_managed(
  NAME       System.Memory.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../Open.snk
  SNK        ../../Open.snk
  INSTALL_DIR lib/mono/4.5/Facades
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
