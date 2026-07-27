# libmono-btls-shared: the BoringSSL-backed TLS provider.
#
# mono/btls is already a self-contained CMake project (the automake build shelled
# out to cmake for it), and it pulls external/boringssl in as a subdirectory with
# its own compiler flags and its own idea of the warning set.  Keeping it as a
# sub-build rather than folding it into this project preserves that isolation --
# in particular it keeps mono's -Werror-adjacent warning flags and link options
# off BoringSSL's sources.

include(ExternalProject)

# BoringSSL's own build wants the architecture spelled its way, to pick the
# right assembly.
set(MONO_BTLS_ARCH "x86_64")
set(MONO_BTLS_ROOT "${CMAKE_SOURCE_DIR}/external/boringssl")
if(NOT EXISTS "${MONO_BTLS_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR
    "external/boringssl is empty -- run `git submodule update --init`, "
    "or configure with -DMONO_ENABLE_BTLS=OFF")
endif()

set(MONO_BTLS_BINARY_DIR "${CMAKE_BINARY_DIR}/mono/btls")
set(MONO_BTLS_LIBRARY
    "${MONO_BTLS_BINARY_DIR}/libmono-btls-shared${CMAKE_SHARED_LIBRARY_SUFFIX}")

ExternalProject_Add(mono-btls
  SOURCE_DIR      "${CMAKE_SOURCE_DIR}/mono/btls"
  BINARY_DIR      "${MONO_BTLS_BINARY_DIR}"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
    -DBUILD_SHARED_LIBS=1
    -DBTLS_ROOT:PATH=${MONO_BTLS_ROOT}
    -DSRC_DIR:PATH=${CMAKE_SOURCE_DIR}/mono/btls
    -DBTLS_ARCH:STRING=${MONO_BTLS_ARCH}
  BUILD_BYPRODUCTS "${MONO_BTLS_LIBRARY}"
  INSTALL_COMMAND ""
  USES_TERMINAL_BUILD TRUE)

install(FILES "${MONO_BTLS_LIBRARY}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}")
