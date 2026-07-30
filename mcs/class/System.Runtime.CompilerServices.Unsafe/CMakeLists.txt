# This assembly has no C# at all.  It is ilasm'd straight from the same
# Unsafe.il corlib splices into mscorlib, plus a header, because the methods it
# exposes have no C# spelling.  It is not installed: it exists so the class
# libraries and the debugger tests can reference it.
mono_add_il_module(
  TARGET  system-runtime-compilerservices-unsafe-net_4_x
  OUTPUT  "${MONO_MANAGED_LIBDIR}/${MONO_PROFILE_net_4_x_DIRECTORY}/System.Runtime.CompilerServices.Unsafe.dll"
  SOURCES AssemblyInfo.il ../corlib/System.Runtime.CompilerServices/Unsafe.il
  PROFILE net_4_x
  FLAGS   /dll)

mono_register_managed_provider(
  PROFILE net_4_x
  NAME    System.Runtime.CompilerServices.Unsafe.dll
  TARGET  system-runtime-compilerservices-unsafe-net_4_x)
