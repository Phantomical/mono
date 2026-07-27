# Kills a runtime that has stopped making progress, so CI gets a core dump
# instead of a timeout with no evidence.

if(NOT MONO_ENABLE_EXECUTABLES)
  return()
endif()

add_executable(mono-hang-watchdog mono-hang-watchdog.c)
target_link_libraries(mono-hang-watchdog PRIVATE mono::common mono::hidden)

install(TARGETS mono-hang-watchdog RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
