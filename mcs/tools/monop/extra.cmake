# A smoke test: monop has to be able to dump three types without falling over.
mono_profile_dir(_pdir net_4_x)
add_test(NAME monop
         COMMAND "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" "${_pdir}/monop.exe"
                 System.Array System.String "System.Collections.Generic.List\`1")
set_tests_properties(monop PROPERTIES
  LABELS "tools" TIMEOUT 300 ENVIRONMENT "MONO_PATH=${_pdir}")
