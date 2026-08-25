add_executable(test-dlopen-runtime test-dlopen-runtime.c)
target_link_libraries(test-dlopen-runtime PRIVATE ${CMAKE_DL_LIBS})

# Once per collector, because what this tests is the shared library rather than
# the runtime inside it.
foreach(_runtime monosgen-2.0 monoboehm-2.0)
  if(TARGET ${_runtime})
    add_test(NAME dlopen-${_runtime}
             COMMAND test-dlopen-runtime "$<TARGET_FILE:${_runtime}>")
  endif()
endforeach()

add_subdirectory(gtest)
add_subdirectory(managed)

if(MONO_CORPUS_ENABLED)
  add_subdirectory(il-offsets)
  add_subdirectory(jitdump)
endif()
