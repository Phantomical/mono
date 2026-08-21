# libmono-native: the System.Native PAL, built from the corefx submodule plus
# a small mono-specific shim.  It is a stand-alone shared library rather than
# part of libmono because mobile targets need two differently-versioned copies
# of it.  On Linux there is only ever one.

set(_corefx      "${CMAKE_SOURCE_DIR}/external/corefx/src/Native")
set(_corefx_fix  "${CMAKE_SOURCE_DIR}/external/corefx-bugfix/src/Native")

if(NOT EXISTS "${_corefx}/Unix/System.Native/pal_errno.c")
  message(FATAL_ERROR
    "external/corefx is empty -- run `git submodule update --init`, "
    "or configure with -DMONO_ENABLE_MONO_NATIVE=OFF")
endif()

set(_native_sources
  mono-native-platform.c
  pal-icalls.c
  "${_corefx}/Unix/System.Native/pal_errno.c"
  "${_corefx}/Unix/System.Native/pal_maphardwaretype.c"
  "${_corefx}/Unix/System.Native/pal_memory.c"
  "${_corefx}/Unix/System.Native/pal_uid.c"
  "${_corefx}/Unix/System.Native/pal_time.c"
  "${_corefx}/Unix/System.Native/pal_networking.c"
  "${_corefx}/Unix/System.Native/pal_networkstatistics.c"
  "${_corefx}/Unix/System.Native/pal_tcpstate.c"
  "${_corefx}/Unix/System.Native/pal_random.c"
  # corefx-bugfix is a writable overlay on the read-only corefx submodule:
  # files that needed patching are copied into it and take precedence.
  "${_corefx_fix}/Unix/System.Native/pal_io.c"
  # brotli, for System.IO.Compression.Brotli
  "${_corefx_fix}/AnyOS/brotli/enc/backward_references.c"
  "${_corefx_fix}/AnyOS/brotli/enc/backward_references_hq.c"
  "${_corefx_fix}/AnyOS/brotli/enc/bit_cost.c"
  "${_corefx_fix}/AnyOS/brotli/enc/block_splitter.c"
  "${_corefx_fix}/AnyOS/brotli/enc/brotli_bit_stream.c"
  "${_corefx_fix}/AnyOS/brotli/enc/cluster.c"
  "${_corefx_fix}/AnyOS/brotli/enc/command.c"
  "${_corefx_fix}/AnyOS/brotli/enc/compress_fragment.c"
  "${_corefx_fix}/AnyOS/brotli/enc/compress_fragment_two_pass.c"
  "${_corefx_fix}/AnyOS/brotli/enc/dictionary_hash.c"
  "${_corefx_fix}/AnyOS/brotli/enc/encode.c"
  "${_corefx_fix}/AnyOS/brotli/enc/encoder_dict.c"
  "${_corefx_fix}/AnyOS/brotli/enc/entropy_encode.c"
  "${_corefx_fix}/AnyOS/brotli/enc/fast_log.c"
  "${_corefx_fix}/AnyOS/brotli/enc/histogram.c"
  "${_corefx_fix}/AnyOS/brotli/enc/literal_cost.c"
  "${_corefx_fix}/AnyOS/brotli/enc/memory.c"
  "${_corefx_fix}/AnyOS/brotli/enc/metablock.c"
  "${_corefx_fix}/AnyOS/brotli/enc/static_dict.c"
  "${_corefx_fix}/AnyOS/brotli/enc/utf8_util.c"
  "${_corefx_fix}/AnyOS/brotli/dec/bit_reader.c"
  "${_corefx_fix}/AnyOS/brotli/dec/decode.c"
  "${_corefx_fix}/AnyOS/brotli/dec/huffman.c"
  "${_corefx_fix}/AnyOS/brotli/dec/state.c"
  "${_corefx_fix}/AnyOS/brotli/common/constants.c"
  "${_corefx_fix}/AnyOS/brotli/common/context.c"
  "${_corefx_fix}/AnyOS/brotli/common/dictionary.c"
  "${_corefx_fix}/AnyOS/brotli/common/platform.c"
  "${_corefx_fix}/AnyOS/brotli/common/transform.c")

# mono_native_get_platform_type () reports which build this is.  The managed
# tests use it to confirm the right library got loaded.
set(MONO_NATIVE_PLATFORM_TYPE "MONO_NATIVE_PLATFORM_TYPE_LINUX")
configure_file(platform-type.c.in "${CMAKE_CURRENT_BINARY_DIR}/platform-type.c" @ONLY)

add_library(mono-native SHARED
  ${_native_sources}
  "${CMAKE_CURRENT_BINARY_DIR}/platform-type.c")

target_include_directories(mono-native PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/mono"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${_corefx_fix}/Unix/Common"
  "${_corefx}/Unix/Common"
  "${_corefx}/Unix/System.Native"
  "${_corefx_fix}/AnyOS/brotli/include")

target_link_libraries(mono-native PRIVATE
  mono::common mono::warnings mono::hidden mono::eglib_headers)

target_compile_definitions(mono-native PRIVATE
  # brotli's bit-reversal intrinsic assumes a compiler builtin corefx does not
  # want to depend on.
  BROTLI_BUILD_NO_RBIT)

# corefx and mono both typedef a few of the same names.  The duplicate is
# harmless, and clang is the only compiler that complains.
check_c_compiler_flag(-Wno-typedef-redefinition MONO_HAS_WNO_TYPEDEF_REDEFINITION)
if(MONO_HAS_WNO_TYPEDEF_REDEFINITION)
  target_compile_options(mono-native PRIVATE -Wno-typedef-redefinition)
endif()

# libtool's default -version-info 0:0:0.
set_target_properties(mono-native PROPERTIES VERSION 0.0.0 SOVERSION 0)

# The static twin libtool also produced, for embedders that link it in.
add_library(mono-native-static STATIC
  ${_native_sources}
  "${CMAKE_CURRENT_BINARY_DIR}/platform-type.c")
target_include_directories(mono-native-static PRIVATE
  $<TARGET_PROPERTY:mono-native,INCLUDE_DIRECTORIES>)
target_link_libraries(mono-native-static PRIVATE
  mono::common mono::warnings mono::hidden mono::eglib_headers)
target_compile_definitions(mono-native-static PRIVATE BROTLI_BUILD_NO_RBIT)
if(MONO_HAS_WNO_TYPEDEF_REDEFINITION)
  target_compile_options(mono-native-static PRIVATE -Wno-typedef-redefinition)
endif()
set_target_properties(mono-native-static PROPERTIES OUTPUT_NAME mono-native)

install(TARGETS mono-native mono-native-static
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
