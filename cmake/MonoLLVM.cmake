# The tier-1 backend's view of LLVM.  This is the CMake replacement for
# llvm/build_llvm_config.sh + llvm/llvm_config.mk.
#
# LLVM must be an unmodified upstream install supplied through
# MONO_LLVM_PREFIX; there is no in-tree LLVM any more.  We deliberately do not
# use LLVM's own CMake package: it exports the project's warning and
# optimization flags along with the include paths, and the backend does not
# want them.

add_library(mono_llvm INTERFACE)
add_library(mono::llvm ALIAS mono_llvm)

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

find_program(MONO_LLVM_CONFIG
             NAMES llvm-config
             HINTS "${MONO_LLVM_PREFIX}/bin"
             NO_DEFAULT_PATH
             DOC "llvm-config of the LLVM install to build against")
if(NOT MONO_LLVM_CONFIG)
  message(FATAL_ERROR "No llvm-config under ${MONO_LLVM_PREFIX}/bin; set MONO_LLVM_PREFIX to an LLVM install")
endif()

function(_mono_llvm_config out)
  execute_process(COMMAND "${MONO_LLVM_CONFIG}" ${ARGN}
                  OUTPUT_VARIABLE _out
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET
                  RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    set(_out "")
  endif()
  set(${out} "${_out}" PARENT_SCOPE)
endfunction()

_mono_llvm_config(_llvm_version --version)
if(NOT _llvm_version)
  message(FATAL_ERROR "${MONO_LLVM_CONFIG} --version failed")
endif()
string(REGEX MATCH "^[0-9]+" _llvm_major "${_llvm_version}")
if(_llvm_major LESS 14)
  message(FATAL_ERROR "LLVM ${_llvm_version} is too old; the backend needs 14 or newer")
endif()

# mono's old patched fork exposed --mono-api-version.  Upstream does not, so
# derive it from the major version the same way build_llvm_config.sh did:
# 18.x -> 1800.
_mono_llvm_config(MONO_LLVM_API_VERSION --mono-api-version)
if(NOT MONO_LLVM_API_VERSION MATCHES "^[0-9]+$")
  math(EXPR MONO_LLVM_API_VERSION "${_llvm_major} * 100")
endif()

_mono_llvm_config(_llvm_prefix --prefix)
_mono_llvm_config(_llvm_libdir --libdir)
_mono_llvm_config(_llvm_incdir --includedir)
_mono_llvm_config(_llvm_system_libs --system-libs)

# Prefer the single libLLVM dylib when the install has one.  The static
# archives of a RelWithDebInfo build weigh gigabytes and every test binary
# linking them pays a multi-minute link; the dylib links in seconds and keeps
# the backend's symbols in one place.  Name it exactly (libLLVM-<major>.so):
# an install prefix that has hosted more than one version can carry several
# dylibs side by side, and the unversioned libLLVM.so symlink would quietly
# follow whichever was installed last.
if(EXISTS "${_llvm_libdir}/libLLVM-${_llvm_major}.so")
  set(_llvm_libs "${_llvm_libdir}/libLLVM-${_llvm_major}.so")
  set(_llvm_system_libs "")
else()
  _mono_llvm_config(_llvm_libs --libs analysis core bitwriter passes orcjit x86codegen)
  separate_arguments(_llvm_libs UNIX_COMMAND "${_llvm_libs}")
endif()
separate_arguments(_llvm_system_libs UNIX_COMMAND "${_llvm_system_libs}")

target_include_directories(mono_llvm SYSTEM INTERFACE "${_llvm_incdir}")
target_compile_definitions(mono_llvm INTERFACE
  __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS
  LLVM_API_VERSION=${MONO_LLVM_API_VERSION})

# LLVM is built -fno-rtti (upstream default).  Subclassing its polymorphic types (the JIT
# memory manager, the custom passes) from a TU compiled with RTTI on is a
# silent ABI break, so the C++ side of the backend must match.  Exceptions stay
# on: the ORC APIs report failures through llvm::Error, and the unwinder needs
# the tables.
target_compile_options(mono_llvm INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
  $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-funwind-tables>)

target_link_directories(mono_llvm INTERFACE "${_llvm_libdir}")
target_link_libraries(mono_llvm INTERFACE ${_llvm_libs} ${_llvm_system_libs})
# The dylib lives outside the system search path (a local install prefix), so
# bake its directory into the runpath of everything that links it - otherwise
# every binary needs LD_LIBRARY_PATH, and the tests don't get one.
target_link_options(mono_llvm INTERFACE "-Wl,-rpath,${_llvm_libdir}")

message(STATUS "LLVM ${_llvm_version} at ${_llvm_prefix} (API version ${MONO_LLVM_API_VERSION})")
