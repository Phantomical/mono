# pkg-config files, the dllmap configuration, and the debugger helper scripts.

set(API_VER          "${MONO_API_VERSION}")
set(VERSION          "${PROJECT_VERSION}")
set(reloc_libdir     "${MONO_RELOC_LIBDIR}")
set(sysconfdir       "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
set(export_ldflags   "-Wl,--export-dynamic")
set(libmono_ldflags  "")
set(libmono_cflags   "")

set(_pc_files
  mono.pc mono-2.pc dotnet.pc dotnet35.pc wcf.pc mono-cairo.pc mono-options.pc
  cecil.pc monodoc.pc mono-lineeditor.pc system.web.extensions_1.0.pc
  system.web.extensions.design_1.0.pc system.web.mvc.pc system.web.mvc2.pc
  system.web.mvc3.pc aspnetwebstack.pc reactive.pc)
if(MONO_ENABLE_SGEN)
  list(APPEND _pc_files monosgen-2.pc)
endif()

foreach(_pc IN LISTS _pc_files)
  configure_file("${_pc}.in" "${CMAKE_CURRENT_BINARY_DIR}/${_pc}" @ONLY)
  list(APPEND _pc_generated "${CMAKE_CURRENT_BINARY_DIR}/${_pc}")
endforeach()

install(FILES ${_pc_generated} xbuild12.pc
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

# data/config -- the dllmap table that turns the Windows library names the
# class libraries P/Invoke into the sonames present on this system.

set(libsuffix ".so")
set(LIBC      "libc.so.6")
set(INTL      "libc.so.6")
set(SQLITE    "libsqlite.so.0")
set(SQLITE3   "libsqlite3.so.0")
set(X11       "libX11.so.6")
set(XINERAMA  "libXinerama.so.1")
set(GDKX11    "libgdk-x11-2.0.so.0")
set(GTKX11    "libgtk-x11-2.0.so.0")
set(ODBC      "libodbc.so.2")
set(MONO_NATIVE_LIBRARY_NAME "libmono-native")
set(libgdiplus_install_loc "libgdiplus.so.0")

configure_file(config.in "${CMAKE_CURRENT_BINARY_DIR}/config" @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/config" browscap.ini
        DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/mono")

# gdb/lldb pretty printers.
install(FILES gdb/mono-gdb.py
        DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES gdb/mono-gdb.py
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME mono-sgen-gdb.py)
install(FILES lldb/mono.py
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/mono/lldb")

# The machine.config / web.config tree, one directory per framework version.
# net_1_1 is not installed -- only the build tree gets a 1.0 directory, for
# tests that still ask for it.
foreach(_v 2.0 4.0 4.5)
  string(REPLACE "." "_" _dir "net_${_v}")
  install(FILES
      "${_dir}/machine.config"
      "${_dir}/web.config"
      "${_dir}/DefaultWsdlHelpGenerator.aspx"
      "${_dir}/settings.map"
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/mono/${_v}")
  # All three versions share one browser-capability file.
  install(FILES Browsers/Compat.browser
          DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/mono/${_v}/Browsers")
endforeach()
