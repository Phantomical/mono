# The config.h entries that are not probes: package identity, the target
# description, the DISABLE_*/ENABLE_* feature switches, and the handful of
# macros configure.ac set unconditionally.

# --- package identity -------------------------------------------------------
set(PACKAGE           "\"mono\"")
set(PACKAGE_NAME      "\"mono\"")
set(PACKAGE_TARNAME   "\"mono\"")
set(PACKAGE_VERSION   "\"${PROJECT_VERSION}\"")
set(PACKAGE_STRING    "\"mono ${PROJECT_VERSION}\"")
set(PACKAGE_BUGREPORT "\"https://github.com/mono/mono/issues/new\"")
set(PACKAGE_URL       "\"\"")
set(VERSION           "\"${PROJECT_VERSION}\"")
# libtool's object directory.  Nothing in a CMake build puts objects there, but
# the macro is part of the public config.h surface, so keep the value stable.
set(LT_OBJDIR         "\".libs/\"")
set(MONO_CORLIB_VERSION "\"${MONO_CORLIB_VERSION_GUID}\"")

set(STDC_HEADERS 1)

# --- host / target ----------------------------------------------------------
# This port is amd64-only.  The other architectures configure.ac knew about
# are not reachable here, so we fail loudly rather than produce a config.h
# that claims a target we do not generate code for.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "Unsupported target ${CMAKE_SYSTEM_PROCESSOR}; this build supports amd64 only")
endif()

set(HOST_AMD64   1)
set(TARGET_AMD64 1)
set(MONO_ARCHITECTURE "\"amd64\"")

# NOTE: configure.ac guarded the HOST_LINUX define with
#   if test echo x$target_os | grep -q linux; then
# which is broken shell.  `test` takes `echo` and `x$target_os` as its own two
# operands instead of running echo, so grep always sees empty input and the
# guard is always false.  HOST_LINUX was never defined in a released build,
# even though the automake conditional of the same name was true.  Keeping
# the C macro undefined preserves the code paths the runtime has actually
# been built and tested with.  MONO_HOST_LINUX, which MonoOptions.cmake sets,
# carries the correct value for the build system's own use: it is a CMake
# variable, and config.h gets nothing from it.

# Classic (non-UWP) Windows API surface: defined on every target.  The runtime
# reads the pair through mono/utils/w32subset.h, which is compiled everywhere,
# so a Unix host answers the question too.
set(HAVE_CLASSIC_WINAPI_SUPPORT 1)
set(HAVE_UWP_WINAPI_SUPPORT     0)

set(MONO_INSIDE_RUNTIME 1)

if(MONO_HOST_WINDOWS)
  set(HOST_WIN32   1)
  set(TARGET_WIN32 1)

  # NTFS has junctions and symlinks, but the runtime reaches them through the
  # POSIX calls in mono/metadata/w32file-unix.c, which this host does not
  # compile.  The IO portability layer is the same story: it rewrites a path's
  # case and separators for a case-sensitive filesystem, and Windows needs
  # neither.
  set(HOST_NO_SYMLINKS    1)
  set(DISABLE_PORTABILITY 1)

  # MSVC rejects `char x[0]` in a struct and takes `char x[1]` as the
  # variable-length member instead, so a trailing array is declared one
  # element long and every size computation subtracts it.
  set(MONO_ZERO_LEN_ARRAY 1)

  set(MONO_KEYWORD_THREAD "__declspec (thread)")

  # The CRT spells it strtok_s, and mono/eglib/eglib-config.hw maps the POSIX
  # name onto it.  Saying so here is what keeps mono/eglib/gpath.c from
  # compiling its own BSD copy over the CRT's declaration.
  set(HAVE_STRTOK_R 1)
else()
  set(MONO_ZERO_LEN_ARRAY 0)

  if(MONO_WITH_TLS STREQUAL "__thread")
    set(MONO_KEYWORD_THREAD "__thread")
  endif()
endif()

if(MONO_UNITY_DEFINE)
  set(UNITY 1)
endif()

if(MONO_XEN_OPT)
  set(MONO_XEN_OPT 1)
else()
  unset(MONO_XEN_OPT)
endif()

if(MONO_ENABLE_JIT_DUMP)
  set(ENABLE_JIT_DUMP 1)
endif()

if(MONO_ENABLE_INTERP_TRACE)
  set(ENABLE_INTERP_TRACE 1)
endif()

if(MONO_ENABLE_BIG_ARRAYS)
  set(MONO_BIG_ARRAYS 1)
endif()

if(MONO_HOST_WINDOWS)
  # mono/utils/mono-rand-windows.c seeds from BCryptGenRandom, so the runtime
  # has an RNG without a device to name.  NAME_DEV_RANDOM is still read by the
  # code that opens the device, hence the empty string rather than no macro.
  set(NAME_DEV_RANDOM "\"\"")
  set(HAVE_CRYPT_RNG 1)
elseif(MONO_ENABLE_DEV_RANDOM)
  set(NAME_DEV_RANDOM "\"/dev/random\"")
  set(HAVE_CRYPT_RNG 1)
endif()

if(MONO_ENABLE_CRASH_REPORTING)
  if(MONO_CRASH_PRIVACY)
    set(MONO_PRIVATE_CRASHES 1)
  endif()
else()
  set(DISABLE_CRASH_REPORTING 1)
  set(DISABLE_STRUCTURED_CRASH 1)
endif()

# --- garbage collectors -----------------------------------------------------
if(MONO_ENABLE_SGEN)
  set(HAVE_MOVING_COLLECTOR 1)
  set(HAVE_CONC_GC_AS_DEFAULT 1)
endif()

if(MONO_ENABLE_BOEHM AND MONO_ENABLE_PARALLEL_MARK)
  set(DEFAULT_GC_NAME "\"Included Boehm (with typed GC and Parallel Mark)\"")
elseif(MONO_ENABLE_BOEHM)
  set(DEFAULT_GC_NAME "\"Included Boehm (with typed GC)\"")
else()
  set(DEFAULT_GC_NAME "\"sgen\"")
endif()

# --- feature switches -------------------------------------------------------
if(MONO_ENABLE_LLVM)
  set(ENABLE_LLVM 1)
  set(ENABLE_LLVM_RUNTIME 1)
endif()

if(MONO_ENABLE_ILGEN)
  set(ENABLE_ILGEN 1)
endif()

if(NOT MONO_ENABLE_ICALL_TABLES)
  set(DISABLE_ICALL_TABLES 1)
endif()

if(NOT MONO_ENABLE_INTERPRETER)
  set(DISABLE_INTERPRETER 1)
endif()

if(NOT MONO_ENABLE_DEBUGGER_AGENT)
  set(DISABLE_DEBUGGER_AGENT 1)
endif()

if(NOT MONO_ENABLE_PROFILER)
  set(DISABLE_PROFILER 1)
endif()

if(NOT MONO_ENABLE_JIT)
  set(DISABLE_JIT 1)
endif()

if(MONO_ENABLE_BTLS)
  set(HAVE_BTLS 1)
endif()

if(MONO_THREAD_SUSPEND STREQUAL "coop")
  set(ENABLE_COOP_SUSPEND 1)
elseif(MONO_THREAD_SUSPEND STREQUAL "hybrid")
  set(ENABLE_HYBRID_SUSPEND 1)
endif()

# configure.ac filled this string from --enable-minimal=LIST, and this build
# has no equivalent option, so the string never varies.  --version reports it
# as "Disabled: none".  The AOT compiler is out of this port's scope, but the
# runtime still reads AOT images.  DISABLE_AOT therefore stays undefined, and
# aot is not one of the features this string names.
set(DISABLED_FEATURES "\"none\"")

# zlib: the runtime prefers the system copy and falls back to the bundled one.
if(ZLIB_FOUND)
  set(HAVE_SYS_ZLIB 1)
endif()
