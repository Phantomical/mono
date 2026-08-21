cmake_minimum_required(VERSION 3.4.1)

add_library(runtime-bootstrap
             SHARED
             src/main/c/runtime-bootstrap.c )

find_library(log-lib
              log )

target_link_libraries(runtime-bootstrap
                       ${CMAKE_CURRENT_SOURCE_DIR}/src/main/jniLibs/${ANDROID_ABI}/libmonosgen-2.0.so
                       ${CMAKE_CURRENT_SOURCE_DIR}/src/main/jniLibs/${ANDROID_ABI}/libMonoPosixHelper.so
                       ${CMAKE_CURRENT_SOURCE_DIR}/src/main/jniLibs/${ANDROID_ABI}/libmono-native.so
                       ${CMAKE_CURRENT_SOURCE_DIR}/src/main/jniLibs/${ANDROID_ABI}/libmono-btls-shared.so
                       ${log-lib} )