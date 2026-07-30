mono_declare_managed(
  NAME       Mono.Data.Sqlite.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Data System.Transactions System.Xml
  FLAGS      /unsafe -resource:resources/SR.resources -resource:resources/SQLiteCommand.bmp -resource:resources/SQLiteConnection.bmp -resource:resources/SQLiteDataAdapter.bmp -d:SQLITE_STANDARD
  RESX       resources/SR.resources
  RESGEN_FLAGS -useSourcePath
  RESOURCES  resources/SQLiteCommand.bmp resources/SQLiteConnection.bmp resources/SQLiteDataAdapter.bmp
  TEST_FLAGS /unsafe -resource:resources/SR.resources -resource:resources/SQLiteCommand.bmp -resource:resources/SQLiteConnection.bmp -resource:resources/SQLiteDataAdapter.bmp -d:SQLITE_STANDARD /nowarn:618 -resource:Test/test.db,Test/test.db
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
