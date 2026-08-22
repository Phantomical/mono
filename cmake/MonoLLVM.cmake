# The tier-1 backend's view of LLVM.  This is the CMake replacement for
# llvm/build_llvm_config.sh + llvm/llvm_config.mk.
#
# LLVM must be an unmodified upstream install supplied through
# MONO_LLVM_PREFIX; there is no in-tree LLVM any more.  Everything here comes
# out of the LLVM CMake package the top-level CMakeLists.txt found.

add_library(mono_llvm INTERFACE)
add_library(mono::llvm ALIAS mono_llvm)

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

if(NOT LLVM_FOUND)
  message(FATAL_ERROR
    "No LLVM CMake package under ${MONO_LLVM_PREFIX}; point MONO_LLVM_PREFIX at "
    "an LLVM install that ships lib/cmake/llvm")
endif()

if(LLVM_VERSION_MAJOR LESS 14)
  message(FATAL_ERROR
    "LLVM ${LLVM_PACKAGE_VERSION} is too old; the backend needs 14 or newer")
endif()

# 18.x -> 1800, the way build_llvm_config.sh derived it.
math(EXPR MONO_LLVM_API_VERSION "${LLVM_VERSION_MAJOR} * 100")

# Prefer the single libLLVM dylib when the install has one.
if(TARGET LLVM)
  set(_llvm_libs LLVM)
else()
  llvm_map_components_to_libnames(_llvm_libs
    analysis core bitwriter linker passes orcjit x86codegen)
endif()

target_include_directories(mono_llvm SYSTEM INTERFACE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(mono_llvm INTERFACE
  __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS
  LLVM_API_VERSION=${MONO_LLVM_API_VERSION})

# An LLVM with assertions on is the configuration that is being checked rather
# than shipped.  The IR verifier belongs to the same class of check, and it
# costs 11-15% of compile CPU.  In exchange the verifier catches a malformed
# module, which otherwise miscompiles silently.  The backend's default follows
# LLVM's own assertion setting, and MONO_LLVM_JIT_VERIFY overrides either way.
if(LLVM_ENABLE_ASSERTIONS)
  target_compile_definitions(mono_llvm INTERFACE MONO_LLVM_ASSERTIONS=1)
endif()

# Subclassing LLVM's polymorphic types (the JIT memory manager, the custom
# passes) out of a TU that disagrees with it about RTTI is a silent ABI break,
# so the C++ side of the backend takes its answer from the install.
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(mono_llvm INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
endif()

# Exceptions stay on regardless of how LLVM itself was built: the ORC APIs
# report failures through llvm::Error, and the unwinder needs the tables.
target_compile_options(mono_llvm INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-funwind-tables>)

target_link_libraries(mono_llvm INTERFACE ${_llvm_libs})
# The dylib lives outside the system search path (a local install prefix), so
# bake its directory into the runpath of everything that links it - otherwise
# every binary needs LD_LIBRARY_PATH, and the tests don't get one.
target_link_options(mono_llvm INTERFACE "-Wl,-rpath,${LLVM_LIBRARY_DIRS}")

message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_INSTALL_PREFIX} (API version ${MONO_LLVM_API_VERSION})")
