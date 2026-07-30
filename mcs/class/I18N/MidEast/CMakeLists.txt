mono_declare_managed(
  NAME       I18N.MidEast.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       I18N
  FLAGS      /unsafe
  TEST_REFS  I18N
  TEST_FLAGS -resource:Test/texts/ascii-test.txt,Test/texts/ascii-test.txt -resource:Test/texts/encoder-handlefallback-generic-output.txt,Test/texts/encoder-handlefallback-generic-output.txt -resource:Test/texts/encoder-handlefallback-generic-utf8.txt,Test/texts/encoder-handlefallback-generic-utf8.txt -resource:Test/texts/turkish-1254.txt,Test/texts/turkish-1254.txt -resource:Test/texts/turkish-utf8.txt,Test/texts/turkish-utf8.txt
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
