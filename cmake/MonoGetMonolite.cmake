# Fetches the prebuilt bootstrap compiler.
#
# The class libraries need a working mono to compile the first mscorlib with.
# Normally that is one already installed.  Monolite is the fallback for
# machines that have none.  Run as `cmake --build <dir> --target get-monolite-latest`,
# then point MONO_BOOTSTRAP_RUNTIME at it.
#
#   MONO_MONOLITE_URL   where to fetch from
#   MONO_MONOLITE_DIR   where to unpack it
#   MONO_MONOLITE_NAME  the directory name inside the tarball

set(_tarball "${MONO_MONOLITE_DIR}/monolite.tar.gz")
file(MAKE_DIRECTORY "${MONO_MONOLITE_DIR}")

message(STATUS "Fetching ${MONO_MONOLITE_URL}")
file(DOWNLOAD "${MONO_MONOLITE_URL}" "${_tarball}" STATUS _st SHOW_PROGRESS)
list(GET _st 0 _rc)
if(NOT _rc EQUAL 0)
  list(GET _st 1 _msg)
  file(REMOVE "${_tarball}")
  message(FATAL_ERROR "could not fetch ${MONO_MONOLITE_URL}: ${_msg}")
endif()

file(REMOVE_RECURSE "${MONO_MONOLITE_DIR}/${MONO_MONOLITE_NAME}")
file(ARCHIVE_EXTRACT INPUT "${_tarball}" DESTINATION "${MONO_MONOLITE_DIR}")
file(REMOVE "${_tarball}")
message(STATUS "Unpacked into ${MONO_MONOLITE_DIR}/${MONO_MONOLITE_NAME}")
