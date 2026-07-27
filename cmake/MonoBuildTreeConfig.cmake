# Rewrites the installed data/config into the build tree's etc/mono/config,
# pointing the dllmap entries for the helper libraries mono builds itself at
# the copies in the build tree.  Without this an uninstalled runtime resolves
# `System.Native` and friends through ld.so and picks up whatever mono is
# installed system-wide.
#
# Run as a script (cmake -P) with IN, OUT and BUILD_DIR.

file(READ "${IN}" _cfg)

# NOTE: the automake rules this replaces matched on `$mono_libdir/<name>`, but
# data/config.in only carries that prefix for the System.Native entries -- so
# the MonoPosixHelper and btls rewrites silently never fired and those two kept
# resolving through ld.so.  Matching the bare names as well makes them fire,
# which is what the rules were there for.
string(REPLACE "target=\"libMonoPosixHelper.so\""
               "target=\"${BUILD_DIR}/support/libMonoPosixHelper.so\""
               _cfg "${_cfg}")
string(REPLACE "target=\"\$mono_libdir/libmono-native.so\""
               "target=\"${BUILD_DIR}/mono/native/libmono-native.so\""
               _cfg "${_cfg}")
string(REPLACE "target=\"libmono-btls-shared.so\""
               "target=\"${BUILD_DIR}/mono/btls/libmono-btls-shared.so\""
               _cfg "${_cfg}")

get_filename_component(_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
file(WRITE "${OUT}" "${_cfg}")
