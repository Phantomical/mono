# Two generated sources that the class libraries compile against.
#
# Both land in the source tree, at the gitignored paths they have always used.
# That is not laziness about the build tree: 146 checked-in .sources files name
# `../../build/common/Consts.cs` by relative path, and gensources resolves
# those against the library's own directory.  Moving the file would mean
# editing every one of them.
#
# Nothing here depends on the build directory, so two build trees over one
# checkout write identical bytes and cannot race meaningfully.

set(MONO_VERSION "${PROJECT_VERSION}.0")
set(MONO_CORLIB_VERSION "${MONO_CORLIB_VERSION_GUID}")
configure_file(common/Consts.cs.in
               "${CMAKE_CURRENT_SOURCE_DIR}/common/Consts.cs" @ONLY)

# sourcelink maps the paths baked into a portable PDB back to the commits they
# came from, so a debugger can fetch the exact sources.  The hashes are read
# once at configure time; they only affect the URLs in the map.
set(mono_topdir "${CMAKE_SOURCE_DIR}")
function(_mono_commit out dir)
  execute_process(COMMAND git -C "${dir}" rev-parse HEAD
                  OUTPUT_VARIABLE _h ERROR_QUIET RESULT_VARIABLE _rc
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _rc EQUAL 0)
    set(_h "unknown")
  endif()
  set(${out} "${_h}" PARENT_SCOPE)
endfunction()
_mono_commit(mono_commit_hash   "${CMAKE_SOURCE_DIR}")
_mono_commit(corefx_commit_hash "${CMAKE_SOURCE_DIR}/external/corefx")
_mono_commit(corert_commit_hash "${CMAKE_SOURCE_DIR}/external/corert")
_mono_commit(cecil_commit_hash  "${CMAKE_SOURCE_DIR}/external/cecil")
configure_file(common/sourcelink.json.in
               "${CMAKE_CURRENT_SOURCE_DIR}/common/sourcelink.json" @ONLY)

# A copy of Consts.cs also lands in each profile's output directory.  Nothing
# in the build reads it; it is there so a monolite package carries the corlib
# version stamp it was built with.
foreach(_p IN LISTS MONO_MANAGED_PROFILES)
  mono_profile_dir(_dir ${_p})
  configure_file(common/Consts.cs.in "${_dir}/Consts.cs" @ONLY)
endforeach()
