# The /usr/bin wrappers.  Each is a two-line shell script that runs the
# corresponding managed tool out of the installed class library tree.

set(bindir         "${CMAKE_INSTALL_FULL_BINDIR}")
set(plat_bindir    "${CMAKE_INSTALL_FULL_BINDIR}")
set(mono_instdir   "${CMAKE_INSTALL_PREFIX}/lib/mono")
set(monodocdir     "${CMAKE_INSTALL_FULL_DATADIR}/monodoc")
set(prefix         "${CMAKE_INSTALL_PREFIX}")
set(exec_prefix    "${CMAKE_INSTALL_PREFIX}")
set(gtkdir         "")
set(mono_interp    "mono")
set(framework_version "4.5")

set(_out "${CMAKE_CURRENT_BINARY_DIR}")

# Tools that run on the default (4.x) framework.
set(_scripts_default
  al ilasm gacutil mkbundle monop resgen wsdl)

# The "2" -suffixed aliases kept for Makefiles written against Mono 2.x.
set(_scripts_2_0
  al2 gacutil2 monop2 resgen2 wsdl2)

set(_scripts_4_0
  aprofutil disco dtd2xsd dtd2rng installvst genxs macpack mdoc
  mono-cil-strip soapsuds caspol cert-sync cert2spc certmgr chktrust
  crlupdate csharp httpcfg illinkanalyzer lc mconfig monolinker
  mono-api-info mono-api-html mono-shlib-cop mozroots permview pdb2mdb
  xsd mono-xmltool setreg secutil signcode sgen sqlmetal sqlsharp svcutil
  ccrewrite cccheck mdbrebase ikdasm mono-symbolicate)

# makecert and sn write key material, so they run with a tight umask.
set(_scripts_umask makecert sn)

foreach(_s IN LISTS _scripts_default _scripts_2_0)
  # The trailing digit is an alias, not part of the assembly name.
  string(REGEX REPLACE "2$" "" _exe "${_s}")
  set(exe_name "${_exe}")
  mono_configure_script("${CMAKE_CURRENT_SOURCE_DIR}/script.in" "${_out}/${_s}")
  list(APPEND MONO_BIN_SCRIPTS "${_out}/${_s}")
endforeach()

if(MONO_ENABLE_MCS_BUILD)
  foreach(_s IN LISTS _scripts_4_0)
    set(exe_name "${_s}")
    mono_configure_script("${CMAKE_CURRENT_SOURCE_DIR}/script.in" "${_out}/${_s}")
    list(APPEND MONO_BIN_SCRIPTS "${_out}/${_s}")
  endforeach()
  foreach(_s dmcs xbuild)
    mono_configure_script("${CMAKE_CURRENT_SOURCE_DIR}/${_s}.in" "${_out}/${_s}")
    list(APPEND MONO_BIN_SCRIPTS "${_out}/${_s}")
  endforeach()
endif()

foreach(_s IN LISTS _scripts_umask)
  set(exe_name "${_s}")
  mono_configure_script("${CMAKE_CURRENT_SOURCE_DIR}/script_umask.in" "${_out}/${_s}")
  list(APPEND MONO_BIN_SCRIPTS "${_out}/${_s}")
endforeach()

# Scripts with a template of their own.
foreach(_s peverify mcs csc vbc csi mono-configuration-crypto)
  set(exe_name "${_s}")
  mono_configure_script("${CMAKE_CURRENT_SOURCE_DIR}/${_s}.in" "${_out}/${_s}")
  list(APPEND MONO_BIN_SCRIPTS "${_out}/${_s}")
endforeach()

foreach(_s mono-service mono-service2)
  set(exe_name "${_s}")
  mono_configure_script("${CMAKE_CURRENT_SOURCE_DIR}/mono-service.in" "${_out}/${_s}")
  list(APPEND MONO_BIN_SCRIPTS "${_out}/${_s}")
endforeach()

set(exe_name "mod")
mono_configure_script("${CMAKE_CURRENT_SOURCE_DIR}/mod.in" "${_out}/mod")
list(APPEND MONO_BIN_SCRIPTS "${_out}/mod")

foreach(_s mono-find-provides mono-find-requires)
  mono_configure_script("${CMAKE_CURRENT_SOURCE_DIR}/${_s}.in" "${_out}/${_s}")
  list(APPEND MONO_BIN_SCRIPTS "${_out}/${_s}")
endforeach()

# mdoc's subcommands are one-liners that re-enter mdoc.
set(_mdoc_subcommands assemble export-html export-msxdoc update validate)
foreach(_c IN LISTS _mdoc_subcommands)
  file(WRITE "${_out}/mdoc-${_c}" "#!/bin/sh\nexec mdoc ${_c} \"$@\"\n")
  file(CHMOD "${_out}/mdoc-${_c}" PERMISSIONS
       OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
       WORLD_READ WORLD_EXECUTE)
  list(APPEND MONO_BIN_SCRIPTS "${_out}/mdoc-${_c}")
endforeach()

install(PROGRAMS ${MONO_BIN_SCRIPTS}
        DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Checked-in scripts that need no substitution, including the pre-mdoc
# monodoc command names still referenced by older documentation builds.
install(PROGRAMS
    mono-package-runtime
    mono-test-install
    mono-heapviz
    mdassembler
    mdvalidater
    monodocer
    monodocs2html
    monodocs2slashdoc
  DESTINATION "${CMAKE_INSTALL_BINDIR}")
