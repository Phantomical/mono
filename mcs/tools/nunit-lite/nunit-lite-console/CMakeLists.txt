# The runner every class-library test assembly is executed by.
mono_declare_managed(
  PROGRAM
  NAME       nunit-lite-console.exe
  PROFILES   net_4_x
  REFS       nunitlite
  NO_INSTALL)
