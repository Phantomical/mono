# The parser generator the C# compiler and ilasm are built with.  Native C, and
# the only thing under mcs/ that is not a managed assembly.
#
# SKEL_DIRECTORY is only the fallback location for the skeleton; every caller
# in this tree redirects a skeleton file in on stdin instead, so it never
# matters in practice.  It is kept because jay reads it when given neither.
add_executable(jay
  closure.c error.c lalr.c lr0.c main.c mkpar.c output.c reader.c
  symtab.c verbose.c warshall.c)
target_compile_definitions(jay PRIVATE
  "SKEL_DIRECTORY=\"${CMAKE_INSTALL_PREFIX}/share/jay\"")
# Not installed: this is a build tool, and the makefiles never reached the
# install rule for it either -- jay only ever ran in the bootstrap profile,
# which installs nothing.
