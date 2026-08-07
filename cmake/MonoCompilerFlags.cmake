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
  # C-only additions.  Two members of configure.ac's set are deliberately not
  # here: -Wstrict-prototypes, which objects to `foo ()` for `foo (void)` in a
  # codebase that spells it that way roughly 3500 times, and -Wc++-compat,
  # which guards a build that compiles these sources as C++ -- something no
  # configuration here does.  Between them they were 90% of the build's warning
  # output, enough to bury the ones worth reading.
  target_compile_options(mono_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>
    $<$<COMPILE_LANGUAGE:C>:-Wnested-externs>
    $<$<COMPILE_LANGUAGE:C>:-Wno-format-zero-length>
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

# --- ccache -----------------------------------------------------------------
# Two separate things keep a second worktree from hitting the entries the first
# one wrote, and both have to go for the cache to be shared.
#
# The first is that ccache hashes the absolute path of the source file and of
# every -I directory.  CCACHE_BASEDIR makes it rewrite the ones under this tree
# into paths relative to the build directory -- both for the hash and for the
# command it eventually runs -- so they read the same from any worktree.  It
# only reaches ccache through the environment, hence wrapping the launcher
# rather than just setting a variable here.
set(_mono_ccache FALSE)
foreach(_lang C CXX ASM)
  if(CMAKE_${_lang}_COMPILER_LAUNCHER MATCHES "ccache")
    set(_mono_ccache TRUE)
    list(PREPEND CMAKE_${_lang}_COMPILER_LAUNCHER
         "${CMAKE_COMMAND}" -E env "CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}")
  endif()
endforeach()

# The second is -g: it puts this tree's build directory in the object as
# DW_AT_comp_dir, and ccache hashes the working directory precisely so that a
# hit can never hand back an object naming some other tree.  Remapping the roots
# we build out of to fixed stand-ins makes the debug info identical everywhere,
# and the hashed directory with it -- ccache applies the same maps to the
# working directory before hashing it.
#
# What that costs is that the debug info no longer names the tree it was built
# in.  gdb started from the build directory still finds the sources, since the
# file names stay relative and $cwd is on its source path; from anywhere else
# it wants
#     set substitute-path /mono ${CMAKE_SOURCE_DIR}
if(_mono_ccache)
  include(CheckCCompilerFlag)
  check_c_compiler_flag("-ffile-prefix-map=/a=/b" MONO_HAVE_FFILE_PREFIX_MAP)
  if(MONO_HAVE_FFILE_PREFIX_MAP)
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/mono")
    # An out-of-tree build directory is not covered by the map above, and
    # CCACHE_BASEDIR does not reach it either, so it needs its own.
    string(FIND "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}/" _mono_bindir_pos)
    if(NOT _mono_bindir_pos EQUAL 0)
      add_compile_options("-ffile-prefix-map=${CMAKE_BINARY_DIR}=/mono-build")
    endif()
  endif()
endif()

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
