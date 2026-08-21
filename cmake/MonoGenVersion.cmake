# Writes mono/mini/version.h.  Run as a script (cmake -P) at build time so the
# recorded revision follows the checkout rather than the last configure.
#
# Expects SRC_DIR, OUT and GIT_EXECUTABLE on the command line.  git is searched
# for at configure time, along with everything else the build needs from
# outside the tree.  An empty GIT_EXECUTABLE means it was not found.

set(_full "tarball")

if(EXISTS "${SRC_DIR}/.git")
  if(GIT_EXECUTABLE)
    if(DEFINED ENV{ghprbPullId} AND NOT "$ENV{ghprbPullId}" STREQUAL "")
      set(_branch "pull-request-$ENV{ghprbPullId}")
    else()
      execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
                      WORKING_DIRECTORY "${SRC_DIR}"
                      OUTPUT_VARIABLE _branch
                      OUTPUT_STRIP_TRAILING_WHITESPACE
                      ERROR_QUIET)
      if(_branch STREQUAL "HEAD")
        set(_branch "explicit")
      endif()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" log --no-color --first-parent -n1 --pretty=format:%h
                    WORKING_DIRECTORY "${SRC_DIR}"
                    OUTPUT_VARIABLE _rev
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_branch AND _rev)
      set(_full "${_branch}/${_rev}")
    endif()
  endif()
elseif(DEFINED ENV{MONO_BRANCH} AND DEFINED ENV{MONO_BUILD_REVISION})
  set(_full "$ENV{MONO_BRANCH}/$ENV{MONO_BUILD_REVISION}")
endif()

set(_content "#define FULL_VERSION \"${_full}\"\n")

# Only touch the file when the contents change, so an unchanged HEAD does not
# trigger a rebuild of everything that includes it.
if(EXISTS "${OUT}")
  file(READ "${OUT}" _old)
  if(_old STREQUAL _content)
    return()
  endif()
endif()
file(WRITE "${OUT}" "${_content}")
