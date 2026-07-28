# Converted from class/Facades/System.ServiceProcess.ServiceController/Makefile.

mono_declare_managed(
  NAME       System.ServiceProcess.ServiceController.dll
  PROFILES   net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System System.ServiceProcess
  FLAGS      /delaysign /nowarn:1616,1699
)
