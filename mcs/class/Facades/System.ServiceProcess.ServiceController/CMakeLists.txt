# Converted from class/Facades/System.ServiceProcess.ServiceController/Makefile.

mono_declare_managed(
  NAME       System.ServiceProcess.ServiceController.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.ServiceProcess
  FLAGS      /delaysign /nowarn:1616,1699
)
