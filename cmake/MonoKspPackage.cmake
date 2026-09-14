# Packages this build's runtime and matching class libraries into a zip laid
# out the way a Unity-embedded install expects. It is the same file set
# .claude/scripts/install-to-ksp.sh copies into a local ~/KSP, for a machine
# with no build tree of its own. `cmake --build build --target package-ksp`.
#
# The Linux layout is checked against a real install, ~/KSP. The Windows one
# -- MonoBleedingEdge/EmbedRuntime, mono-2.0-bdwgc.dll -- is Unity's
# documented convention, taken on faith with no install here to check it
# against. etc/mono/config is assumed to sit beside them at the same level on
# both platforms, rather than under the per-arch directory, for the same
# reason.

if(NOT MONO_ENABLE_MCS_BUILD OR NOT MONO_ENABLE_LIBRARIES)
  return()
endif()

if(MONO_UNITY_SGEN_AS_BDWGC AND MONO_ENABLE_SGEN)
  set(_ksp_runtime_target monobdwgc-2.0)
elseif(MONO_ENABLE_BOEHM)
  set(_ksp_runtime_target monoboehm-2.0)
else()
  message(WARNING
    "package-ksp needs MONO_UNITY_SGEN_AS_BDWGC=ON (with MONO_ENABLE_SGEN) or "
    "MONO_ENABLE_BOEHM=ON; neither is satisfied, so the target is not defined.")
  return()
endif()

if(MONO_HOST_WINDOWS)
  set(_ksp_platform_tag    "windows")
  set(_ksp_native_subdir   "MonoBleedingEdge/EmbedRuntime")
  set(_ksp_runtime_name    "mono-2.0-bdwgc.dll")
  set(_ksp_native_pal      OFF)
else()
  set(_ksp_platform_tag    "linux")
  set(_ksp_native_subdir   "MonoBleedingEdge/x86_64")
  set(_ksp_runtime_name    "libmonobdwgc-2.0.so")
  set(_ksp_native_pal      "${MONO_ENABLE_MONO_NATIVE}")
endif()
string(REPLACE ".dll" ".pdb" _ksp_runtime_pdb_name "${_ksp_runtime_name}")

set(_ksp_stage    "${CMAKE_BINARY_DIR}/ksp-package")
set(_ksp_zip      "${CMAKE_BINARY_DIR}/mono-llvm-jit-ksp-${_ksp_platform_tag}.zip")
set(_ksp_overrides "${CMAKE_BINARY_DIR}/mono/mini/mono-overrides.dll")
set(_ksp_overrides_pdb "${CMAKE_BINARY_DIR}/mono/mini/mono-overrides.pdb")
set(_ksp_managed_dir "${CMAKE_BINARY_DIR}/mcs/class/lib/net_4_x-${MONO_MANAGED_PLATFORM}")
set(_ksp_config_subdir "MonoBleedingEdge/etc/mono")

# Mono.Cecil is deliberately absent: mods bind against the game's own copy,
# which carries no runtime coupling, so shipping ours only adds a version
# skew.
set(_ksp_assemblies
    mscorlib System System.Core System.Xml System.Configuration System.Security
    Mono.Posix Mono.Security
    I18N I18N.West I18N.CJK I18N.MidEast I18N.Other I18N.Rare)

# Written outside the staging directory: the target's own rm -rf clears that
# on every run, before the build-time copies below put these back.
set(_ksp_readme "${CMAKE_BINARY_DIR}/ksp-package-readme.txt")
file(WRITE "${_ksp_readme}"
"This zip replaces a Unity KSP install's own Mono runtime and class
libraries with a build of Unity Technologies' LLVM-JIT mono fork. Back up
whatever it overwrites first -- there is no undo once the copy lands.

If <Game>_Data/boot.config has player-connection-debug=1, the player starts
with sequence points on and both inliners refused. Launch it with
MONO_DEBUG=force-disable-seq-points to measure the shipping runtime instead.
")

# Unity's own MonoBleedingEdge/etc/mono/config, ground-truthed from a KSP
# install's pristine backup (config.orig, made before install-to-ksp.sh's own
# patch ever touched it). Embedded rather than patched at install time like
# that script does, because a zip has no existing install to patch.
set(_ksp_config_base [=[<configuration>
	<dllmap dll="i:cygwin1.dll" target="libc.so.6" os="!windows" />
	<dllmap dll="libc" target="libc.so.6" os="!windows"/>
	<dllmap dll="intl" target="libc.so.6" os="!windows"/>
	<dllmap dll="intl" name="bind_textdomain_codeset" target="libc.so.6" os="solaris"/>
	<dllmap dll="libintl" name="bind_textdomain_codeset" target="libc.so.6" os="solaris"/>
	<dllmap dll="libintl" target="libc.so.6" os="!windows"/>
	<dllmap dll="i:libxslt.dll" target="libxslt.so" os="!windows"/>
	<dllmap dll="i:odbc32.dll" target="libodbc.so" os="!windows"/>
	<dllmap dll="i:odbc32.dll" target="libiodbc.dylib" os="osx"/>
	<dllmap dll="oci" target="libclntsh.so" os="!windows"/>
	<dllmap dll="db2cli" target="libdb2_36.so" os="!windows"/>
	<dllmap dll="MonoPosixHelper" target="libMonoPosixHelper.so" os="!windows" />
	<dllmap dll="libmono-btls-shared" target="libmono-btls-shared.so" os="!windows" />
	<dllmap dll="i:msvcrt" target="libc.so.6" os="!windows"/>
	<dllmap dll="i:msvcrt.dll" target="libc.so.6" os="!windows"/>
	<dllmap dll="sqlite" target="libsqlite.so.0" os="!windows"/>
	<dllmap dll="sqlite3" target="libsqlite3.so.0" os="!windows"/>
	<dllmap dll="libX11" target="libX11.so.6" os="!windows" />
	<dllmap dll="libgdk-x11-2.0" target="libgdk-x11-2.0.so.0" os="!windows"/>
	<dllmap dll="libgdk_pixbuf-2.0" target="libgdk_pixbuf-2.0.so.0" os="!windows"/>
	<dllmap dll="libgtk-x11-2.0" target="libgtk-x11-2.0.so.0" os="!windows"/>
	<dllmap dll="libglib-2.0" target="libglib-2.0.so.0" os="!windows"/>
	<dllmap dll="libgobject-2.0" target="libgobject-2.0.so.0" os="!windows"/>
	<dllmap dll="libgnomeui-2" target="libgnomeui-2.so.0" os="!windows"/>
	<dllmap dll="librsvg-2" target="librsvg-2.so.2" os="!windows"/>
	<dllmap dll="libXinerama" target="libXinerama.so.1" os="!windows" />
	<dllmap dll="libasound" target="libasound.so.2" os="!windows" />
	<dllmap dll="libcairo-2.dll" target="libcairo.so.2" os="!windows"/>
	<dllmap dll="libcairo-2.dll" target="libcairo.2.dylib" os="osx"/>
	<dllmap dll="libcups" target="libcups.so.2" os="!windows"/>
	<dllmap dll="libcups" target="libcups.dylib" os="osx"/>
	<dllmap dll="i:kernel32.dll">
		<dllentry dll="__Internal" name="CopyMemory" target="mono_win32_compat_CopyMemory"/>
		<dllentry dll="__Internal" name="FillMemory" target="mono_win32_compat_FillMemory"/>
		<dllentry dll="__Internal" name="MoveMemory" target="mono_win32_compat_MoveMemory"/>
		<dllentry dll="__Internal" name="ZeroMemory" target="mono_win32_compat_ZeroMemory"/>
	</dllmap>
	<dllmap dll="gdiplus" target="libgdiplus.so.0" os="!windows"/>
	<dllmap dll="gdiplus.dll" target="libgdiplus.so.0"  os="!windows"/>
	<dllmap dll="gdi32" target="libgdiplus.so.0" os="!windows"/>
	<dllmap dll="gdi32.dll" target="libgdiplus.so.0" os="!windows"/>
</configuration>
]=])
if(_ksp_native_pal)
  # The same two lines install-to-ksp.sh's sed adds, so corlib's Interop.Sys
  # P/Invokes reach libmono-native without a manual edit.
  string(REPLACE
    "<dllmap dll=\"MonoPosixHelper\" target=\"libMonoPosixHelper.so\" os=\"!windows\" />"
    "<dllmap dll=\"MonoPosixHelper\" target=\"libMonoPosixHelper.so\" os=\"!windows\" />
	<dllmap dll=\"System.Native\" target=\"libmono-native.so\" os=\"!windows\" />
	<dllmap dll=\"System.Net.Security.Native\" target=\"libmono-native.so\" os=\"!windows\" />"
    _ksp_config_base "${_ksp_config_base}")
endif()
set(_ksp_config "${CMAKE_BINARY_DIR}/ksp-package-config.xml")
file(WRITE "${_ksp_config}" "${_ksp_config_base}")

set(_ksp_copy_commands "")
foreach(_asm IN LISTS _ksp_assemblies)
  list(APPEND _ksp_copy_commands
       COMMAND "${CMAKE_COMMAND}" -E copy
               "${_ksp_managed_dir}/${_asm}.dll" "${_ksp_stage}/Managed/${_asm}.dll")
endforeach()
if(_ksp_native_pal)
  list(APPEND _ksp_copy_commands
       COMMAND "${CMAKE_COMMAND}" -E copy
               "$<TARGET_FILE:mono-native>" "${_ksp_stage}/${_ksp_native_subdir}/libmono-native.so")
endif()
if(MONO_HOST_WINDOWS)
  # MSVC links every target with /DEBUG (cmake/MonoCompilerFlags.cmake), so
  # both the runtime and mono-overrides always have a PDB to bring along.
  list(APPEND _ksp_copy_commands
       COMMAND "${CMAKE_COMMAND}" -E copy
               "$<TARGET_PDB_FILE:${_ksp_runtime_target}>" "${_ksp_stage}/${_ksp_native_subdir}/${_ksp_runtime_pdb_name}")
endif()

add_custom_target(package-ksp
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_ksp_stage}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ksp_stage}/${_ksp_native_subdir}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ksp_stage}/${_ksp_config_subdir}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ksp_stage}/Managed"
  # libmonobdwgc-2.0 (mono-2.0-bdwgc.dll on Windows) is Unity's fixed name for
  # its embedded runtime, whichever collector backs it. The rename below is
  # the same whether _ksp_runtime_target built under that name already or
  # under monoboehm-2.0.
  COMMAND "${CMAKE_COMMAND}" -E copy
          "$<TARGET_FILE:${_ksp_runtime_target}>" "${_ksp_stage}/${_ksp_native_subdir}/${_ksp_runtime_name}"
  COMMAND "${CMAKE_COMMAND}" -E copy
          "${_ksp_overrides}" "${_ksp_stage}/${_ksp_native_subdir}/mono-overrides.dll"
  # -debug:portable (mono/mini/CMakeLists.txt) writes this beside the dll
  # unconditionally, on both platforms.
  COMMAND "${CMAKE_COMMAND}" -E copy
          "${_ksp_overrides_pdb}" "${_ksp_stage}/${_ksp_native_subdir}/mono-overrides.pdb"
  COMMAND "${CMAKE_COMMAND}" -E copy
          "${_ksp_config}" "${_ksp_stage}/${_ksp_config_subdir}/config"
  COMMAND "${CMAKE_COMMAND}" -E copy
          "${_ksp_readme}" "${_ksp_stage}/README-mono-llvm-jit.txt"
  ${_ksp_copy_commands}
  # WORKING_DIRECTORY on the whole target would cd once, before the rm -rf
  # above deletes that same directory. The shell's cwd then points at an
  # unlinked inode, and every relative path after that, this one included,
  # resolves nowhere. -E chdir instead spawns tar fresh, after the rm -rf and
  # the copies above it are done.
  COMMAND "${CMAKE_COMMAND}" -E chdir "${_ksp_stage}"
          "${CMAKE_COMMAND}" -E tar cf "${_ksp_zip}" --format=zip --
          "MonoBleedingEdge" Managed "README-mono-llvm-jit.txt"
  COMMENT "Packaging ${_ksp_zip}"
  VERBATIM)

add_dependencies(package-ksp ${_ksp_runtime_target} mono-overrides mcs-net_4_x)
if(_ksp_native_pal)
  add_dependencies(package-ksp mono-native)
endif()
