mono_declare_managed(
  NAME       System.Numerics.Vectors.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Numerics
  FLAGS      -unsafe
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
