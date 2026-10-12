Unity 2019.4 Mono runtime
=========================

This replaces the Mono runtime of a KSP 1.x install (Unity 2019.4) with a
build of Unity's own 2019.4 Mono. The class libraries are unchanged and stay
as they are.

The ETW and VTune profilers in the Windows build are not part of this one.

Install
-------

1. Back up MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so in the KSP install.
2. Unpack this zip into the KSP install root and let it overwrite that file.

To uninstall, put the backed-up libmonobdwgc-2.0.so back.

The runtime needs glibc 2.34 or later.

libmonobdwgc-2.0.so.debug holds the debug information split out of the
library. Keep it next to the library so gdb and perf can name the runtime's
own functions.
