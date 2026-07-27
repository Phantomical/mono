# Read-only view of the git submodules a directory depends on.
#
# .gitmodules and the index are the registry: the URL comes from the former and
# the pinned revision is the gitlink in the latter, so there is nothing to parse
# but git's own metadata.
#
# Nothing here fetches.  Getting sources is the user's job; this only detects
# and reports, and a missing checkout skips the tests that need it.

find_package(Git QUIET)

# ---------------------------------------------------------------------------
# mono_submodule_status(<path> <slug>)
#
# <path> is repository-relative, as it appears in .gitmodules.  Defines in the
# caller's scope:
#   MONO_SUBMODULE_<slug>_PATH      absolute path to the checkout
#   MONO_SUBMODULE_<slug>_URL       from .gitmodules
#   MONO_SUBMODULE_<slug>_REV       the gitlink the superproject records
#   MONO_SUBMODULE_<slug>_HEAD      what is checked out, or "" when absent
#   MONO_SUBMODULE_<slug>_PRESENT   the checkout exists
#   MONO_SUBMODULE_<slug>_AT_REV    HEAD matches the recorded gitlink
# ---------------------------------------------------------------------------
function(mono_submodule_status path slug)
  set(_abs "${CMAKE_SOURCE_DIR}/${path}")

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" config --file "${CMAKE_SOURCE_DIR}/.gitmodules"
            --get "submodule.${path}.url"
    OUTPUT_VARIABLE _url OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

  # The gitlink: what revision this commit of the superproject pins.
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" ls-files -s -- "${path}"
    OUTPUT_VARIABLE _lsfiles OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  set(_rev "")
  if(_lsfiles MATCHES "^160000 ([0-9a-f]+)")
    set(_rev "${CMAKE_MATCH_1}")
  endif()

  set(_present OFF)
  set(_head "")
  set(_at_rev OFF)
  if(EXISTS "${_abs}/.git")
    set(_present ON)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${_abs}" rev-parse HEAD
      OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_head AND _head STREQUAL _rev)
      set(_at_rev ON)
    endif()
  endif()

  set(MONO_SUBMODULE_${slug}_PATH    "${_abs}"     PARENT_SCOPE)
  set(MONO_SUBMODULE_${slug}_URL     "${_url}"     PARENT_SCOPE)
  set(MONO_SUBMODULE_${slug}_REV     "${_rev}"     PARENT_SCOPE)
  set(MONO_SUBMODULE_${slug}_HEAD    "${_head}"    PARENT_SCOPE)
  set(MONO_SUBMODULE_${slug}_PRESENT "${_present}" PARENT_SCOPE)
  set(MONO_SUBMODULE_${slug}_AT_REV  "${_at_rev}"  PARENT_SCOPE)
  set(MONO_SUBMODULE_${slug}_GITPATH "${path}"     PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# mono_submodule_init_hint(<slug> <out-var>)
#
# The command that fetches this one checkout.  Named per-submodule rather than
# as a bare `git submodule update --init`, because the acceptance-test corpora
# are large and most builds want none of them.
# ---------------------------------------------------------------------------
function(mono_submodule_init_hint slug out)
  set(_extra "")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" config --file "${CMAKE_SOURCE_DIR}/.gitmodules"
            --get "submodule.${MONO_SUBMODULE_${slug}_GITPATH}.update"
    OUTPUT_VARIABLE _update OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_update STREQUAL "none")
    # `update = none` makes plain `--init` skip it, on purpose.
    set(_extra " --checkout")
  endif()
  set(${out}
      "git submodule update --init${_extra} ${MONO_SUBMODULE_${slug}_GITPATH}"
      PARENT_SCOPE)
endfunction()
