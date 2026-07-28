# Converted from class/System.Drawing/Makefile.

mono_declare_managed(
  NAME       System.Drawing.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System
  FLAGS      /unsafe -resource:Assembly/Mono.ico,Mono.ico -resource:Assembly/Information.ico,Information.ico -resource:Assembly/Error.ico,Error.ico -resource:Assembly/Warning.ico,Warning.ico -resource:Assembly/Question.ico,Question.ico -resource:Assembly/Shield.ico,Shield.ico -d:FEATURE_TYPECONVERTER,SUPPORTS_WINDOWS_COLORS
)
