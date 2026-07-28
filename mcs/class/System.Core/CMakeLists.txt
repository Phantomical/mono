# Converted from class/System.Core/Makefile.

mono_declare_managed(
  NAME       System.Core.dll
  PROFILES   build net_4_x unityjit
  REFS       System
  FLAGS      -d:FEATURE_PAL,PFX_LEGACY_3_5,FEATURE_NETCORE -d:INSIDE_SYSCORE -d:LIBC -unsafe -nowarn:436 -d:NET_3_5 -d:FEATURE_COMPILE,FEATURE_COMPILE_TO_METHODBUILDER
)
