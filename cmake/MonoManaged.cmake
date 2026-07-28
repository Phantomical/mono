# The class-library build.
#
# mcs/ used to carry its own recursive make system, which this build shelled
# out to.  This module replaces it: every assembly under mcs/ becomes a normal
# CMake custom command, so the whole tree -- native and managed -- lives in one
# dependency graph, one job pool, and one incremental rebuild.
#
# Two things about the shape of this file are worth knowing up front.
#
# Declarations are collected, not built.  A directory's CMakeLists.txt calls
# mono_declare_managed() and nothing happens; mcs/CMakeLists.txt calls
# mono_managed_materialize() at the end and every target appears at once.  The
# reason is that references are written as bare assembly names -- `LIB_REFS =
# System System.XML` in the old makefiles -- and a name cannot be resolved to
# the target that produces it until every directory has been read.  Collecting
# first also means the resulting graph is the real dependency DAG rather than
# the serial-then-barrier approximation the makefiles encoded with SUBDIRS and
# PARALLEL_SUBDIRS.
#
# The same directory builds in several profiles.  A declaration therefore names
# the profiles it participates in, and produces one target per profile, named
# mcs-<profile>-<assembly>.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Where things go
# ---------------------------------------------------------------------------
# Output lands in the build tree.  The makefiles wrote it back into mcs/, which
# meant a second build directory over one checkout silently clobbered the
# first, and left the source tree dirty after every build.
set(MONO_MANAGED_ROOT "${CMAKE_BINARY_DIR}/mcs")
set(MONO_MANAGED_LIBDIR "${MONO_MANAGED_ROOT}/class/lib")
set(MONO_MANAGED_DEPSDIR "${MONO_MANAGED_ROOT}/deps")
set(MONO_MANAGED_PLATFORMS_DIR "${MONO_MCS_TOPDIR}/build")

file(MAKE_DIRECTORY "${MONO_MANAGED_LIBDIR}" "${MONO_MANAGED_DEPSDIR}")

# ---------------------------------------------------------------------------
# The runtime that bootstraps the compiler
# ---------------------------------------------------------------------------
# The `build` profile compiles on a mono that already works, spelled `mono` and
# resolved through PATH -- it is what produces the first mscorlib, so it cannot
# be the runtime being built.  This is independent of
# MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS, which only moves the *later* profiles.
find_program(MONO_BOOTSTRAP_RUNTIME NAMES mono mono-sgen)
if(NOT MONO_BOOTSTRAP_RUNTIME)
  message(FATAL_ERROR
    "No `mono` found on PATH.  The class libraries are bootstrapped by a mono "
    "that already works; install one, or point MONO_BOOTSTRAP_RUNTIME at it.")
endif()

# Fail at configure time rather than a thousand compile commands later.  This
# is the check the makefiles ran as `basic-profile-check`, which on failure
# downloaded a monolite tarball and latched a flag file that silently
# redirected later builds onto the in-tree runtime.  Failing loudly is better.
if(NOT MONO_BOOTSTRAP_RUNTIME_CHECKED STREQUAL "${MONO_BOOTSTRAP_RUNTIME}")
  execute_process(COMMAND "${MONO_BOOTSTRAP_RUNTIME}" --version
                  OUTPUT_VARIABLE _v ERROR_VARIABLE _v RESULT_VARIABLE _rc
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "'${MONO_BOOTSTRAP_RUNTIME} --version' failed:\n${_v}")
  endif()
  set(MONO_BOOTSTRAP_RUNTIME_CHECKED "${MONO_BOOTSTRAP_RUNTIME}" CACHE INTERNAL "")
endif()

# ---------------------------------------------------------------------------
# Profiles
# ---------------------------------------------------------------------------
# Ported from mcs/build/profiles/*.make for HOST_PLATFORM=linux.  The knobs the
# mobile profiles used to set -- MOBILE_PROFILE, NO_SRE, AOT_FRIENDLY_PROFILE
# and the rest -- are constants now that those profiles are gone, so the
# conditionals they drove are folded away rather than carried over.
#
# DIRECTORY is the on-disk name; ALIAS is the unsuffixed name everything else
# in the tree spells.  The makefiles created the alias as a symlink lazily, as
# a side effect of the first library built; it is made up front here.

set(MONO_MANAGED_PROFILES build net_4_x xbuild_12 xbuild_14 unityjit)

macro(_mono_profile name)
  cmake_parse_arguments(P "" "DIRECTORY;ALIAS;FRAMEWORK_VERSION;XBUILD_VERSION;API_BIN_PROFILE;BOOTSTRAP"
                          "MCS_FLAGS;DEFAULT_REFERENCES;OPTIONS" ${ARGN})
  set(MONO_PROFILE_${name}_DIRECTORY   "${P_DIRECTORY}")
  set(MONO_PROFILE_${name}_ALIAS       "${P_ALIAS}")
  set(MONO_PROFILE_${name}_MCS_FLAGS   "${P_MCS_FLAGS}")
  set(MONO_PROFILE_${name}_DEFAULT_REFERENCES "${P_DEFAULT_REFERENCES}")
  set(MONO_PROFILE_${name}_FRAMEWORK_VERSION  "${P_FRAMEWORK_VERSION}")
  set(MONO_PROFILE_${name}_XBUILD_VERSION     "${P_XBUILD_VERSION}")
  set(MONO_PROFILE_${name}_API_BIN_PROFILE    "${P_API_BIN_PROFILE}")
  set(MONO_PROFILE_${name}_BOOTSTRAP          "${P_BOOTSTRAP}")
  foreach(_o IN LISTS P_OPTIONS)
    set(MONO_PROFILE_${name}_${_o} TRUE)
  endforeach()
endmacro()

# The bootstrap profile.  Compiles on system mono against the checked-in
# reference assemblies; signs nothing and installs nothing.  Its whole purpose
# is to produce a toolchain good enough to build net_4_x.
_mono_profile(build
  DIRECTORY build-linux
  ALIAS     build
  MCS_FLAGS -d:NET_4_0 -d:NET_4_5 -d:MONO -d:WIN_PLATFORM -d:BOOTSTRAP_BASIC
            -nowarn:1699 -nostdlib
  DEFAULT_REFERENCES mscorlib
  FRAMEWORK_VERSION  4.5
  API_BIN_PROFILE    v4.7.1
  OPTIONS   NO_SIGN NO_INSTALL NO_TEST BOOTSTRAP_COMPILER)

_mono_profile(net_4_x
  DIRECTORY net_4_x-linux
  ALIAS     net_4_x
  MCS_FLAGS -d:NET_4_0 -d:NET_4_5 -d:NET_4_6 -d:MONO -d:WIN_PLATFORM
            -nowarn:1699 -nostdlib
  DEFAULT_REFERENCES mscorlib
  FRAMEWORK_VERSION  4.5
  XBUILD_VERSION     4.0
  API_BIN_PROFILE    v4.7.1
  BOOTSTRAP          build
  OPTIONS   WARN_AS_ERROR ENABLE_GSS DEBUG FACADES)

# xbuild builds against net_4_x, reached through the alias -- these two
# profiles are unsuffixed, so `../net_4_x/mscorlib` only resolves because the
# alias directory exists.
_mono_profile(xbuild_12
  DIRECTORY xbuild_12
  MCS_FLAGS -d:NET_4_0 -d:NET_4_5 -d:NET_4_6 -d:MONO -d:WIN_PLATFORM
            -nowarn:1699 -nostdlib -d:XBUILD_12
  DEFAULT_REFERENCES ../net_4_x/mscorlib
  FRAMEWORK_VERSION  4.5
  XBUILD_VERSION     12.0
  API_BIN_PROFILE    v4.7.1
  BOOTSTRAP          build
  OPTIONS   WARN_AS_ERROR ENABLE_GSS DEBUG)

_mono_profile(xbuild_14
  DIRECTORY xbuild_14
  MCS_FLAGS -d:NET_4_0 -d:NET_4_5 -d:NET_4_6 -d:MONO -d:WIN_PLATFORM
            -nowarn:1699 -nostdlib -d:XBUILD_12 -d:XBUILD_14
  DEFAULT_REFERENCES ../net_4_x/mscorlib
  FRAMEWORK_VERSION  4.5
  XBUILD_VERSION     14.0
  API_BIN_PROFILE    v4.7.1
  BOOTSTRAP          build
  OPTIONS   WARN_AS_ERROR ENABLE_GSS DEBUG)

_mono_profile(unityjit
  DIRECTORY unityjit-linux
  ALIAS     unityjit
  MCS_FLAGS -d:NET_4_0 -d:NET_4_5 -d:NET_4_6 -d:MONO -d:UNITY_JIT -d:UNITY
            -d:WIN_PLATFORM -nowarn:1699 -nostdlib -d:DISABLE_COM
  DEFAULT_REFERENCES mscorlib
  FRAMEWORK_VERSION  4.5
  XBUILD_VERSION     4.0
  API_BIN_PROFILE    v4.7.1
  BOOTSTRAP          build
  OPTIONS   ENABLE_GSS DEBUG FACADES)

# Flags every compile gets, from build/rules.make and build/config-default.make
# with the linux platform fragment folded in (which contributes nothing but the
# `:` path separator and a `cat` that made the response file a plain copy).
set(MONO_MANAGED_COMMON_FLAGS
    /codepage:65001 /nologo /noconfig /deterministic /langversion:latest
    -optimize)                              # -optimize is config.make's BCL_OPTIMIZE
set(MONO_MANAGED_DEBUG_FLAGS /debug:portable)

function(mono_profile_dir out profile)
  set(${out} "${MONO_MANAGED_LIBDIR}/${MONO_PROFILE_${profile}_DIRECTORY}" PARENT_SCOPE)
endfunction()

# The alias directories, created before anything compiles rather than as a side
# effect of the first library to notice they were missing.
foreach(_p IN LISTS MONO_MANAGED_PROFILES)
  mono_profile_dir(_dir ${_p})
  file(MAKE_DIRECTORY "${_dir}")
  set(_alias "${MONO_PROFILE_${_p}_ALIAS}")
  if(_alias AND NOT _alias STREQUAL "${MONO_PROFILE_${_p}_DIRECTORY}")
    if(NOT EXISTS "${MONO_MANAGED_LIBDIR}/${_alias}")
      file(CREATE_LINK "${_dir}" "${MONO_MANAGED_LIBDIR}/${_alias}" SYMBOLIC)
    endif()
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Declaring an assembly
# ---------------------------------------------------------------------------
# One call per assembly per directory; PROFILES says which profiles build it.
# The argument names follow the makefile variables they replace closely enough
# to diff against them: NAME is LIBRARY/PROGRAM, OUTPUT_NAME is LIBRARY_NAME,
# REFS is LIB_REFS, FLAGS is LIB_MCS_FLAGS.
function(mono_declare_managed)
  cmake_parse_arguments(A
    "PROGRAM;NO_SIGN;NO_INSTALL;NO_DEBUG;INTERMEDIATE;NO_DEFAULT_REFERENCES"
    "NAME;OUTPUT_NAME;SUBDIR;KEYFILE;SNK;PACKAGE;INSTALL_DIR;TARGET_NET_REFERENCE;SOURCES_FILE"
    "PROFILES;REFS;API_BIN_REFS;FLAGS;BUILT_SOURCES;DEPENDS;RESOURCES;STRING_REPLACER_FLAGS;ENV;SOURCES"
    ${ARGN})

  if(NOT A_NAME)
    message(FATAL_ERROR "mono_declare_managed: NAME is required")
  endif()
  if(NOT A_PROFILES)
    message(FATAL_ERROR "mono_declare_managed(${A_NAME}): PROFILES is required")
  endif()

  # Everything is stashed as one flat property per declaration; the fields are
  # read back in mono_managed_materialize().  A list of lists would need
  # escaping that CMake makes more painful than it is worth.
  get_property(_n GLOBAL PROPERTY MONO_MANAGED_COUNT)
  if(NOT _n)
    set(_n 0)
  endif()
  set(_id "MONO_MANAGED_${_n}")

  foreach(_f NAME OUTPUT_NAME SUBDIR KEYFILE SNK PACKAGE INSTALL_DIR
             TARGET_NET_REFERENCE SOURCES_FILE PROFILES REFS API_BIN_REFS FLAGS
             BUILT_SOURCES DEPENDS RESOURCES STRING_REPLACER_FLAGS ENV SOURCES
             PROGRAM NO_SIGN NO_INSTALL NO_DEBUG INTERMEDIATE NO_DEFAULT_REFERENCES)
    set_property(GLOBAL PROPERTY ${_id}_${_f} "${A_${_f}}")
  endforeach()
  set_property(GLOBAL PROPERTY ${_id}_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

  math(EXPR _n "${_n} + 1")
  set_property(GLOBAL PROPERTY MONO_MANAGED_COUNT ${_n})
endfunction()

# ---------------------------------------------------------------------------
# The compiler, and the tools that surround it
# ---------------------------------------------------------------------------
# Roslyn's compiler server is worth a lot across ~900 compiles.  The pipe name
# is derived from the build directory: the makefiles used the fixed literal
# `monomake`, so two build trees shared one server -- and that server holds the
# other tree's class/lib/build assemblies open through MONO_PATH.
option(MONO_MCS_COMPILER_SERVER "Use the Roslyn compiler server for class-library compiles" ON)
string(SHA256 _mono_pipe_hash "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${_mono_pipe_hash}" 0 16 _mono_pipe_hash)
set(MONO_MCS_PIPENAME "mono-${_mono_pipe_hash}" CACHE STRING
    "Roslyn compiler-server pipe name (per build directory)")

# Assembles the argv for one managed tool.
#
# The `build` profile is the odd one.  Its tools run on system mono with no
# MONO_PATH, out of a `tmp/` directory holding nothing but themselves -- mono
# resolves an assembly's dependencies from its own directory, and the normal
# location is simultaneously being filled with this tree's mscorlib.dll, which
# system mono cannot load.  Every other profile runs them on the runtime being
# built, with MONO_PATH pointing at the bootstrap output.
function(_mono_tool_command out profile tool)
  mono_profile_dir(_builddir build)
  if(MONO_PROFILE_${profile}_BOOTSTRAP_COMPILER)
    set(${out} "${MONO_BOOTSTRAP_RUNTIME}" "${_builddir}/tmp/${tool}" PARENT_SCOPE)
  else()
    set(${out} "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" "${_builddir}/${tool}" PARENT_SCOPE)
  endif()
endfunction()

# Tools for every profile but the bootstrap one run on the runtime this build
# produces, so that runtime has to exist first.  The bootstrap profile runs on
# system mono and depends on nothing here.
function(_mono_tool_depends out profile)
  if(MONO_PROFILE_${profile}_BOOTSTRAP_COMPILER)
    set(${out} "" PARENT_SCOPE)
  else()
    # mono-build-config is not optional: the wrapper points MONO_CFG_DIR at
    # the build tree's etc/, and without the dllmap in it every tool that
    # touches the filesystem dies with "DllNotFoundException: System.Native".
    set(_d mono-${MONO_DEFAULT_GC_SUFFIX} mono-build-config)
    # The BCL's filesystem and networking layers P/Invoke into this, and the
    # dllmap names it by path in the build tree.
    if(MONO_ENABLE_MONO_NATIVE)
      list(APPEND _d mono-native)
    endif()
    set(${out} ${_d} PARENT_SCOPE)
  endif()
endfunction()

function(_mono_tool_env out profile)
  if(MONO_PROFILE_${profile}_BOOTSTRAP_COMPILER)
    set(${out} "" PARENT_SCOPE)
  else()
    mono_profile_dir(_builddir build)
    set(${out} "MONO_PATH=${_builddir}" PARENT_SCOPE)
  endif()
endfunction()

# What csc itself runs on.  In the bootstrap profile that is always system mono
# -- it is producing the first mscorlib, so it cannot be the runtime being
# built.  Later profiles follow MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS.
function(_mono_csc_command out profile)
  if(MONO_PROFILE_${profile}_BOOTSTRAP_COMPILER)
    set(${out} "${MONO_BOOTSTRAP_RUNTIME}" "${MONO_CSC}" PARENT_SCOPE)
  elseif(MONO_TOOLS_RUNTIME_IS_SYSTEM)
    set(${out} "${MONO_CSC_HOST}" --gc-params=nursery-size=64m "${MONO_CSC}" PARENT_SCOPE)
  else()
    set(${out} "${CMAKE_BINARY_DIR}/runtime/mono-wrapper"
               --gc-params=nursery-size=64m "${MONO_CSC}" PARENT_SCOPE)
  endif()
endfunction()

function(_mono_csc_env out profile)
  set(_env "")
  if(NOT MONO_PROFILE_${profile}_BOOTSTRAP_COMPILER)
    mono_profile_dir(_builddir build)
    # Empty, not unset: it is a sanity check that every reference csc gets was
    # named by an explicit path rather than found through an SDK directory.
    list(APPEND _env "CSC_SDK_PATH_DISABLED=" "MONO_PATH=${_builddir}")
  endif()
  set(${out} "${_env}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Materializing
# ---------------------------------------------------------------------------
# Turns every declaration into targets.  Called once, from mcs/CMakeLists.txt,
# after all the subdirectories have been added.

# `Foo.dll` -> `Foo`; references are written without the extension.
function(_mono_stem out name)
  string(REGEX REPLACE "\\.(dll|exe)$" "" _s "${name}")
  set(${out} "${_s}" PARENT_SCOPE)
endfunction()

function(_mono_target_name out profile subdir name)
  _mono_stem(_s "${name}")
  if(subdir)
    set(${out} "mcs-${profile}-${subdir}-${_s}" PARENT_SCOPE)
  else()
    set(${out} "mcs-${profile}-${_s}" PARENT_SCOPE)
  endif()
endfunction()

function(mono_managed_materialize)
  get_property(_count GLOBAL PROPERTY MONO_MANAGED_COUNT)
  if(NOT _count)
    return()
  endif()
  math(EXPR _last "${_count} - 1")

  # Pass 1 -- who produces what.  A reference is a bare assembly name, so this
  # map is the only way to turn `LIB_REFS = System` into an edge.
  foreach(_i RANGE ${_last})
    set(_id "MONO_MANAGED_${_i}")
    get_property(_name    GLOBAL PROPERTY ${_id}_NAME)
    get_property(_subdir  GLOBAL PROPERTY ${_id}_SUBDIR)
    get_property(_profs   GLOBAL PROPERTY ${_id}_PROFILES)
    _mono_stem(_stem "${_name}")
    foreach(_p IN LISTS _profs)
      _mono_target_name(_t ${_p} "${_subdir}" "${_name}")
      if(_subdir)
        set(_key "${_p}/${_subdir}/${_stem}")
      else()
        set(_key "${_p}/${_stem}")
      endif()
      get_property(_existing GLOBAL PROPERTY MONO_MANAGED_PROVIDER_${_key})
      if(_existing)
        message(FATAL_ERROR
          "Two declarations produce ${_stem} in profile ${_p}: ${_existing} and ${_t}")
      endif()
      set_property(GLOBAL PROPERTY MONO_MANAGED_PROVIDER_${_key} "${_t}")
    endforeach()
  endforeach()

  # Pass 2 -- the commands.
  foreach(_i RANGE ${_last})
    _mono_materialize_one("MONO_MANAGED_${_i}")
  endforeach()

  # One aggregate per profile, so the profile ordering constraints and the
  # rest of the build can name them.
  foreach(_p IN LISTS MONO_MANAGED_PROFILES)
    get_property(_targets GLOBAL PROPERTY MONO_MANAGED_PROFILE_TARGETS_${_p})
    add_custom_target(mcs-${_p})
    if(_targets)
      add_dependencies(mcs-${_p} ${_targets})
    endif()
  endforeach()
endfunction()

function(_mono_materialize_one id)
  foreach(_f NAME OUTPUT_NAME SUBDIR KEYFILE SNK PACKAGE INSTALL_DIR
             TARGET_NET_REFERENCE SOURCES_FILE PROFILES REFS API_BIN_REFS FLAGS
             BUILT_SOURCES DEPENDS RESOURCES STRING_REPLACER_FLAGS ENV SOURCES
             PROGRAM NO_SIGN NO_INSTALL NO_DEBUG INTERMEDIATE NO_DEFAULT_REFERENCES DIR)
    get_property(A_${_f} GLOBAL PROPERTY ${id}_${_f})
  endforeach()

  foreach(_p IN LISTS A_PROFILES)
    _mono_materialize_profile("${_p}")
  endforeach()
endfunction()

# Split out only so the variables above stay in scope; not meant to be called
# directly.
macro(_mono_materialize_profile _profile)
  # A macro argument is textual substitution, not a variable, so it cannot be
  # reached by the @VAR@ expansion in the settings file below.  Everything that
  # has to survive into that file goes through a real variable.
  set(_prof "${_profile}")
  mono_profile_dir(_pdir ${_profile})
  set(_outdir "${_pdir}")
  if(A_SUBDIR)
    set(_outdir "${_pdir}/${A_SUBDIR}")
  endif()

  set(_outname "${A_NAME}")
  if(A_OUTPUT_NAME)
    set(_outname "${A_OUTPUT_NAME}")
  endif()
  set(_out "${_outdir}/${_outname}")

  # A few bootstrap assemblies compile into tmp/ and are copied up.  That
  # directory is the clean app base the bootstrap tools run out of; see
  # _mono_tool_command().
  if(A_INTERMEDIATE)
    set(_build_out "${_outdir}/tmp/${_outname}")
  else()
    set(_build_out "${_out}")
  endif()

  _mono_target_name(_target ${_profile} "${A_SUBDIR}" "${A_NAME}")
  _mono_stem(_stem "${A_NAME}")

  # -- references ----------------------------------------------------------
  # Two resolution modes coexist.  Most references point at what this build
  # just produced; API_BIN_REFS and TARGET_NET_REFERENCE point at the
  # checked-in reference assemblies instead, which is how the bootstrap tools
  # compile against a stable surface rather than the half-built BCL.
  set(_refflags "")
  set(_refdeps "")
  # corlib is the one assembly with no references at all -- it *is* the
  # reference everything else resolves against.
  set(_refs ${A_REFS})
  if(NOT A_NO_DEFAULT_REFERENCES)
    list(APPEND _refs ${MONO_PROFILE_${_profile}_DEFAULT_REFERENCES})
  endif()

  # API_BIN_REFS and TARGET_NET_REFERENCE both reach into the checked-in
  # reference assemblies but pick different versions of them, so they get
  # separate directories even though no assembly currently uses both.
  set(_refasm_root "${CMAKE_SOURCE_DIR}/external/binary-reference-assemblies")
  set(_api_bin_dir "${_refasm_root}/${MONO_PROFILE_${_profile}_API_BIN_PROFILE}")
  set(_binref_dir  "${_refasm_root}/${A_TARGET_NET_REFERENCE}")

  foreach(_r IN LISTS A_API_BIN_REFS)
    list(APPEND _refflags "-r:${_api_bin_dir}/${_r}.dll")
  endforeach()

  foreach(_r IN LISTS _refs)
    # `alias=Assembly` is an extern alias; the alias survives into the flag.
    set(_alias "")
    if(_r MATCHES "^([^=]+)=(.+)$")
      set(_alias "${CMAKE_MATCH_1}=")
      set(_r "${CMAKE_MATCH_2}")
    endif()
    # With TARGET_NET_REFERENCE the framework half of the references comes from
    # the checked-in reference assemblies and the rest still comes from this
    # build -- cil-stringreplacer takes System and mscorlib from v4.7 but
    # Mono.Cecil from the bootstrap output.  The makefiles selected on the name
    # containing "System" or "mscorlib", so this does too.
    if(A_TARGET_NET_REFERENCE AND (_r MATCHES "System" OR _r MATCHES "mscorlib"))
      list(APPEND _refflags "-r:${_alias}${_binref_dir}/${_r}.dll")
      continue()
    endif()
    # `../net_4_x/mscorlib` -- the xbuild profiles reach into net_4_x through
    # the alias directory.
    set(_refprofile "${_profile}")
    set(_refname "${_r}")
    if(_r MATCHES "^\\.\\./([^/]+)/(.+)$")
      set(_refprofile "${CMAKE_MATCH_1}")
      set(_refname "${CMAKE_MATCH_2}")
      mono_profile_dir(_rdir ${_refprofile})
      list(APPEND _refflags "-r:${_alias}${_rdir}/${_refname}.dll")
    else()
      list(APPEND _refflags "-r:${_alias}${_pdir}/${_refname}.dll")
    endif()
    get_property(_provider GLOBAL PROPERTY MONO_MANAGED_PROVIDER_${_refprofile}/${_refname})
    if(_provider)
      list(APPEND _refdeps "${_provider}")
    endif()
  endforeach()

  # -- flags ---------------------------------------------------------------
  set(_flags ${MONO_MANAGED_COMMON_FLAGS})
  if(MONO_MCS_COMPILER_SERVER)
    list(APPEND _flags "/shared:${MONO_MCS_PIPENAME}")
  endif()
  list(APPEND _flags ${MONO_PROFILE_${_profile}_MCS_FLAGS})
  # PLATFORM_DEBUG_FLAGS, which 135 makefiles cleared locally -- the Facades
  # and friends ship without a .pdb and without sourcelink.
  if(MONO_PROFILE_${_profile}_DEBUG AND NOT A_NO_DEBUG)
    list(APPEND _flags ${MONO_MANAGED_DEBUG_FLAGS}
                       "-sourcelink:${MONO_MCS_TOPDIR}/build/common/sourcelink.json")
  endif()
  list(APPEND _flags ${A_FLAGS} ${_refflags})
  if(A_KEYFILE)
    list(APPEND _flags "/keyfile:${A_DIR}/${A_KEYFILE}")
  endif()
  if(A_PROGRAM)
    list(APPEND _flags -target:exe)
  else()
    list(APPEND _flags -target:library)
  endif()

  # -- the source list -----------------------------------------------------
  set(_sources_inputs "")
  set(_gensources "")
  if(A_SOURCES)
    # A handful of directories -- tools/security most of all -- build several
    # assemblies from explicit source lists rather than one .sources file, so
    # the response file is written here instead of by gensources.
    set(_response "${MONO_MANAGED_DEPSDIR}/${_target}.sources")
    set(_abs "")
    foreach(_s IN LISTS A_SOURCES)
      if(NOT IS_ABSOLUTE "${_s}")
        set(_s "${A_DIR}/${_s}")
      endif()
      list(APPEND _abs "${_s}")
    endforeach()
    string(JOIN "\n" _body ${_abs})
    file(CONFIGURE OUTPUT "${_response}" CONTENT "${_body}\n")
    set(_sources_inputs ${_abs})
  elseif(A_PROGRAM)
    # Programs are handed their .sources verbatim; no expansion step.
    set(_sourcefile "${A_SOURCES_FILE}")
    if(NOT _sourcefile)
      set(_sourcefile "${A_NAME}.sources")
    endif()
    set(_response "${A_DIR}/${_sourcefile}")
    list(APPEND _sources_inputs "${_response}")
  else()
    if(A_SUBDIR)
      set(_response "${MONO_MANAGED_DEPSDIR}/${_profile}_${A_SUBDIR}_${A_NAME}.sources")
    else()
      set(_response "${MONO_MANAGED_DEPSDIR}/${_profile}_${A_NAME}.sources")
    endif()
    _mono_tool_command(_gensources ${_profile} gensources.exe)
    # Every library's source list comes out of gensources, so it has to be
    # built first -- including for the bootstrap profile, where it is the very
    # first assembly produced.
    get_property(_gsprov GLOBAL PROPERTY MONO_MANAGED_PROVIDER_build/gensources)
    if(_gsprov AND NOT _gsprov STREQUAL _target)
      list(APPEND _refdeps "${_gsprov}")
    endif()
    # Any .sources in the directory can change the answer, so all of them are
    # inputs.  The makefiles did the same, with the same admitted coarseness.
    file(GLOB _sources_inputs CONFIGURE_DEPENDS "${A_DIR}/*.sources")
  endif()

  # -- assemble the settings file the driver reads -------------------------
  _mono_csc_command(_csc ${_profile})
  _mono_csc_env(_csc_env ${_profile})
  _mono_tool_env(_tool_env ${_profile})
  _mono_tool_depends(_tool_deps ${_profile})
  list(APPEND _refdeps ${_tool_deps})
  list(APPEND _csc_env ${A_ENV})

  # Libraries are re-signed by default; programs only when they name a key.
  # executable.make guards the sn call on PROGRAM_SNK, and nearly nothing sets
  # it -- signing every .exe fails outright on assemblies delay-signed with a
  # public key that is not mono.snk's.
  set(_sn "")
  set(_snk "")
  set(_do_sign TRUE)
  if(A_NO_SIGN OR MONO_PROFILE_${_profile}_NO_SIGN)
    set(_do_sign FALSE)
  elseif(A_PROGRAM AND NOT A_SNK)
    set(_do_sign FALSE)
  endif()
  if(_do_sign)
    _mono_tool_command(_sn ${_profile} sn.exe)
    list(APPEND _sn -q)
    set(_snk "${MONO_MCS_TOPDIR}/class/mono.snk")
    if(A_SNK)
      set(_snk "${A_DIR}/${A_SNK}")
    endif()
    get_property(_snprov GLOBAL PROPERTY MONO_MANAGED_PROVIDER_build/sn)
    if(_snprov)
      list(APPEND _refdeps "${_snprov}")
    endif()
  endif()

  # Only corlib has anything to do here: its string table and the ilasm'd
  # Unsafe.il module are spliced into the assembly csc just emitted.
  set(_string_replacer "")
  if(A_STRING_REPLACER_FLAGS)
    _mono_tool_command(_string_replacer ${_profile} cil-stringreplacer.exe)
    get_property(_srprov GLOBAL PROPERTY MONO_MANAGED_PROVIDER_build/cil-stringreplacer)
    if(_srprov)
      list(APPEND _refdeps "${_srprov}")
    endif()
  endif()

  set(_settings "${MONO_MANAGED_DEPSDIR}/${_target}.cmake")
  set(_depfile  "${MONO_MANAGED_DEPSDIR}/${_target}.d")

  set(_built "")
  foreach(_b IN LISTS A_BUILT_SOURCES)
    list(APPEND _built "${_b}")
  endforeach()

  # PLATFORM_PLATFORM is empty for the xbuild profiles, which declare no
  # PLATFORMS and are therefore unsuffixed.
  set(_platform "linux")
  if(NOT MONO_PROFILE_${_profile}_ALIAS)
    set(_platform "")
  endif()

  file(CONFIGURE OUTPUT "${_settings}" CONTENT [[
# Generated by MonoManaged.cmake -- do not edit.
set(MCS_SOURCE_DIR    [==[@A_DIR@]==])
set(MCS_OUTPUT        [==[@_out@]==])
set(MCS_BUILD_OUTPUT  [==[@_build_out@]==])
set(MCS_DEPFILE       [==[@_depfile@]==])
set(MCS_RESPONSE      [==[@_response@]==])
set(MCS_SOURCES_INPUTS [==[@_sources_inputs@]==])
set(MCS_GENSOURCES    [==[@_gensources@]==])
set(MCS_PLATFORMS_DIR [==[@MONO_MANAGED_PLATFORMS_DIR@]==])
set(MCS_LIBRARY       [==[@A_NAME@]==])
set(MCS_PLATFORM      [==[@_platform@]==])
set(MCS_PROFILE       [==[@_prof@]==])
set(MCS_CSC           [==[@_csc@]==])
set(MCS_CSC_FLAGS     [==[@_flags@]==])
set(MCS_CSC_ENV       [==[@_csc_env@]==])
set(MCS_TOOL_ENV      [==[@_tool_env@]==])
set(MCS_BUILT_SOURCES [==[@_built@]==])
set(MCS_SN            [==[@_sn@]==])
set(MCS_SNK           [==[@_snk@]==])
set(MCS_STRING_REPLACER       [==[@_string_replacer@]==])
set(MCS_STRING_REPLACER_FLAGS [==[@A_STRING_REPLACER_FLAGS@]==])
]] @ONLY)

  add_custom_command(
    OUTPUT "${_out}"
    COMMAND "${CMAKE_COMMAND}" -D "SETTINGS=${_settings}"
            -P "${CMAKE_SOURCE_DIR}/cmake/MonoCompileAssembly.cmake"
    # Generated sources are not listed here: they are produced in another
    # directory, so they are reached through the target that owns them, which
    # the caller passes in DEPENDS.
    DEPENDS ${_sources_inputs} ${_refdeps} ${A_DEPENDS} "${_settings}"
    DEPFILE "${_depfile}"
    COMMENT "CSC [${_profile}] ${_outname}"
    VERBATIM)

  add_custom_target(${_target} DEPENDS "${_out}")
  set_property(GLOBAL APPEND PROPERTY MONO_MANAGED_PROFILE_TARGETS_${_profile} ${_target})
  set_property(GLOBAL PROPERTY MONO_MANAGED_OUTPUT_${_target} "${_out}")
endmacro()

# ---------------------------------------------------------------------------
# Generated parsers
# ---------------------------------------------------------------------------
# jay takes the grammar as an argument but the output skeleton on stdin, and a
# custom command cannot redirect, so the invocation goes through a shell.
#
#   mono_jay_parser(TARGET <name> OUTPUT <file> GRAMMAR <file.jay>
#                   [FLAGS <flag>...] [SKELETON <file>])
#
# TARGET is not decoration.  An OUTPUT custom command only produces a build
# rule for targets in its own directory, and the compile that consumes this
# file is created later, in mcs/'s scope -- so the generated source needs a
# target here to own it, which the assembly then names in DEPENDS.
function(mono_jay_parser)
  cmake_parse_arguments(J "" "TARGET;OUTPUT;GRAMMAR;SKELETON" "FLAGS" ${ARGN})
  if(NOT J_SKELETON)
    set(J_SKELETON "${MONO_MCS_TOPDIR}/jay/skeleton.cs")
  endif()
  get_filename_component(_grammar "${J_GRAMMAR}" ABSOLUTE)
  get_filename_component(_name "${J_OUTPUT}" NAME)
  string(JOIN " " _flags ${J_FLAGS})
  # One shell word, so sh sees a single script rather than a shifted argv.
  add_custom_command(
    OUTPUT "${J_OUTPUT}"
    COMMAND sh -c
      "'$<TARGET_FILE:jay>' ${_flags} -o '${J_OUTPUT}' '${_grammar}' < '${J_SKELETON}'"
    DEPENDS jay "${_grammar}" "${J_SKELETON}"
    COMMENT "JAY     ${_name}"
    VERBATIM)
  add_custom_target(${J_TARGET} DEPENDS "${J_OUTPUT}")
endfunction()

# ---------------------------------------------------------------------------
# IL modules
# ---------------------------------------------------------------------------
# Assembles a .il file with ilasm.  corlib uses this for the one construct csc
# cannot express, and cil-stringreplacer then splices the result into the
# freshly compiled mscorlib.dll.
#
#   mono_add_il_module(TARGET <name> OUTPUT <file> SOURCE <file.il>
#                      PROFILE <profile> [FLAGS <flag>...])
#
# TARGET exists for the same reason it does on mono_jay_parser(): the compile
# that consumes this file is created in another directory.
function(mono_add_il_module)
  cmake_parse_arguments(I "" "TARGET;OUTPUT;SOURCE;PROFILE" "FLAGS" ${ARGN})
  get_filename_component(_src "${I_SOURCE}" ABSOLUTE)
  get_filename_component(_name "${I_OUTPUT}" NAME)
  _mono_tool_command(_ilasm ${I_PROFILE} ilasm.exe)
  _mono_tool_env(_env ${I_PROFILE})
  set(_cmd ${_ilasm})
  if(_env)
    set(_cmd "${CMAKE_COMMAND}" -E env ${_env} ${_ilasm})
  endif()
  get_property(_ilasm_target GLOBAL PROPERTY MONO_MANAGED_PROVIDER_build/ilasm)
  _mono_tool_depends(_rt ${I_PROFILE})
  add_custom_command(
    OUTPUT "${I_OUTPUT}"
    COMMAND ${_cmd} "${_src}" ${I_FLAGS} "/out:${I_OUTPUT}"
    DEPENDS "${_src}" ${_ilasm_target} ${_rt}
    COMMENT "ILASM   ${_name}"
    VERBATIM)
  add_custom_target(${I_TARGET} DEPENDS "${I_OUTPUT}")
endfunction()
