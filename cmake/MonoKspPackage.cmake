# Packages this build's runtime and matching class libraries into a zip laid
# out the way a Unity-embedded install expects. It is the same file set
# .claude/scripts/install-to-ksp.sh copies into a local ~/KSP, for a machine
# with no build tree of its own. `cmake --build build --target package-ksp`.
#
# The Linux layout is checked against a real install, ~/KSP. The Windows one
# -- MonoBleedingEdge/EmbedRuntime, mono-2.0-bdwgc.dll -- is Unity's
# documented convention, taken on faith with no install here to check it
# against.

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

set(_ksp_stage    "${CMAKE_BINARY_DIR}/ksp-package")
set(_ksp_zip      "${CMAKE_BINARY_DIR}/mono-llvm-jit-ksp-${_ksp_platform_tag}.zip")
set(_ksp_overrides "${CMAKE_BINARY_DIR}/mono/mini/mono-overrides.dll")
set(_ksp_managed_dir "${CMAKE_BINARY_DIR}/mcs/class/lib/net_4_x-${MONO_MANAGED_PLATFORM}")

# Mono.Cecil is deliberately absent: mods bind against the game's own copy,
# which carries no runtime coupling, so shipping ours only adds a version
# skew.
set(_ksp_assemblies
    mscorlib System System.Core System.Xml System.Configuration System.Security
    Mono.Posix Mono.Security
    I18N I18N.West I18N.CJK I18N.MidEast I18N.Other I18N.Rare)

# Written outside the staging directory: the target's own rm -rf clears that
# on every run, before the copy below puts this back.
set(_ksp_readme "${CMAKE_BINARY_DIR}/ksp-package-readme.txt")
if(_ksp_native_pal)
  file(WRITE "${_ksp_readme}"
"This zip replaces a Unity KSP install's own Mono runtime and class
libraries with a build of Unity Technologies' LLVM-JIT mono fork. Back up
whatever it overwrites first -- there is no undo once the copy lands.

One file it does not touch: <Game>_Data/MonoBleedingEdge/etc/mono/config has
no dllmap for System.Native, which this corlib's Interop.Sys P/Invokes
need (Guid.NewGuid() is the usual first thing that finds that out). Add these
two lines next to the existing MonoPosixHelper <dllmap>:

  <dllmap dll=\"System.Native\" target=\"libmono-native.so\" os=\"!windows\" />
  <dllmap dll=\"System.Net.Security.Native\" target=\"libmono-native.so\" os=\"!windows\" />

If <Game>_Data/boot.config has player-connection-debug=1, the player starts
with sequence points on and both inliners refused. Launch it with
MONO_DEBUG=force-disable-seq-points to measure the shipping runtime instead.
")
else()
  file(WRITE "${_ksp_readme}"
"This zip replaces a Unity KSP install's own Mono runtime and class
libraries with a build of Unity Technologies' LLVM-JIT mono fork. Back up
whatever it overwrites first -- there is no undo once the copy lands.
")
endif()

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

add_custom_target(package-ksp
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_ksp_stage}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ksp_stage}/${_ksp_native_subdir}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ksp_stage}/Managed"
  # libmonobdwgc-2.0 (mono-2.0-bdwgc.dll on Windows) is Unity's fixed name for
  # its embedded runtime, whichever collector backs it. The rename below is
  # the same whether _ksp_runtime_target built under that name already or
  # under monoboehm-2.0.
  COMMAND "${CMAKE_COMMAND}" -E copy
          "$<TARGET_FILE:${_ksp_runtime_target}>" "${_ksp_stage}/${_ksp_native_subdir}/${_ksp_runtime_name}"
  COMMAND "${CMAKE_COMMAND}" -E copy
          "${_ksp_overrides}" "${_ksp_stage}/${_ksp_native_subdir}/mono-overrides.dll"
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
          "${_ksp_native_subdir}" Managed "README-mono-llvm-jit.txt"
  COMMENT "Packaging ${_ksp_zip}"
  VERBATIM)

add_dependencies(package-ksp ${_ksp_runtime_target} mono-overrides mcs-net_4_x)
if(_ksp_native_pal)
  add_dependencies(package-ksp mono-native)
endif()
