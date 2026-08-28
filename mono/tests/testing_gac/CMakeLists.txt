# Strong-name and GAC resolution.
#
# Two versions of the same library, each built signed and unsigned, are placed
# in four directories. The tests then vary MONO_PATH and MONO_GAC_PREFIX and
# check which one the loader picks -- and, for the unsigned case, that it
# refuses to pick anything.

set(_gac_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_gac_bin "${CMAKE_CURRENT_BINARY_DIR}")
set(_gac_key "${_gac_src}/testkey.snk")
set(_gacutil "${_buildcls}/gacutil.exe")
set(_sn      "${_buildcls}/sn.exe")

set(_gac_outputs "")

# Signed assemblies are delay-signed at compile time and then re-signed with
# the real key, because csc cannot sign with a key it does not own.
function(_gac_library out source)
  cmake_parse_arguments(ARG "SIGN" "" "" ${ARGN})
  get_filename_component(_d "${_gac_bin}/${out}" DIRECTORY)
  if(ARG_SIGN)
    add_custom_command(
      OUTPUT "${_gac_bin}/${out}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_d}"
      COMMAND ${_csc} -target:library -delaysign "-keyfile:${_gac_key}"
              "-out:${_gac_bin}/${out}" "${_gac_src}/${source}"
      COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_buildcls}" "${_wrapper}"
              "${_sn}" -R "${_gac_bin}/${out}" "${_gac_key}"
      DEPENDS "${_gac_src}/${source}" "${_gac_key}" mono-test-toolchain
      COMMENT "CSC testing_gac/${out} (signed)"
      VERBATIM)
  else()
    add_custom_command(
      OUTPUT "${_gac_bin}/${out}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_d}"
      COMMAND ${_csc} -target:library "-out:${_gac_bin}/${out}" "${_gac_src}/${source}"
      DEPENDS "${_gac_src}/${source}" mono-test-toolchain
      COMMENT "CSC testing_gac/${out}"
      VERBATIM)
  endif()
  set(_gac_outputs "${_gac_outputs};${_gac_bin}/${out}" PARENT_SCOPE)
endfunction()

_gac_library(unsigned_v1/gactestlib.dll v1/gactestlib.cs)
_gac_library(unsigned_v2/gactestlib.dll v2/gactestlib.cs)
_gac_library(signed_v1/gactestlib.dll   v1/gactestlib.cs SIGN)
_gac_library(signed_v2/gactestlib.dll   v2/gactestlib.cs SIGN)

foreach(_app app app-refl-load)
  add_custom_command(
    OUTPUT "${_gac_bin}/${_app}-v1.exe"
    COMMAND ${_csc} -target:exe "-out:${_gac_bin}/${_app}-v1.exe"
            "-r:${_gac_bin}/signed_v1/gactestlib.dll" "${_gac_src}/v1/${_app}.cs"
    DEPENDS "${_gac_src}/v1/${_app}.cs" "${_gac_bin}/signed_v1/gactestlib.dll"
            mono-test-toolchain
    COMMENT "CSC testing_gac/${_app}-v1.exe"
    VERBATIM)
  list(APPEND _gac_outputs "${_gac_bin}/${_app}-v1.exe")
endforeach()

# app-both references both versions under aliases, so it can ask for either.
add_custom_command(
  OUTPUT "${_gac_bin}/app-both.exe"
  COMMAND ${_csc} -target:exe "-out:${_gac_bin}/app-both.exe"
          "-r:V1=${_gac_bin}/signed_v1/gactestlib.dll"
          "-r:V2=${_gac_bin}/signed_v2/gactestlib.dll"
          "${_gac_src}/app-both.cs"
  DEPENDS "${_gac_src}/app-both.cs" "${_gac_bin}/signed_v1/gactestlib.dll"
          "${_gac_bin}/signed_v2/gactestlib.dll" mono-test-toolchain
  COMMENT "CSC testing_gac/app-both.exe"
  VERBATIM)
list(APPEND _gac_outputs "${_gac_bin}/app-both.exe")

# Install the signed v1 into a GAC rooted in the build directory, for the
# MONO_GAC_PREFIX test.
add_custom_command(
  OUTPUT "${_gac_bin}/lib/mono/gac/.stamp"
  COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_buildcls}" "${_wrapper}"
          "${_gacutil}" -i "${_gac_bin}/signed_v1/gactestlib.dll" -gacdir "${_gac_bin}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_gac_bin}/lib/mono/gac/.stamp"
  DEPENDS "${_gac_bin}/signed_v1/gactestlib.dll" mono-test-toolchain
  COMMENT "GACUTIL testing_gac/signed_v1/gactestlib.dll"
  VERBATIM)
list(APPEND _gac_outputs "${_gac_bin}/lib/mono/gac/.stamp")

add_custom_target(testing-gac-tests DEPENDS ${_gac_outputs})
add_dependencies(runtime-corpora testing-gac-tests)

# Each case is the same programs under a different MONO_PATH, so the env var is
# the test. It overrides the default MONO_PATH mono_runtime_suite sets, which is
# why it goes through ENV rather than anywhere else.
function(_gac_test name)
  cmake_parse_arguments(ARG "" "EXPECT;GAC_PREFIX" "PATH;PROGRAMS" ${ARGN})
  set(_env "")
  set(_mp "")
  foreach(_p IN LISTS ARG_PATH)
    list(APPEND _mp "${_gac_bin}/${_p}")
  endforeach()
  if(_mp)
    mono_path_join(_gac_path ${_mp} "${_class}")
    list(APPEND _env "MONO_PATH=${_gac_path}")
  endif()
  if(ARG_GAC_PREFIX)
    list(APPEND _env "MONO_GAC_PREFIX=${ARG_GAC_PREFIX}")
  endif()
  # These test strict strong-name resolution, so the loader is put in strict mode.
  mono_runtime_suite(testing_gac_${name} TESTS ${ARG_PROGRAMS}
                     WORKDIR "${_gac_bin}" TIMEOUT 600
                     RUNTIME_ARGS "--assembly-loader=strict"
                     EXPECT "${ARG_EXPECT}" ENV ${_env})
endfunction()

set(_app_v1 app-v1.exe app-refl-load-v1.exe)

# An unsigned library must not satisfy a strong-named reference.
_gac_test(mp-unsigned-v1        PATH unsigned_v1 PROGRAMS ${_app_v1} EXPECT 1)
_gac_test(mp-signed-v1          PATH signed_v1   PROGRAMS ${_app_v1})
_gac_test(mp-unsigned-v2-signed-v1 PATH unsigned_v2 signed_v1 PROGRAMS ${_app_v1})
_gac_test(mp-signed-v2-signed-v1   PATH signed_v2 signed_v1   PROGRAMS ${_app_v1})
# Path order matters: v1 must load first, and the v2 lookup then has to fail
# to find v2 inside the v1 directory.
_gac_test(app-both PATH signed_v1 signed_v2 PROGRAMS app-both.exe)
_gac_test(gac PROGRAMS ${_app_v1} GAC_PREFIX "${_gac_bin}")
