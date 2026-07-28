# Converted from class/System.ComponentModel.Composition.4.5/Makefile.

mono_declare_managed(
  NAME       System.ComponentModel.Composition.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System System.Core
  FLAGS      -d:CLR40 -d:USE_ECMA_KEY,FEATURE_REFLECTIONCONTEXT,FEATURE_REFLECTIONFILEIO,FEATURE_SERIALIZATION,FEATURE_SLIMLOCK -nowarn:219,414 -nowarn:436
)
