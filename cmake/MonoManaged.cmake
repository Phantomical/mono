# The class-library build.  Every assembly under mcs/ is a normal CMake custom
# command, so the whole tree -- native and managed -- lives in one dependency
# graph, one job pool, and one incremental rebuild.
#
# Two things about the shape of this file are worth knowing up front.
#
# Declarations are collected, not built.  A directory's CMakeLists.txt calls
# mono_declare_managed() and nothing happens; mcs/CMakeLists.txt calls
# mono_managed_materialize() at the end and every target appears at once.  The
# reason is that references are written as bare assembly names -- `REFS System
# System.XML` -- and a name cannot be resolved to the target that produces it
# until every directory has been read.  Collecting first is also what makes the
# resulting graph the real dependency DAG.
#
# The same directory builds in several profiles.  A declaration therefore names
# the profiles it participates in, and produces one target per profile, named
# mcs-<profile>-<assembly>.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/MonoManagedTests.cmake")

# ---------------------------------------------------------------------------
# Where things go
# ---------------------------------------------------------------------------
# Output lands in the build tree, so two build directories over one checkout
# stay independent and a build leaves the source tree clean.
set(MONO_MANAGED_ROOT "${CMAKE_BINARY_DIR}/mcs")
set(MONO_MANAGED_LIBDIR "${MONO_MCS_LIBDIR}")
set(MONO_MANAGED_DEPSDIR "${MONO_MANAGED_ROOT}/deps")

# The host-platform prefixes a .sources file name may carry.  This and
# MONO_MANAGED_PROFILES below are the two name sets gensources works from, and
# it takes them on its command line; they are the build's to define.
set(MONO_MANAGED_PLATFORM_NAMES linux macos unix win32)

file(MAKE_DIRECTORY "${MONO_MANAGED_LIBDIR}" "${MONO_MANAGED_DEPSDIR}")

# Ninja's depfile parser splits a path at a backtick and no escaping recovers
# it, so the generic-arity file names this tree uses -- `Nullable`1.cs` and its
# 36 siblings -- cannot ride the depfile: ninja would invent two phony inputs
# that never exist and rebuild the assembly on every run.  The driver leaves
# them out of the depfile; they become configure-time dependencies instead,
# which costs a rebuild only when one of them is added or removed.
file(GLOB_RECURSE MONO_MANAGED_ODD_SOURCES "${MONO_MCS_TOPDIR}/class/*`*.cs")

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

# Fail at configure time rather than a thousand compile commands later.
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
# The Linux profile set.
#
# DIRECTORY is the on-disk name; ALIAS is the unsuffixed name everything else
# in the tree spells, and is created as a symlink beside it.

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
  OPTIONS   WARN_AS_ERROR ENABLE_GSS DEBUG FACADES TESTS)

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

# Flags every compile gets.
set(MONO_MANAGED_COMMON_FLAGS
    /codepage:65001 /nologo /noconfig /deterministic /langversion:latest
    -optimize)                              # -optimize is config.make's BCL_OPTIMIZE
set(MONO_MANAGED_DEBUG_FLAGS /debug:portable)

function(mono_profile_dir out profile)
  set(${out} "${MONO_MANAGED_LIBDIR}/${MONO_PROFILE_${profile}_DIRECTORY}" PARENT_SCOPE)
endfunction()

# The alias directories, created before anything compiles.
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
# Resolves a path a declaration named.  Relative is against the declaring
# directory, as in the makefiles.  <topdir>/ stands for the mcs root: a few
# defaults library.make anchors there (LIBRARY_SNK) reach directories at
# varying depths, and spelling it this way keeps the generated CMakeLists free
# of any checkout-specific path.
macro(_mono_resolve_path out path base)
  if("${path}" MATCHES "^<topdir>/(.*)$")
    set(${out} "${MONO_MCS_TOPDIR}/${CMAKE_MATCH_1}")
  elseif(IS_ABSOLUTE "${path}")
    set(${out} "${path}")
  else()
    set(${out} "${base}/${path}")
  endif()
endmacro()

# Rewrites the -resource: flags naming a .resx-generated file, which now lives
# in the build tree rather than beside its .resx.  An explicit resource id is
# added where the makefile relied on the default, which csc derives from the
# file name -- moving the file must not rename the resource.
function(_mono_rewrite_resource_flags out flags resx resdir)
  set(_result "")
  foreach(_f IN LISTS flags)
    if(_f MATCHES "^([-/]resource:)([^,]+)(,?.*)$")
      set(_pfx "${CMAKE_MATCH_1}")
      set(_rpath "${CMAKE_MATCH_2}")
      set(_rrest "${CMAKE_MATCH_3}")
      if("${_rpath}" IN_LIST resx)
        if(_rrest STREQUAL "")
          get_filename_component(_rid "${_rpath}" NAME)
          set(_rrest ",${_rid}")
        endif()
        set(_f "${_pfx}${resdir}/${_rpath}${_rrest}")
      endif()
    endif()
    list(APPEND _result "${_f}")
  endforeach()
  set(${out} "${_result}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# One call per assembly per directory; PROFILES says which profiles build it.
# The argument names follow the makefile variables they replace closely enough
# to diff against them: NAME is LIBRARY/PROGRAM, OUTPUT_NAME is LIBRARY_NAME,
# REFS is LIB_REFS, FLAGS is LIB_MCS_FLAGS.
# Every field a declaration carries.  One list, used to stash the declaration
# and to read it back -- they drifted apart once already, and a field missing
# from the read-back side is silently empty at materialize time rather than an
# error.
set(MONO_MANAGED_FIELDS
    NAME OUTPUT_NAME SUBDIR KEYFILE SNK PACKAGE INSTALL_DIR
    TARGET_NET_REFERENCE SOURCES_FILE PROFILES REFS API_BIN_REFS FLAGS
    BUILT_SOURCES DEPENDS RESOURCES STRING_REPLACER_FLAGS ENV SOURCES
    RESX RESGEN_FLAGS RESOURCE_DEFS
    TEST_REFS TEST_FLAGS TEST_RESOURCES TEST_EXCLUDES TEST_RUNNER_FILES
    XTEST_REFS XTEST_FLAGS
    TEST_CONFIG_GLOBAL TEST_CONFIG_RUNTIME
    PROGRAM NO_SIGN NO_INSTALL NO_DEBUG INTERMEDIATE NO_DEFAULT_REFERENCES NO_TEST
    XTEST_REMOTE_EXECUTOR)

function(mono_declare_managed)
  cmake_parse_arguments(A
    "PROGRAM;NO_SIGN;NO_INSTALL;NO_DEBUG;INTERMEDIATE;NO_DEFAULT_REFERENCES;NO_TEST;XTEST_REMOTE_EXECUTOR"
    "NAME;OUTPUT_NAME;SUBDIR;KEYFILE;SNK;PACKAGE;INSTALL_DIR;TARGET_NET_REFERENCE;SOURCES_FILE;TEST_CONFIG_GLOBAL;TEST_CONFIG_RUNTIME"
    "PROFILES;REFS;API_BIN_REFS;FLAGS;BUILT_SOURCES;DEPENDS;RESOURCES;STRING_REPLACER_FLAGS;ENV;SOURCES;RESX;RESGEN_FLAGS;RESOURCE_DEFS;TEST_REFS;TEST_FLAGS;TEST_RESOURCES;TEST_EXCLUDES;TEST_RUNNER_FILES;XTEST_REFS;XTEST_FLAGS"
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

  foreach(_f IN LISTS MONO_MANAGED_FIELDS)
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
# is derived from the build directory so that two build trees do not share a
# server: it holds the tree's class/lib/build assemblies open through
# MONO_PATH.
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
    # The tools run on the bootstrap profile, with MONO_PATH pointing at it, so
    # the whole of it has to be there before any of them start.
    set(_d mono-${MONO_DEFAULT_GC_SUFFIX} mono-build-config mcs-build)
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

# Registers the install-time gacutil call for one assembly.  LIBRARY_PACKAGE
# defaults to the profile's framework version and `none` means GAC only, with
# no symlink under lib/mono/<package>.
function(_mono_gac_install lib profile package)
  if(NOT package)
    set(package "${MONO_PROFILE_${profile}_FRAMEWORK_VERSION}")
  endif()
  if(package STREQUAL "none")
    set(package "")
  endif()
  mono_profile_dir(_builddir build)
  # CMAKE_INSTALL_PREFIX is left for install time so that
  # `cmake --install --prefix` still reaches the right root.
  install(CODE "
set(MONO_GAC_RUNTIME [==[${CMAKE_BINARY_DIR}/runtime/mono-wrapper]==])
set(MONO_GAC_TOOL [==[${_builddir}/gacutil.exe]==])
set(MONO_GAC_MONO_PATH [==[${_builddir}]==])
set(MONO_GAC_LIBDIR \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}\")
set(MONO_GAC_LIB [==[${lib}]==])
set(MONO_GAC_PACKAGE [==[${package}]==])
include([==[${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MonoGacInstall.cmake]==])
" COMPONENT mcs)
endfunction()

# For the one assembly that is installed twice: mono-service.exe is a program
# in lib/mono/4.5 *and* a GAC entry, so its directory asks for the second half
# explicitly.  PACKAGE defaults to `none`, i.e. no lib/mono/<package> symlink.
#
#   mono_gac_install(PROFILE <profile> NAME <file> [PACKAGE <package>])
function(mono_gac_install)
  cmake_parse_arguments(G "" "PROFILE;NAME;PACKAGE" "" ${ARGN})
  if(NOT G_PACKAGE)
    set(G_PACKAGE none)
  endif()
  mono_profile_dir(_dir ${G_PROFILE})
  _mono_gac_install("${_dir}/${G_NAME}" "${G_PROFILE}" "${G_PACKAGE}")
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
    get_property(_outname GLOBAL PROPERTY ${_id}_OUTPUT_NAME)
    get_property(_subdir  GLOBAL PROPERTY ${_id}_SUBDIR)
    get_property(_profs   GLOBAL PROPERTY ${_id}_PROFILES)
    # Keyed on the file that lands in the profile directory, not on the
    # declaration's name: LIB_REFS spells the output, and Microsoft.Build.Tasks
    # ships as Microsoft.Build.Tasks.v4.0.dll.
    if(_outname)
      _mono_stem(_stem "${_outname}")
    else()
      _mono_stem(_stem "${_name}")
    endif()
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

  # The test assemblies are part of `all`, like every other test input: a
  # finished build has every suite's assemblies on disk, so running ctest
  # never builds anything.  The per-suite aggregates under these still exist
  # for building one suite's inputs by hand.
  add_custom_target(mcs-tests ALL)
  add_custom_target(mcs-xunit-tests ALL)

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
  foreach(_f IN LISTS MONO_MANAGED_FIELDS ITEMS DIR)
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
      set(_reffile "${_rdir}/${_refname}.dll")
    else()
      set(_reffile "${_pdir}/${_refname}.dll")
    endif()
    list(APPEND _refflags "-r:${_alias}${_reffile}")
    get_property(_provider GLOBAL PROPERTY MONO_MANAGED_PROVIDER_${_refprofile}/${_refname})
    if(_provider)
      # Both the target and the file.  A DEPENDS on a target name alone is an
      # order-only edge in Ninja: it would sequence the two compiles but never
      # recompile this assembly when the one it references changes.
      list(APPEND _refdeps "${_provider}" "${_reffile}")
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
  # A flag may name a file by <topdir>-relative path -- the NuGet task's
  # keyfile lives outside mcs entirely -- for the same reason KEYFILE may.
  string(REPLACE "<topdir>" "${MONO_MCS_TOPDIR}" _aflags "${A_FLAGS}")
  list(APPEND _flags ${_aflags} ${_refflags})
  if(A_KEYFILE)
    _mono_resolve_path(_keyfile "${A_KEYFILE}" "${A_DIR}")
    list(APPEND _flags "/keyfile:${_keyfile}")
  endif()
  if(A_PROGRAM)
    list(APPEND _flags -target:exe)
  else()
    list(APPEND _flags -target:library)
  endif()

  # -- generated resources -------------------------------------------------
  # RESX_RESOURCES in the makefiles: a .resx compiled by resgen and embedded.
  # They were generated beside the .resx and named by a relative path, which
  # worked only because csc ran with the source directory as its cwd.  Here
  # they go into the build tree, so the -resource: flag that names one has to
  # be rewritten to point at it.
  # Checked-in files embedded as-is.  They cannot be produced, so they are here
  # only so that editing one rebuilds the assembly.
  set(_resdeps "")
  foreach(_r IN LISTS A_RESOURCES)
    if(NOT IS_ABSOLUTE "${_r}")
      set(_r "${A_DIR}/${_r}")
    endif()
    list(APPEND _resdeps "${_r}")
  endforeach()

  if(A_RESX OR A_RESOURCE_DEFS)
    set(_resdir "${MONO_MANAGED_DEPSDIR}/res/${_target}")
    _mono_tool_command(_resgen ${_profile} resgen.exe)
    _mono_tool_env(_resgen_env ${_profile})
    _mono_tool_depends(_rgdeps ${_profile})
    get_property(_rgprov GLOBAL PROPERTY MONO_MANAGED_PROVIDER_build/resgen)
    if(_rgprov)
      list(APPEND _rgdeps "${_rgprov}")
    endif()
  endif()

  # An `<id>,<file>` pair names the resource itself rather than letting csc
  # derive it from the file name, so the input can be a .txt, or a .resx off in
  # external/, and still be embedded under the name the assembly looks up.
  foreach(_pair IN LISTS A_RESOURCE_DEFS)
    if(NOT _pair MATCHES "^([^,]+),(.+)$")
      message(FATAL_ERROR
              "mono_declare_managed(${A_NAME}): RESOURCE_DEFS wants <id>,<file>, got '${_pair}'")
    endif()
    set(_rid "${CMAKE_MATCH_1}")
    set(_rin "${CMAKE_MATCH_2}")
    if(NOT IS_ABSOLUTE "${_rin}")
      set(_rin "${A_DIR}/${_rin}")
    endif()
    add_custom_command(
      OUTPUT "${_resdir}/${_rid}.resources"
      COMMAND "${CMAKE_COMMAND}"
              -D "RESGEN=${_resgen}" -D "RESGEN_ENV=${_resgen_env}"
              -D "RESGEN_FLAGS=${A_RESGEN_FLAGS}"
              -D "INPUT=${_rin}"
              -D "OUTPUT=${_resdir}/${_rid}.resources"
              -D "WORKDIR=${A_DIR}"
              -P "${CMAKE_SOURCE_DIR}/cmake/MonoResgen.cmake"
      DEPENDS "${_rin}" ${_rgdeps}
      COMMENT "RESGEN [${_profile}] ${_rid}.resources"
      VERBATIM)
    list(APPEND _resdeps "${_resdir}/${_rid}.resources")
    list(APPEND _flags "-resource:${_resdir}/${_rid}.resources,${_rid}.resources")
  endforeach()

  if(A_RESX)
    foreach(_r IN LISTS A_RESX)
      string(REGEX REPLACE "\\.resources$" ".resx" _resx "${_r}")
      add_custom_command(
        OUTPUT "${_resdir}/${_r}"
        COMMAND "${CMAKE_COMMAND}"
                -D "RESGEN=${_resgen}" -D "RESGEN_ENV=${_resgen_env}"
                -D "RESGEN_FLAGS=${A_RESGEN_FLAGS}"
                -D "INPUT=${A_DIR}/${_resx}"
                -D "OUTPUT=${_resdir}/${_r}"
                -D "PREBUILT=${A_DIR}/${_r}.prebuilt"
                -D "WORKDIR=${A_DIR}"
                -P "${CMAKE_SOURCE_DIR}/cmake/MonoResgen.cmake"
        DEPENDS "${A_DIR}/${_resx}" ${_rgdeps}
        COMMENT "RESGEN [${_profile}] ${_r}"
        VERBATIM)
      list(APPEND _resdeps "${_resdir}/${_r}")
    endforeach()

    # A checked-in @response file can name the resources instead of the
    # makefile doing it -- System.Windows.Forms does -- so it needs the same
    # rewrite, on a copy in the build tree.
    set(_newflags "")
    foreach(_f IN LISTS _flags)
      if(_f MATCHES "^@(.+)$")
        set(_rsp "${CMAKE_MATCH_1}")
        if(NOT IS_ABSOLUTE "${_rsp}")
          set(_rsp "${A_DIR}/${_rsp}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rsp}")
        file(STRINGS "${_rsp}" _rsplines)
        _mono_rewrite_resource_flags(_rsplines "${_rsplines}" "${A_RESX}" "${_resdir}")
        get_filename_component(_rspname "${_rsp}" NAME)
        string(JOIN "\n" _rsptext ${_rsplines})
        # file(GENERATE) rather than file(WRITE): it leaves the file alone when
        # the content has not changed, so reconfiguring does not rebuild.
        file(GENERATE OUTPUT "${_resdir}/${_rspname}" CONTENT "${_rsptext}\n")
        set(_f "@${_resdir}/${_rspname}")
      endif()
      list(APPEND _newflags "${_f}")
    endforeach()
    _mono_rewrite_resource_flags(_flags "${_newflags}" "${A_RESX}" "${_resdir}")
  endif()

  # The sources ninja cannot name in a depfile, restricted to the ones this
  # assembly could plausibly compile: its own directory, plus the shared
  # referencesource pool that .sources files across the tree include from.
  set(_odd "")
  foreach(_o IN LISTS MONO_MANAGED_ODD_SOURCES)
    string(FIND "${_o}" "${A_DIR}/" _pos)
    if(_pos EQUAL 0)
      list(APPEND _odd "${_o}")
    else()
      string(FIND "${_o}" "${MONO_MCS_TOPDIR}/class/referencesource/" _pos)
      if(_pos EQUAL 0)
        list(APPEND _odd "${_o}")
      endif()
    endif()
  endforeach()

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
      _mono_resolve_path(_snk "${A_SNK}" "${A_DIR}")
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
set(MCS_PLATFORM_NAMES [==[@MONO_MANAGED_PLATFORM_NAMES@]==])
set(MCS_PROFILE_NAMES [==[@MONO_MANAGED_PROFILES@]==])
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
    DEPENDS ${_sources_inputs} ${_refdeps} ${_resdeps} ${A_DEPENDS} ${_odd}
            "${_settings}"
    DEPFILE "${_depfile}"
    COMMENT "CSC [${_profile}] ${_outname}"
    VERBATIM)

  # A checked-in <assembly>.config travels with the assembly and is installed
  # beside it.  executable.make found it with $(wildcard), preferring the
  # profile-specific spelling.
  set(_extra "")
  set(_cfgsrc "")
  if(EXISTS "${A_DIR}/${_outname}.config.${_profile}")
    set(_cfgsrc "${A_DIR}/${_outname}.config.${_profile}")
  elseif(EXISTS "${A_DIR}/${_outname}.config")
    set(_cfgsrc "${A_DIR}/${_outname}.config")
  endif()
  if(_cfgsrc)
    add_custom_command(
      OUTPUT "${_out}.config"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "${_cfgsrc}" "${_out}.config"
      DEPENDS "${_cfgsrc}"
      COMMENT "COPY [${_profile}] ${_outname}.config"
      VERBATIM)
    set(_extra "${_out}.config")
  endif()

  add_custom_target(${_target} DEPENDS "${_out}" ${_extra})
  set_property(GLOBAL APPEND PROPERTY MONO_MANAGED_PROFILE_TARGETS_${_profile} ${_target})
  set_property(GLOBAL PROPERTY MONO_MANAGED_OUTPUT_${_target} "${_out}")

  # -- install -------------------------------------------------------------
  # Three shapes, exactly as library.make and executable.make had them: a
  # library with LIBRARY_INSTALL_DIR is copied there, a program goes to
  # PROGRAM_INSTALL_DIR (default lib/mono/<framework version>), and a library
  # without one goes through gacutil.
  if(NOT A_NO_INSTALL AND NOT MONO_PROFILE_${_profile}_NO_INSTALL)
    string(REGEX REPLACE "\\.(dll|exe)$" ".pdb" _pdb "${_out}")
    string(REPLACE "<libdir>" "${CMAKE_INSTALL_LIBDIR}" _dest "${A_INSTALL_DIR}")
    string(REPLACE "<sysconfdir>" "${CMAKE_INSTALL_SYSCONFDIR}" _dest "${_dest}")
    if(NOT _dest AND A_PROGRAM)
      set(_dest
          "${CMAKE_INSTALL_LIBDIR}/mono/${MONO_PROFILE_${_profile}_FRAMEWORK_VERSION}")
    endif()
    if(_dest)
      install(PROGRAMS "${_out}" DESTINATION "${_dest}" COMPONENT mcs)
      install(FILES "${_pdb}" DESTINATION "${_dest}" COMPONENT mcs OPTIONAL)
      if(_cfgsrc)
        install(FILES "${_out}.config" DESTINATION "${_dest}" COMPONENT mcs)
      endif()
    else()
      _mono_gac_install("${_out}" "${_profile}" "${A_PACKAGE}")
    endif()
  endif()

  # -- tests ---------------------------------------------------------------
  # A suite exists when the directory has a .sources file naming one, which is
  # the same wildcard test the makefiles used.
  if(MONO_PROFILE_${_profile}_TESTS AND NOT A_NO_TEST AND NOT A_PROGRAM)
    # A test assembly inherits the library's flags, so it names the same
    # generated resources and needs the same rewriting.
    set(_tflags ${A_TEST_FLAGS})
    set(_xflags ${A_XTEST_FLAGS})
    if(A_RESX)
      _mono_rewrite_resource_flags(_tflags "${_tflags}" "${A_RESX}" "${_resdir}")
      _mono_rewrite_resource_flags(_xflags "${_xflags}" "${A_RESX}" "${_resdir}")
    endif()
    mono_test_fixture_dir(_fixdir ${_profile} "${_outname}")
    _mono_rewrite_fixture_flags(_tflags "${_tflags}" "${A_DIR}" "${_fixdir}")
    _mono_rewrite_fixture_flags(_xflags "${_xflags}" "${A_DIR}" "${_fixdir}")

    _mono_test_sources(_tsrc _tstem nunit ${_profile} "${A_DIR}" "${_outname}" "${A_NAME}")
    if(_tsrc)
      _mono_add_managed_test(nunit ${_profile} "${A_DIR}" ${_target} "${_outname}"
                             "${_tstem}" "${_tsrc}"
                             REFS ${A_TEST_REFS} FLAGS ${_tflags}
                             LIB_REFFLAGS ${_refflags}
                             RESOURCES ${A_TEST_RESOURCES}
                             EXCLUDES ${A_TEST_EXCLUDES}
                             RUNNER_FILES ${A_TEST_RUNNER_FILES}
                             GLOBAL_CONFIG "${A_TEST_CONFIG_GLOBAL}"
                             RUNTIME_CONFIG "${A_TEST_CONFIG_RUNTIME}"
                             DEPENDS ${A_DEPENDS})
    endif()
    _mono_test_sources(_xsrc _xstem xunit ${_profile} "${A_DIR}" "${_outname}" "${A_NAME}")
    if(_xsrc)
      _mono_add_managed_test(xunit ${_profile} "${A_DIR}" ${_target} "${_outname}"
                             "${_xstem}" "${_xsrc}"
                             REFS ${A_XTEST_REFS} FLAGS ${_xflags}
                             LIB_REFFLAGS ${_refflags}
                             REMOTE_EXECUTOR ${A_XTEST_REMOTE_EXECUTOR}
                             DEPENDS ${A_DEPENDS})
    endif()
  endif()
endmacro()

# ---------------------------------------------------------------------------
# Sources generated by a managed tool
# ---------------------------------------------------------------------------
# A handful of directories run something this build produced to write a .cs --
# System.Web's culevel, RabbitMQ's Apigen.
#
#   mono_generated_source(TARGET <name> OUTPUT <file> PROFILE <profile>
#                         TOOL <exe> [TOOL_PROFILE <profile>]
#                         [ARGS <arg>...] [DEPENDS <file>...])
#
# TOOL_PROFILE is which profile's copy of the tool to run and defaults to the
# bootstrap one, matching BUILD_TOOLS_PROFILE.  The tool is depended on by
# target name rather than through the provider map, because the per-directory
# files that call this run before mono_managed_materialize() has built it.
function(mono_generated_source)
  cmake_parse_arguments(G "" "TARGET;OUTPUT;PROFILE;TOOL;TOOL_PROFILE"
                        "ARGS;DEPENDS" ${ARGN})
  if(NOT G_TOOL_PROFILE)
    set(G_TOOL_PROFILE build)
  endif()
  mono_profile_dir(_tooldir ${G_TOOL_PROFILE})
  _mono_tool_depends(_deps ${G_PROFILE})
  _mono_target_name(_tooltgt ${G_TOOL_PROFILE} "" "${G_TOOL}")
  list(APPEND _deps ${_tooltgt})
  get_filename_component(_name "${G_OUTPUT}" NAME)
  get_filename_component(_outdir "${G_OUTPUT}" DIRECTORY)
  add_custom_command(
    OUTPUT "${G_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_outdir}"
    COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_tooldir}"
            "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" "${_tooldir}/${G_TOOL}"
            ${G_ARGS}
    DEPENDS ${G_DEPENDS} ${_deps}
    COMMENT "GEN     [${G_PROFILE}] ${_name}"
    VERBATIM)
  add_custom_target(${G_TARGET} DEPENDS "${G_OUTPUT}")
endfunction()

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
#   mono_add_il_module(TARGET <name> OUTPUT <file> SOURCES <file.il>...
#                      PROFILE <profile> [FLAGS <flag>...])
#
# TARGET exists for the same reason it does on mono_jay_parser(): the compile
# that consumes this file is created in another directory.
function(mono_add_il_module)
  cmake_parse_arguments(I "" "TARGET;OUTPUT;PROFILE" "SOURCES;FLAGS" ${ARGN})
  set(_srcs "")
  foreach(_s IN LISTS I_SOURCES)
    get_filename_component(_s "${_s}" ABSOLUTE)
    list(APPEND _srcs "${_s}")
  endforeach()
  get_filename_component(_name "${I_OUTPUT}" NAME)
  _mono_tool_command(_ilasm ${I_PROFILE} ilasm.exe)
  _mono_tool_env(_env ${I_PROFILE})
  set(_cmd ${_ilasm})
  if(_env)
    set(_cmd "${CMAKE_COMMAND}" -E env ${_env} ${_ilasm})
  endif()
  # By target name, not through the provider map: this runs while the
  # per-directory files are being read, before materialize has filled it in.
  _mono_target_name(_ilasm_target build "" ilasm.exe)
  _mono_tool_depends(_rt ${I_PROFILE})
  add_custom_command(
    OUTPUT "${I_OUTPUT}"
    # /quiet drops the per-file banner and the success line; warnings and errors
    # are printed either way.
    COMMAND ${_cmd} /quiet ${_srcs} ${I_FLAGS} "/out:${I_OUTPUT}"
    DEPENDS ${_srcs} ${_ilasm_target} ${_rt}
    COMMENT "ILASM   [${I_PROFILE}] ${_name}"
    VERBATIM)
  add_custom_target(${I_TARGET} DEPENDS "${I_OUTPUT}")
endfunction()

# Declares that <target> produces the assembly <name> in <profile>, for the two
# directories that build one without going through mono_declare_managed().
# Without this a reference to it compiles against the right path but carries no
# dependency edge, so the build races.
function(mono_register_managed_provider)
  cmake_parse_arguments(R "" "PROFILE;NAME;TARGET" "" ${ARGN})
  _mono_stem(_stem "${R_NAME}")
  set_property(GLOBAL PROPERTY MONO_MANAGED_PROVIDER_${R_PROFILE}/${_stem}
               "${R_TARGET}")
  set_property(GLOBAL APPEND PROPERTY
               MONO_MANAGED_PROFILE_TARGETS_${R_PROFILE} ${R_TARGET})
endfunction()
