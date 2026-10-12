Unity 2019.4 Mono runtime with ETW and VTune profiling support
==============================================================

This replaces the Mono runtime of a KSP 1.x install (Unity 2019.4) with a
build of Unity's own 2019.4 Mono that can also report JIT-compiled code to
Windows profilers. The class libraries are unchanged and stay as they are.

Install
-------

1. Back up MonoBleedingEdge\EmbedRuntime\mono-2.0-bdwgc.dll in the KSP install.
2. Unpack this zip into the KSP install root, the directory that holds
   KSP_x64.exe, and let it overwrite that file.

To uninstall, put the backed-up mono-2.0-bdwgc.dll back.

With no profiling option set, the game runs as it does with Unity's runtime.

Profiling with ETW (Superluminal, PerfView, WPA, ultra)
-------------------------------------------------------

Start KSP with MONO_ENV_OPTIONS set:

    set MONO_ENV_OPTIONS=--profile=etw
    KSP_x64.exe

Then record with a tool that enables the .NET runtime events. Superluminal and
PerfView do this by default. With WPR, add the .NET profile:

    wpr -start CPU -start DotNET

Recording can start after the game is running: methods compiled before the
recording began are reported when it starts.

--profile=etw makes the runtime keep IL offset maps for every method it
compiles, which costs memory and compile time while the game runs. Leave the
variable unset when you are not profiling.

Profiling with Intel VTune
--------------------------

VTune must launch the game. Attaching to a running KSP does not name
JIT-compiled methods.

1. Set the analysis target to "Launch Application" with KSP_x64.exe.
2. Add the environment variable MONO_ENV_OPTIONS=--profile=ittapi to the
   target.

mono-2.0-bdwgc.pdb
------------------

Keep it next to the DLL. Profilers and debuggers read it to name the
runtime's own functions.
