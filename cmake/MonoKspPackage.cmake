# Packages this build's runtime and matching class libraries into a zip laid
# out the way a Unity-embedded install expects. It is the same file set
# .claude/scripts/install-to-ksp.sh copies into a local ~/KSP, for a machine
# with no build tree of its own. `cmake --build build --target package-ksp`.
#
# Both layouts are checked against a real install: Linux against ~/KSP, Windows
# against "Kerbal Space Program - Test 5". etc/mono/config sits beside the
# per-arch directory on both, rather than under it.
#
# On Windows the zip unpacks over the install root with nothing left to move:
# the managed assemblies carry KSP_x64_Data/Managed with them, and z.dll lands
# at the root beside KSP_x64.exe.

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
  set(_ksp_managed_subdir  "KSP_x64_Data/Managed")
else()
  set(_ksp_platform_tag    "linux")
  set(_ksp_native_subdir   "MonoBleedingEdge/x86_64")
  set(_ksp_runtime_name    "libmonobdwgc-2.0.so")
  set(_ksp_native_pal      "${MONO_ENABLE_MONO_NATIVE}")
  # Which <Game>_Data a Linux install carries is unchecked, so the assemblies
  # stay at the root of the zip and the README says where to put them. Name
  # the directory here once one can be read, and this zip unpacks in place
  # like the Windows one.
  set(_ksp_managed_subdir  "Managed")
endif()

if(NOT MONO_HOST_WINDOWS)
  # MonoLLVM.cmake bakes the build machine's own LLVM_LIBRARY_DIRS into every
  # runtime's rpath, alongside the $ORIGIN that finds a bundled copy
  # (mono/mini/CMakeLists.txt). Harmless during development -- it is what
  # lets a locally built mono-boehm run without LD_LIBRARY_PATH -- but a path
  # that exists on no machine but this one has no business in a package
  # meant for another one, so it comes back off here.
  find_program(MONO_PATCHELF patchelf)
  if(NOT MONO_PATCHELF)
    message(WARNING
      "patchelf not found; package-ksp will leave the runtime's build-machine "
      "rpath in place instead of stripping it down to \$ORIGIN")
  endif()
endif()

set(_ksp_stage    "${CMAKE_BINARY_DIR}/ksp-package")
set(_ksp_zip      "${CMAKE_BINARY_DIR}/mono-llvm-jit-ksp-${_ksp_platform_tag}.zip")
set(_ksp_overrides "${CMAKE_BINARY_DIR}/mono/mini/mono-overrides.dll")
set(_ksp_overrides_pdb "${CMAKE_BINARY_DIR}/mono/mini/mono-overrides.pdb")
# unityjit rather than net_4_x: an icall's native half is compiled into the
# runtime beside these and its managed half into mscorlib, so both have to come
# from the profile the runtime is built as.
set(_ksp_managed_dir "${CMAKE_BINARY_DIR}/mcs/class/lib/unityjit-${MONO_MANAGED_PLATFORM}")
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
if(MONO_HOST_WINDOWS)
  set(_ksp_readme_install
"Unpack it over the KSP install root -- the directory holding KSP_x64.exe --
and every file lands where the game already looks for it.

z.dll sits at that root rather than beside the runtime that imports it,
because Windows resolves an import against the application directory. Moved
into MonoBleedingEdge\\EmbedRuntime it stops being found, and Unity reports
only \"Unable to load mono library\".")
else()
  set(_ksp_readme_install
"Unpack MonoBleedingEdge/ over the KSP install root, and copy the contents of
Managed/ over <Game>_Data/Managed -- which of those this install carries is
not something the packaging step could check, so the assemblies are left at
the root of the zip for you to place.")
endif()
file(WRITE "${_ksp_readme}"
"This zip replaces a Unity KSP install's own Mono runtime and class
libraries with a build of Unity Technologies' LLVM-JIT mono fork. Back up
whatever it overwrites first -- there is no undo once the copy lands.

${_ksp_readme_install}

The runtime and the assemblies beside it are one build and do not come apart:
an icall's native half ships in the runtime and its managed half in
mscorlib.dll, both from this tree. Keeping Unity's own mscorlib.dll while
swapping only the runtime is what made CultureInfo's static constructor throw.

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
               "${_ksp_managed_dir}/${_asm}.dll" "${_ksp_stage}/${_ksp_managed_subdir}/${_asm}.dll")
endforeach()
if(_ksp_native_pal)
  list(APPEND _ksp_copy_commands
       COMMAND "${CMAKE_COMMAND}" -E copy
               "$<TARGET_FILE:mono-native>" "${_ksp_stage}/${_ksp_native_subdir}/libmono-native.so")
endif()
# MONO_UNITY_BUILD links LLVM as a static archive, absorbed into the runtime
# below with nothing left to copy. Off that switch -- the only option on
# Windows, where it is forced off (MonoOptions.cmake) -- LLVM is a shared
# library the runtime loads dynamically, found through its own $ORIGIN
# runpath (mono/mini/CMakeLists.txt), so it has to ride along too.
foreach(_llvm_lib IN LISTS MONO_LLVM_SHARED_LIBRARY_TARGETS)
  list(APPEND _ksp_copy_commands
       COMMAND "${CMAKE_COMMAND}" -E copy
               "$<TARGET_FILE:${_llvm_lib}>" "${_ksp_stage}/${_ksp_native_subdir}/$<TARGET_FILE_NAME:${_llvm_lib}>")
endforeach()
if(MONO_PATCHELF)
  list(APPEND _ksp_copy_commands
       COMMAND "${MONO_PATCHELF}" --set-rpath "\$ORIGIN"
               "${_ksp_stage}/${_ksp_native_subdir}/${_ksp_runtime_name}")
endif()
if(MONO_HOST_WINDOWS)
  # Unity ships a MonoPosixHelper of its own. Leaving it there pairs this
  # tree's System.dll with a stranger's helper, across the P/Invoke surface
  # DeflateStream and Mono.Posix both reach it through.
  list(APPEND _ksp_copy_commands
       COMMAND "${CMAKE_COMMAND}" -E copy
               "$<TARGET_FILE:MonoPosixHelper>" "${_ksp_stage}/${_ksp_native_subdir}/MonoPosixHelper.dll"
       COMMAND "${CMAKE_COMMAND}" -E copy
               "$<TARGET_PDB_FILE:MonoPosixHelper>" "${_ksp_stage}/${_ksp_native_subdir}/$<TARGET_PDB_FILE_NAME:MonoPosixHelper>")

  # Windows resolves an import against the application directory, not against
  # the directory of the DLL naming it, so z.dll goes to the root beside
  # KSP_x64.exe. Moved into EmbedRuntime it stops being found, and Unity
  # reports only "Unable to load mono library".
  if(ZLIB_FOUND)
    list(APPEND _ksp_copy_commands
         COMMAND "${CMAKE_COMMAND}" -E copy
                 "$<TARGET_FILE_DIR:${_ksp_runtime_target}>/z.dll" "${_ksp_stage}/z.dll")
  endif()

  # MSVC links every target with /DEBUG (cmake/MonoCompilerFlags.cmake), so
  # both the runtime and mono-overrides always have a PDB to bring along.
  # Kept under its own build-time name rather than renamed to match the dll.
  # A debugger locates a pdb by the name recorded in the dll's debug
  # directory, not by the dll's own file name.
  list(APPEND _ksp_copy_commands
       COMMAND "${CMAKE_COMMAND}" -E copy
               "$<TARGET_PDB_FILE:${_ksp_runtime_target}>" "${_ksp_stage}/${_ksp_native_subdir}/$<TARGET_PDB_FILE_NAME:${_ksp_runtime_target}>")
endif()

add_custom_target(package-ksp
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_ksp_stage}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ksp_stage}/${_ksp_native_subdir}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ksp_stage}/${_ksp_config_subdir}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ksp_stage}/${_ksp_managed_subdir}"
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
  #
  # `.` rather than a list of top-level names. libarchive writes the entries
  # with no `./` prefix, so the zip is the staging directory's contents.
  COMMAND "${CMAKE_COMMAND}" -E chdir "${_ksp_stage}"
          "${CMAKE_COMMAND}" -E tar cf "${_ksp_zip}" --format=zip -- .
  COMMENT "Packaging ${_ksp_zip}"
  VERBATIM)

add_dependencies(package-ksp ${_ksp_runtime_target} mono-overrides mcs-unityjit)
if(MONO_HOST_WINDOWS)
  add_dependencies(package-ksp MonoPosixHelper)
endif()
if(_ksp_native_pal)
  add_dependencies(package-ksp mono-native)
endif()
