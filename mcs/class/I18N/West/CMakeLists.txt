mono_declare_managed(
  NAME       I18N.West.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       I18N
  FLAGS      /unsafe
  TEST_FLAGS -resource:Test/texts/box-437.txt,Test/texts/box-437.txt -resource:Test/texts/box-utf8.txt,Test/texts/box-utf8.txt -resource:Test/texts/czech-utf8.txt,Test/texts/czech-utf8.txt -resource:Test/texts/esperanto-utf8.txt,Test/texts/esperanto-utf8.txt -resource:Test/texts/french-10000.txt,Test/texts/french-10000.txt -resource:Test/texts/french-utf8.txt,Test/texts/french-utf8.txt -resource:Test/texts/french2-863.txt,Test/texts/french2-863.txt -resource:Test/texts/french2-utf8.txt,Test/texts/french2-utf8.txt -resource:Test/texts/greek-1253.txt,Test/texts/greek-1253.txt -resource:Test/texts/greek-28597.txt,Test/texts/greek-28597.txt -resource:Test/texts/greek-utf8.txt,Test/texts/greek-utf8.txt -resource:Test/texts/hungarian-28592.txt,Test/texts/hungarian-28592.txt -resource:Test/texts/hungarian-utf8.txt,Test/texts/hungarian-utf8.txt -resource:Test/texts/icelandic-10079.txt,Test/texts/icelandic-10079.txt -resource:Test/texts/icelandic-utf8.txt,Test/texts/icelandic-utf8.txt -resource:Test/texts/icelandic2-861.txt,Test/texts/icelandic2-861.txt -resource:Test/texts/icelandic2-utf8.txt,Test/texts/icelandic2-utf8.txt -resource:Test/texts/italian-28593.txt,Test/texts/italian-28593.txt -resource:Test/texts/italian-utf8.txt,Test/texts/italian-utf8.txt -resource:Test/texts/latin-28605.txt,Test/texts/latin-28605.txt -resource:Test/texts/latin-850.txt,Test/texts/latin-850.txt -resource:Test/texts/latin-utf8.txt,Test/texts/latin-utf8.txt -resource:Test/texts/nordic-865.txt,Test/texts/nordic-865.txt -resource:Test/texts/nordic-utf8.txt,Test/texts/nordic-utf8.txt -resource:Test/texts/norwegian-1252.txt,Test/texts/norwegian-1252.txt -resource:Test/texts/norwegian-utf8.txt,Test/texts/norwegian-utf8.txt -resource:Test/texts/polish-1250.txt,Test/texts/polish-1250.txt -resource:Test/texts/polish-utf8.txt,Test/texts/polish-utf8.txt -resource:Test/texts/portguese-860.txt,Test/texts/portguese-860.txt -resource:Test/texts/portguese-utf8.txt,Test/texts/portguese-utf8.txt -resource:Test/texts/proto-slavic-utf8.txt,Test/texts/proto-slavic-utf8.txt
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
