mono_declare_managed(
  NAME       System.IO.Compression.FileSystem.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.IO.Compression
  FLAGS      -unsafe -nowarn:436
  TEST_REFS  System System.Core System.IO.Compression
  TEST_FLAGS -resource:Test/resources/foo/foo.txt,Test/resources/foo/foo.txt -resource:Test/resources/foo/bar.txt,Test/resources/foo/bar.txt -resource:Test/resources/foo/foobar/foo.txt,Test/resources/foo/foobar/foo.txt -resource:Test/resources/foo/foobar/bar.txt,Test/resources/foo/foobar/bar.txt
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
