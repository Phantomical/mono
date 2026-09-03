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

set(_llvm_components analysis core bitwriter linker passes orcjit x86codegen x86asmparser)

# Prefer the single libLLVM dylib when the install has one.  A Unity build
# takes a static LLVM instead, because an allocation inside the dylib binds to
# the player's operator new.
#
# libLLVM.a comes before the component archives, because an install built with
# LLVM_ENABLE_LTO leaves bitcode in those and ld answers "file format not
# recognized".  Linking the bitcode instead runs a ThinLTO of the whole of LLVM
# on every relink, so such an install wants that link done once and archived.
# A raw path carries none of the dependencies an imported target would, which
# is what llvm-config is asked for here.
if(MONO_UNITY_BUILD)
  find_library(MONO_LLVM_STATIC NAMES libLLVM.a
               PATHS "${LLVM_LIBRARY_DIRS}" NO_DEFAULT_PATH
               DOC "Static LLVM the Unity build links")
  if(MONO_LLVM_STATIC)
    execute_process(
      COMMAND "${LLVM_TOOLS_BINARY_DIR}/llvm-config" --link-static --system-libs
      OUTPUT_VARIABLE _llvm_system_libs OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(_llvm_system_libs NATIVE_COMMAND "${_llvm_system_libs}")
    set(_llvm_libs "${MONO_LLVM_STATIC}" ${_llvm_system_libs})
    get_filename_component(_llvm_archives "${MONO_LLVM_STATIC}" NAME)

    # This archive is over a gigabyte with debug info, and GNU ld wants it in
    # memory all at once.  lld reads it in a fraction of the time and the space.
    find_program(MONO_LLD NAMES ld.lld)
    if(MONO_LLD)
      add_link_options(-fuse-ld=lld)
    else()
      message(WARNING "no ld.lld; linking ${MONO_LLVM_STATIC} with the default "
                      "linker needs several GB per link")
    endif()

    # Even so, one link at a time: Ninja otherwise starts as many as -j allows
    # and the machine runs out of memory.  Raise the pool where there is
    # headroom for it.
    set_property(GLOBAL PROPERTY JOB_POOLS mono_link=1)
    set(CMAKE_JOB_POOL_LINK mono_link)
  elseif(TARGET LLVMCore)
    llvm_map_components_to_libnames(_llvm_libs ${_llvm_components})
    foreach(_lib IN LISTS _llvm_libs)
      list(APPEND _llvm_archives "lib${_lib}.a")
    endforeach()
  else()
    message(FATAL_ERROR
      "MONO_UNITY_BUILD needs a static LLVM; ${LLVM_INSTALL_PREFIX} ships the "
      "dylib alone")
  endif()
elseif(TARGET LLVM)
  set(_llvm_libs LLVM)
else()
  llvm_map_components_to_libnames(_llvm_libs ${_llvm_components})
endif()

# A static LLVM otherwise reaches the dynamic symbol table, because
# --export-dynamic puts everything there: mono-sgen exported 45506 symbols
# against 6322, 24130 of them llvm::.  --exclude-libs names the archives to
# keep out rather than ALL.  The runtime links libmonosgen-2.0.a and
# libmonogc.a as archives too, and managed code P/Invokes what those export.
# One option per archive: the comma-separated form ld also takes cannot cross
# -Wl,, which splits on commas.
foreach(_archive IN LISTS _llvm_archives)
  target_link_options(mono_llvm INTERFACE "-Wl,--exclude-libs,${_archive}")
endforeach()

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
  if(MSVC)
    target_compile_options(mono_llvm INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/GR->)
  else()
    target_compile_options(mono_llvm INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
  endif()
endif()

# Exceptions stay on regardless of how LLVM itself was built: the ORC APIs
# report failures through llvm::Error, and the unwinder needs the tables.
# MSVC's /EHsc, which CMake passes already, is both of those.
if(NOT MSVC)
  target_compile_options(mono_llvm INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-funwind-tables>)
endif()

target_link_libraries(mono_llvm INTERFACE ${_llvm_libs})
# The dylib lives outside the system search path (a local install prefix), so
# bake its directory into the runpath of everything that links it - otherwise
# every binary needs LD_LIBRARY_PATH, and the tests don't get one.  A PE image
# has no such field: it finds a DLL by the loader's search order, and this
# build links LLVM statically there anyway.
if(NOT MSVC)
  target_link_options(mono_llvm INTERFACE "-Wl,-rpath,${LLVM_LIBRARY_DIRS}")
endif()

message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_INSTALL_PREFIX} (API version ${MONO_LLVM_API_VERSION})")
