# The security command-line tools.
#
# Hand-written for two reasons. The directory builds fourteen executables from
# explicit source lists rather than one PROGRAM with a .sources file. The
# bootstrap profile also wants only one of them. sn.exe signs every assembly in
# every later profile, so it is a prerequisite of most of the build.

set(_helper AssemblyInfo.cs "${MONO_MCS_TOPDIR}/build/common/Consts.cs")

# sn is the one the build itself depends on.
mono_declare_managed(
  PROGRAM
  NAME     sn.exe
  PROFILES build net_4_x
  REFS     Mono.Security System System.Core
  SOURCES  sn.cs StrongNameManager.cs ${_helper}
  NO_INSTALL)

# The rest ship, but nothing in the build uses them, so they are built only
# where they are installed from.
# permview has its own rule in the makefile: it reads assembly metadata, so it
# takes Mono.Cecil and none of the references the pattern rule hands the rest.
mono_declare_managed(
  PROGRAM
  NAME        permview.exe
  PROFILES    net_4_x
  REFS        Mono.Cecil
  SOURCES     permview.cs ${_helper}
  INSTALL_DIR <libdir>/mono/4.5)

foreach(_p secutil cert2spc makecert chktrust crlupdate signcode setreg
           certmgr caspol mozroots cert-sync httpcfg)
  mono_declare_managed(
    PROGRAM
    NAME        ${_p}.exe
    PROFILES    net_4_x
    REFS        Mono.Security System System.Core
    SOURCES     ${_p}.cs ${_helper}
    INSTALL_DIR <libdir>/mono/4.5)
endforeach()
