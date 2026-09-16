# x64-windows against the static CRT, which is what an official LLVM release is
# built with.

include("${CMAKE_CURRENT_LIST_DIR}/x64-windows.cmake")
set(VCPKG_CRT_LINKAGE static)

# A port left a DLL here would carry a CRT of its own, and every string it hands
# back would be freed against the wrong heap.
set(VCPKG_LIBRARY_LINKAGE static)
