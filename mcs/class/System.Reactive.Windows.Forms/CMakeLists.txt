# Converted from class/System.Reactive.Windows.Forms/Makefile.

mono_declare_managed(
  NAME       System.Reactive.Windows.Forms.dll
  PROFILES   net_4_x
  KEYFILE    ../reactive.pub
  REFS       System System.Core System.Reactive.Interfaces System.Reactive.Core System.Reactive.Linq System.Windows.Forms
  FLAGS      -d:SIGNED -delaysign -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
)

mono_declare_managed(
  NAME       System.Reactive.Windows.Forms.dll
  PROFILES   unityjit
  KEYFILE    ../reactive.pub
  REFS       System System.Core System.Reactive.Interfaces System.Reactive.Core System.Reactive.Linq System.Windows.Forms
  FLAGS      -d:SIGNED -delaysign -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  NO_INSTALL
)
