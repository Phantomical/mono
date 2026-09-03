# The flag sets configure.ac assembled into CPPFLAGS/CFLAGS/CXXFLAGS/LDFLAGS.
#
# They are INTERFACE targets rather than additions to CMAKE_C_FLAGS, so that
# a target takes only the sets that apply to it.  mono::hidden stays out of
# mono::common because eglib is built without it.

include(CheckCCompilerFlag)

# CMake seeds the MSVC flags with `/DWIN32 /D_WINDOWS`.  _WINDOWS says the image
# is a GUI subsystem one, and mono/mini/main.c reads it to pick wWinMain over
# main; the mono binaries are console programs.  WIN32 stays, since it is what
# the runtime's own sources test.
if(MSVC)
  foreach(_lang C CXX)
    string(REPLACE "/D_WINDOWS" "" CMAKE_${_lang}_FLAGS "${CMAKE_${_lang}_FLAGS}")
  endforeach()
endif()

# --- mono::warnings ---------------------------------------------------------
# --enable-compile-warnings: applied to C and C++ alike.
add_library(mono_warnings INTERFACE)
add_library(mono::warnings ALIAS mono_warnings)
if(MONO_ENABLE_COMPILE_WARNINGS AND MSVC)
  # MSVC's warning policy is a set of #pragma warning directives in
  # msvc/msvc-disabled-warnings.h, which config.h includes, so it reaches every
  # translation unit without a flag.  Keeping it there rather than restating it
  # here is what lets the msvc/ project files and this build agree.  /W3 is the
  # level those pragmas were written against.
  target_compile_options(mono_warnings INTERFACE $<$<COMPILE_LANGUAGE:C,CXX>:/W3>)
elseif(MONO_ENABLE_COMPILE_WARNINGS)
  target_compile_options(mono_warnings INTERFACE
    -Wall -Wunused -Wmissing-declarations -Wpointer-arith
    -Wno-cast-qual -Wwrite-strings -Wno-switch-enum
    -Wno-unused-value -Wno-attributes)

  # Clang spellings, so each one has to be asked for.
  foreach(_w string-concatenation null-pointer-subtraction gnu-null-pointer-arithmetic)
    string(MAKE_C_IDENTIFIER "MONO_HAS_W_${_w}" _mono_w_var)
    check_c_compiler_flag("-W${_w}" ${_mono_w_var})
    if(${_mono_w_var})
      target_compile_options(mono_warnings INTERFACE "-W${_w}")
    endif()
  endforeach()
  # C-only additions.
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

if(MONO_HOST_WINDOWS)
  # The identity macros the runtime and Boehm read to tell which host they are
  # on.  HOST_WIN32 and TARGET_WIN32 are not here: config.h carries those, and
  # a second home for them is a second answer.
  #
  # __x86_64__ is a compiler macro everywhere else, and mono's own headers test
  # it directly in places MSVC would otherwise fall off the end of.
  # _WINDOWS is deliberately not here: it says the image is a GUI subsystem
  # one, and mono/mini/main.c reads it to pick wWinMain over main.  The mono
  # binaries are console programs.
  target_compile_definitions(mono_common INTERFACE
    HAVE_CONFIG_H
    WIN32 _WIN32 __WIN32__ WIN64 _WIN64
    __x86_64__
    GC_WIN32_THREADS
    _CRT_SECURE_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
    # The runtime's Windows code passes wide strings to the Win32 entry points
    # it names undecorated -- GetModuleHandleEx with an L"" literal, and its
    # neighbours.  Without this those names resolve to the ANSI half and the
    # call reads the string as bytes.  msvc/mono.props is where the msvc build
    # says the same.
    UNICODE _UNICODE
    # select(3) is what the socket layer waits on, and 64 descriptors is what
    # winsock2.h defaults to.
    FD_SETSIZE=1024
    # mono/utils/valgrind.h has no MSVC arm -- its client requests are written
    # as inline assembly gcc and clang accept.  This is the header's own switch
    # for a host it cannot instrument, and it compiles every request to a
    # constant.
    NVALGRIND
    # winsock.h and winsock2.h declare the same symbols differently, and
    # anything that pulls in windows.h first gets the older one.
    WIN32_LEAN_AND_MEAN
    # windows.h otherwise defines min and max as macros, and LLVM's headers
    # are full of std::numeric_limits<T>::max ().
    NOMINMAX)

  # ml64 takes none of these, and answers an option it does not know by
  # dropping a character and trying the rest again, so one stray flag becomes a
  # dozen warnings and an object it may not write.  Hence the language guard on
  # every one.
  target_compile_options(mono_common INTERFACE
    # Source is UTF-8 and the runtime has string literals that are not ASCII;
    # without this MSVC reads them in the machine's ANSI code page.
    $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>
    # mono/interp/transform.c and the metadata tables pass 65535 sections.
    $<$<COMPILE_LANGUAGE:C,CXX>:/bigobj>
    # -std=gnu11 elsewhere.  MSVC's default C dialect predates designated
    # initializers, which the icall tables and the interpreter's opcode
    # descriptions are written with.
    $<$<COMPILE_LANGUAGE:C>:/std:c11>
    # The runtime's macros are written against a conforming preprocessor.
    # MSVC's traditional one rescans a parenthesised argument differently, and
    # mono/metadata/icall-def.h -- where an icall's parameter list is one such
    # argument -- comes apart under it.
    $<$<COMPILE_LANGUAGE:C,CXX>:/Zc:preprocessor>)
else()
  target_compile_definitions(mono_common INTERFACE
    HAVE_CONFIG_H
    _GNU_SOURCE _REENTRANT
    GC_LINUX_THREADS USE_MMAP USE_MUNMAP USE_COMPILER_TLS)

  target_compile_options(mono_common INTERFACE
    # gnu11 rather than configure.ac's gnu99. The C-facing headers repeat a
    # forward typedef instead of including the whole of metadata, and C99
    # forbids the repeat.
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
    $<$<COMPILE_LANGUAGE:C>:-fwrapv>
    # The runtime takes the address of TLS variables across shared-library
    # boundaries. The segment-relative shortcut breaks that under Xen.
    $<$<COMPILE_LANGUAGE:C>:-mno-tls-direct-seg-refs>)
endif()

# MONO_DLL_EXPORT turns the MONO_API attributes into "export" rather than
# "import" -- correct for everything built into the runtime itself.
target_compile_definitions(mono_common INTERFACE
  $<$<COMPILE_LANGUAGE:C>:MONO_DLL_EXPORT>)

target_link_libraries(mono_common INTERFACE mono::warnings)

# --- mono::hidden -----------------------------------------------------------
# $(SHARED_CFLAGS).  Not global: eglib is deliberately built without it.
add_library(mono_hidden INTERFACE)
add_library(mono::hidden ALIAS mono_hidden)
# A PE image exports nothing it has not marked, so on Windows the default is
# already what this asks for and the option has nothing to add.
if(MONO_ENABLE_VISIBILITY_HIDDEN AND NOT MSVC)
  target_compile_options(mono_hidden INTERFACE -fvisibility=hidden)
endif()

# --- mono::eglib ------------------------------------------------------------
# $(GLIB_CFLAGS) for the embedded eglib: the include paths, so that
# consumers can pick it up without linking the library.
add_library(mono_eglib_headers INTERFACE)
add_library(mono::eglib_headers ALIAS mono_eglib_headers)
target_include_directories(mono_eglib_headers INTERFACE
  "${CMAKE_SOURCE_DIR}/mono/eglib"
  "${CMAKE_BINARY_DIR}/mono/eglib")

# --- mono::platform ---------------------------------------------------------
# The system libraries the runtime needs from its host.  Everything that links
# a runtime -- libmono, the mono binaries, the helper libraries -- names this
# instead of a library, so a host that spells one of them differently is one
# change here.
add_library(mono_platform INTERFACE)
add_library(mono::platform ALIAS mono_platform)
target_link_libraries(mono_platform INTERFACE Threads::Threads)
if(MONO_HOST_WINDOWS)
  # ws2_32/mswsock are the sockets, iphlpapi the interface enumeration,
  # psapi/dbghelp what the crash reporter and the stack walks read, bcrypt the
  # RNG, version the file-version metadata an assembly carries, and winmm the
  # millisecond timer.  The rest are what OLE and the registry need.
  target_link_libraries(mono_platform INTERFACE
    ws2_32 mswsock iphlpapi psapi dbghelp bcrypt version winmm
    advapi32 ole32 oleaut32 shell32 user32 userenv secur32 crypt32)
else()
  # libm and libdl. Threads::Threads above carries -pthread.
  target_link_libraries(mono_platform INTERFACE m ${CMAKE_DL_LIBS})
endif()

# --- ccache -----------------------------------------------------------------
# Two separate things keep a second worktree from hitting the entries the first
# one wrote, and both have to go for the cache to be shared.
#
# The first is that ccache hashes the absolute path of the source file and of
# every -I directory.  CCACHE_BASEDIR makes it rewrite the ones under this tree
# into paths relative to the build directory -- both for the hash and for the
# command it eventually runs -- so they read the same from any worktree.  It
# only reaches ccache through the environment, hence wrapping the launcher
# rather than setting a variable here.
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
# file names stay relative and $cwd is on its source path. From anywhere else
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
if(MSVC)
  # mono/interp/mintops.def describes each opcode with a designated
  # initializer, and the tables it builds are C++.  gcc and clang take those in
  # C++17 as an extension; MSVC takes them only in C++20.
  set(CMAKE_CXX_STANDARD 20)
else()
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  if(MSVC)
    # /O1 /Ob1 is CMake's own MinSizeRel, and /Zi puts the debug info in a PDB
    # beside the object rather than in it.
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
    set(CMAKE_C_FLAGS_MINSIZEREL   "/O1 /Ob1 /Zi /DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_MINSIZEREL "/O1 /Ob1 /Zi /DNDEBUG" CACHE STRING "" FORCE)
  else()
    # Matches the tree's usual CFLAGS="-fPIC -Os" plus -g.
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
    set(CMAKE_C_FLAGS_MINSIZEREL   "-Os -g" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -g" CACHE STRING "" FORCE)
  endif()
endif()

if(MSVC)
  # /DEBUG so the PDBs the compile wrote reach the image, and /OPT:REF,ICF
  # back, which naming /DEBUG otherwise turns off.
  add_link_options(/DEBUG /OPT:REF /OPT:ICF)
else()
  # --export-dynamic so managed code can P/Invoke back into the runtime's own
  # symbols. -Bsymbolic and -z now so the runtime binds its internal references
  # to itself instead of to whatever came earlier in the search order.
  add_link_options(-Wl,--export-dynamic -Wl,-Bsymbolic -Wl,-z,now)
endif()
