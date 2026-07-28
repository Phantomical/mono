# Converted from tools/sqlmetal/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       sqlmetal.exe
  PROFILES   net_4_x
  REFS       System.Configuration System.Core System.Data System.Xml System System.Runtime.Serialization
  FLAGS      -d:MONO_STRICT -keyfile:src/DbMetal/../DbLinq.snk -resource:src/DbMetal/Language/EnglishWords.txt,DbMetal.Language.EnglishWords.txt -resource:src/DbMetal/Language/FrenchWords.txt,DbMetal.Language.FrenchWords.txt -resource:src/DbMetal/Language/GermanWords.txt,DbMetal.Language.GermanWords.txt -publicsign
)
