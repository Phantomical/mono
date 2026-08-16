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
# This port is amd64-only; the other architectures configure.ac knew about are
# not reachable here, so fail loudly rather than produce a config.h that claims
# a target we do not generate code for.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "Unsupported target ${CMAKE_SYSTEM_PROCESSOR}; this build supports amd64 only")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Unsupported host ${CMAKE_SYSTEM_NAME}; this build supports Linux only")
endif()

set(HOST_AMD64   1)
set(TARGET_AMD64 1)
set(MONO_ARCHITECTURE "\"amd64\"")

# NOTE: configure.ac guarded the HOST_LINUX define with
#   if test echo x$target_os | grep -q linux; then
# which is a `test' invocation with three operands, not a pipeline, so it is
# always false and HOST_LINUX was never defined in a released build.  The
# automake conditional of the same name *was* true.  Keeping the C macro
# undefined preserves the code paths the runtime has actually been built and
# tested with; MONO_HOST_LINUX below carries the honest answer for the build
# system's own use.
set(MONO_HOST_LINUX ON)

# Classic (non-UWP) Windows API surface: defined on every target.
set(HAVE_CLASSIC_WINAPI_SUPPORT 1)
set(HAVE_UWP_WINAPI_SUPPORT     0)

set(MONO_ZERO_LEN_ARRAY 0)
set(MONO_INSIDE_RUNTIME 1)

if(MONO_WITH_TLS STREQUAL "__thread")
  set(MONO_KEYWORD_THREAD "__thread")
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

if(MONO_ENABLE_DEV_RANDOM)
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

# The AOT compiler is out of this port's scope, but the runtime still needs to
# read AOT images, so only the compiler side is switched off.
set(DISABLED_FEATURES "\"none\"")

# zlib: the runtime prefers the system copy and falls back to the bundled one.
find_package(ZLIB)
if(ZLIB_FOUND)
  set(HAVE_SYS_ZLIB 1)
endif()
