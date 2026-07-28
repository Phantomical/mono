# The security command-line tools.
#
# Hand-written because this directory builds fourteen executables from explicit
# source lists rather than one PROGRAM with a .sources file, and because the
# bootstrap profile wants only one of them: sn.exe signs every assembly in
# every later profile, so it is a prerequisite of most of the build.

set(_helper AssemblyInfo.cs "${MONO_MCS_TOPDIR}/build/common/Consts.cs")

# sn is the one the build itself depends on.
mono_declare_managed(
  PROGRAM
  NAME     sn.exe
  PROFILES build net_4_x unityjit
  REFS     Mono.Security System System.Core
  SOURCES  sn.cs StrongNameManager.cs ${_helper}
  NO_INSTALL)

# The rest ship, but nothing in the build uses them, so they are built only
# where they are installed from.
foreach(_p secutil cert2spc makecert chktrust crlupdate signcode setreg
           certmgr caspol permview mozroots cert-sync httpcfg)
  mono_declare_managed(
    PROGRAM
    NAME        ${_p}.exe
    PROFILES    net_4_x
    REFS        Mono.Security System System.Core
    SOURCES     ${_p}.cs ${_helper}
    INSTALL_DIR lib/mono/4.5)
endforeach()
