# Expands the .sources files every library compile is driven from, so it has to
# exist before any of them.  It is a program, which means its own source list is
# the checked-in .sources read verbatim -- that is what lets it bootstrap.
#
# Compiled against the checked-in reference assemblies rather than the BCL this
# build produces, and run out of tmp/, for the reasons in MonoManaged.cmake.
mono_declare_managed(
  PROGRAM
  NAME       gensources.exe
  PROFILES   build
  REFS       System System.Core
  TARGET_NET_REFERENCE v4.7
  INTERMEDIATE
  NO_INSTALL)
