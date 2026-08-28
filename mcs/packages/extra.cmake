# csi is Roslyn's C# script host; the test checks it runs a script on this
# runtime at all.
get_filename_component(_roslyn "${MONO_CSC}" DIRECTORY)
mono_profile_dir(_pdir net_4_x)
add_test(NAME csi
         COMMAND "${CMAKE_COMMAND}"
                 -D "RUNTIME=${MONO_RUNTIME_WRAPPER}"
                 -D "CSI=${_roslyn}/csi.exe"
                 -D "SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/csi-test.csx"
                 -D "MONO_PATH=${_pdir}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
set_tests_properties(csi PROPERTIES LABELS "tools" TIMEOUT 600)
