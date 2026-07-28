# Converted from mcs/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       mcs.exe
  PROFILES   net_4_x
  REFS       System.Core System.Xml System
  FLAGS      -d:STATIC,NO_SYMBOL_WRITER,NO_AUTHENTICODE -d:MONO_FEATURE_THREAD_ABORT -d:MONO_FEATURE_PROCESS_START
)
