# The class libraries.
#
# Each subdirectory below declares the assemblies it produces; nothing is built
# until mono_managed_materialize() runs at the end, because a reference is a
# bare assembly name and cannot be resolved to a target before every directory
# has been read.  See cmake/MonoManaged.cmake.

# --- the bootstrap profile -------------------------------------------------
# Order here is irrelevant -- the dependency graph comes from the declarations,
# not from the order directories are added.  It follows the old build_SUBDIRS
# list only so the two can be diffed.
add_subdirectory(build)
add_subdirectory(jay)
add_subdirectory(tools/gensources)
add_subdirectory(class/Mono.Cecil)
add_subdirectory(tools/cil-stringreplacer)
add_subdirectory(class/PEAPI)
add_subdirectory(ilasm)
add_subdirectory(class/corlib)

mono_managed_materialize()

# `mcs` keeps its old meaning: build everything the enabled profiles produce.
add_custom_target(mcs ALL)
foreach(_p IN LISTS MONO_MANAGED_PROFILES)
  add_dependencies(mcs mcs-${_p})
endforeach()
