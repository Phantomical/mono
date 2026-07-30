mono_declare_managed(
  NAME       I18N.CJK.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       I18N
  FLAGS      /unsafe /resource:big5.table /resource:gb2312.table /resource:jis.table /resource:ks.table /resource:gb18030.table /define:DISABLE_UNSAFE
  TEST_REFS  I18N
  TEST_FLAGS -resource:Test/texts/chinese-54936.txt,Test/texts/chinese-54936.txt -resource:Test/texts/chinese-936.txt,Test/texts/chinese-936.txt -resource:Test/texts/chinese-utf8.txt,Test/texts/chinese-utf8.txt -resource:Test/texts/chinese2-950.txt,Test/texts/chinese2-950.txt -resource:Test/texts/chinese2-utf8.txt,Test/texts/chinese2-utf8.txt -resource:Test/texts/chinese3-936.txt,Test/texts/chinese3-936.txt -resource:Test/texts/chinese3-utf8.txt,Test/texts/chinese3-utf8.txt -resource:Test/texts/chinese4-950.txt,Test/texts/chinese4-950.txt -resource:Test/texts/chinese4-utf8.txt,Test/texts/chinese4-utf8.txt -resource:Test/texts/japanese-50221.txt,Test/texts/japanese-50221.txt -resource:Test/texts/japanese-50222.txt,Test/texts/japanese-50222.txt -resource:Test/texts/japanese-51932.txt,Test/texts/japanese-51932.txt -resource:Test/texts/japanese-932.txt,Test/texts/japanese-932.txt -resource:Test/texts/japanese-utf8.txt,Test/texts/japanese-utf8.txt -resource:Test/texts/japanese2-50220.txt,Test/texts/japanese2-50220.txt -resource:Test/texts/japanese2-utf8.txt,Test/texts/japanese2-utf8.txt -resource:Test/texts/japanese3-50220.txt,Test/texts/japanese3-50220.txt -resource:Test/texts/japanese3-50221.txt,Test/texts/japanese3-50221.txt -resource:Test/texts/japanese3-utf8.txt,Test/texts/japanese3-utf8.txt -resource:Test/texts/korean-949.txt,Test/texts/korean-949.txt -resource:Test/texts/korean-utf8.txt,Test/texts/korean-utf8.txt
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
