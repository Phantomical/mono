# vcpkg's own x64-windows, repeated verbatim, plus one override.
#
# zlib links statically so that nothing this build ships imports z.dll. A
# z.dll beside the runtime in MonoBleedingEdge\EmbedRuntime is never found --
# Windows resolves an import against the application directory, not against
# the directory of the DLL naming it -- so Unity reports "Unable to load mono
# library". The KSP root is the only place it would load from, and that is a
# shared directory a stock install has no z.dll in.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_PROVIDED_FORTRAN ON)

if(PORT STREQUAL "zlib")
  set(VCPKG_LIBRARY_LINKAGE static)
endif()
