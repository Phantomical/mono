# The JNI shim IKVM.NET P/Invokes into.

add_library(ikvm-native SHARED jni.c os.c)
target_link_libraries(ikvm-native PRIVATE
  mono::common mono::eglib_headers eglib)

add_library(ikvm-native-static STATIC jni.c os.c)
target_link_libraries(ikvm-native-static PRIVATE mono::common mono::eglib_headers)
set_target_properties(ikvm-native-static PROPERTIES OUTPUT_NAME ikvm-native)

install(TARGETS ikvm-native ikvm-native-static
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
