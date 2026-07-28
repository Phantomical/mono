# Converted from class/Facades/System.Buffers/Makefile.

mono_declare_managed(
  NAME       System.Buffers.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../Open.snk
  SNK        ../../Open.snk
  INSTALL_DIR lib/mono/4.5/Facades
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
