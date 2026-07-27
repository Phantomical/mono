# The flag sets configure.ac assembled into CPPFLAGS/CFLAGS/CXXFLAGS/LDFLAGS.
#
# They are exposed as INTERFACE targets rather than appended to
# CMAKE_C_FLAGS so that a directory can opt out of one of them: eglib and
# mono/native both replace the warning set wholesale, exactly as they did
# under automake.

# --- mono::warnings ---------------------------------------------------------
# --enable-compile-warnings: applied to C and C++ alike.
add_library(mono_warnings INTERFACE)
add_library(mono::warnings ALIAS mono_warnings)
if(MONO_ENABLE_COMPILE_WARNINGS)
  target_compile_options(mono_warnings INTERFACE
    -Wall -Wunused -Wmissing-declarations -Wpointer-arith
    -Wno-cast-qual -Wwrite-strings -Wno-switch -Wno-switch-enum
    -Wno-unused-value -Wno-attributes)
  # C-only additions.  -Wc++-compat keeps the C sources compilable by a C++
  # compiler, which the --enable-cxx build relies on.
  target_compile_options(mono_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>
    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
    $<$<COMPILE_LANGUAGE:C>:-Wnested-externs>
    $<$<COMPILE_LANGUAGE:C>:-Wno-format-zero-length>
    $<$<COMPILE_LANGUAGE:C>:-Wc++-compat>
    $<$<COMPILE_LANGUAGE:C>:-Wno-unused-but-set-variable>)
endif()

# --- mono::common -----------------------------------------------------------
# What every runtime translation unit gets: the config.h include path, the
# Boehm/mmap feature defines configure folded into CPPFLAGS, and the C dialect.
add_library(mono_common INTERFACE)
add_library(mono::common ALIAS mono_common)

target_include_directories(mono_common INTERFACE
  "${CMAKE_BINARY_DIR}"          # config.h
  "${CMAKE_SOURCE_DIR}")

target_compile_definitions(mono_common INTERFACE
  HAVE_CONFIG_H
  _GNU_SOURCE _REENTRANT
  GC_LINUX_THREADS USE_MMAP USE_MUNMAP USE_COMPILER_TLS)

target_compile_options(mono_common INTERFACE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu99>
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  # The runtime takes the address of TLS variables across shared-library
  # boundaries; the segment-relative shortcut breaks that under Xen.
  $<$<COMPILE_LANGUAGE:C>:-mno-tls-direct-seg-refs>)

# MONO_DLL_EXPORT turns the MONO_API attributes into "export" rather than
# "import" -- correct for everything built into the runtime itself.
target_compile_definitions(mono_common INTERFACE
  $<$<COMPILE_LANGUAGE:C>:MONO_DLL_EXPORT>)

target_link_libraries(mono_common INTERFACE mono::warnings)

# --- mono::hidden -----------------------------------------------------------
# $(SHARED_CFLAGS).  Not global: eglib is deliberately built without it.
add_library(mono_hidden INTERFACE)
add_library(mono::hidden ALIAS mono_hidden)
if(MONO_ENABLE_VISIBILITY_HIDDEN)
  target_compile_options(mono_hidden INTERFACE -fvisibility=hidden)
endif()

# --- mono::eglib ------------------------------------------------------------
# $(GLIB_CFLAGS) for the embedded eglib: just the include paths, so that
# consumers can pick it up without linking the library.
add_library(mono_eglib_headers INTERFACE)
add_library(mono::eglib_headers ALIAS mono_eglib_headers)
target_include_directories(mono_eglib_headers INTERFACE
  "${CMAKE_SOURCE_DIR}/mono/eglib"
  "${CMAKE_BINARY_DIR}/mono/eglib")

# --- global defaults --------------------------------------------------------
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # Matches the tree's usual CFLAGS="-fPIC -Os" plus -g.
  set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
  set(CMAKE_C_FLAGS_MINSIZEREL   "-Os -g" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -g" CACHE STRING "" FORCE)
endif()

# --export-dynamic so managed code can P/Invoke back into the runtime's own
# symbols; -Bsymbolic and -z now so the runtime binds its internal references
# to itself instead of to whatever came earlier in the search order.
add_link_options(-Wl,--export-dynamic -Wl,-Bsymbolic -Wl,-z,now)
