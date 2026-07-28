# Converted from class/System/Makefile.

mono_declare_managed(
  NAME       System.dll
  PROFILES   build net_4_x unityjit
  REFS       MonoSecurity=Mono.Security
  API_BIN_REFS System.Net.Http System.Xml System.Core System.Numerics System.Configuration
  FLAGS      -d:COREFX -d:CONFIGURATION_2_0 -d:SYSTEM_NET_PRIMITIVES_DLL -d:XML_DEP -d:SECURITY_DEP -d:FEATURE_PAL,SYSTEM_NAMESPACE,MONO,PLATFORM_UNIX -d:MONO_FEATURE_PROCESS_START -d:MONO_FEATURE_THREAD_ABORT -d:MONO_FEATURE_THREAD_SUSPEND_RESUME -d:MONO_FEATURE_MULTIPLE_APPDOMAINS -unsafe -resource:resources/Asterisk.wav -resource:resources/Beep.wav -resource:resources/Exclamation.wav -resource:resources/Hand.wav -resource:resources/Question.wav -nowarn:436 -d:MONO_SECURITY_ALIAS -d:CODEDOM -d:FEATURE_COMPILED -d:ENABLE_GSS -d:CONFIGURATION_DEP
)
