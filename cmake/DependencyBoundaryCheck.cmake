# Configure-time dependency-boundary assertion [Phase 4 Stage 2 Task 21,
# gate matrix-issue16 row 12 as amended by gate/rulings-2026-08-18.md #16;
# recorded as a reusable pattern, registry item 14]. Nothing like this
# existed anywhere in this repository before this task (verified directly:
# .github/workflows/ci.yml and every existing CMakeLists.txt in this repo
# were checked for an analogous "assert target X does not link target Y"
# mechanism -- none found; boundary discipline before this task was
# convention + code review only).
#
# Fails the CONFIGURE step (before any compilation starts) if a named
# target's transitive LINK_LIBRARIES/INTERFACE_LINK_LIBRARIES closure
# contains a target/library name matching a forbidden substring -- this
# ticket's own concrete use is `rx_assert_target_excludes_dependency(rx_core
# "imgui")` (and the same for every other rx_* CORE target), enforcing the
# "core libraries and public ABI stay ImGui-free" hard boundary D20/the
# #16 ticket both restate at the point a violation is CHEAPEST to catch --
# before compiling anything, not at review time or via a runtime symbol
# audit after the fact.
#
# Deliberately NOT a CI-only step: this is plain CMake, called from the
# root CMakeLists.txt itself, so a local `cmake --preset ...` configure on
# any developer's machine catches a violation immediately, exactly like
# every other configure-time FATAL_ERROR this project's build already
# relies on (e.g. cmake/DepCache.cmake's own dep-cache failures).

# _rx_dep_closure_contains(<target> <forbidden> <depth> <out_found> <out_chain>)
#
# Recursive walk, depth-limited (not visited-set-deduplicated -- see below)
# rather than the more usual visited-set design: CMake function scoping
# does not support passing a mutable accumulator "by reference" through a
# chain of recursive calls the way a real programming language would (a
# variable NAME passed as an argument does not let a callee mutate the
# CALLER's variable except via the one-level-up PARENT_SCOPE mechanism,
# which only reaches the immediate caller, not an arbitrary ancestor) --
# threading a shared visited-set through recursion this way was tried
# first, this task, and does not work correctly. Given this project's own
# real target graph is small (under two dozen targets total, each with a
# handful of direct dependencies), an unmemoized walk that may revisit a
# shared dependency (e.g. rx_core) once per path reaching it costs nothing
# observable at configure time -- correctness (finding a real match if one
# exists) does not depend on deduplication, only performance would, and
# performance is a non-issue at this scale. The depth cap below is a purely
# defensive guard against a pathological/self-referential entry, not a
# realistic ceiling this project's own graph could ever approach.
function(_rx_dep_closure_contains _target _forbidden _depth _out_found _out_chain)
  if(_depth GREATER 64)
    message(FATAL_ERROR
      "[dependency-boundary-check] recursion depth exceeded walking '${_target}' -- likely a cyclic or "
      "self-referential LINK_LIBRARIES entry; investigate before raising this limit.")
  endif()

  # Substring match on the dependency NAME itself first -- catches both a
  # real target (e.g. a hypothetical `imgui` target) AND a plain, not-yet-
  # (or never-)defined library-name string that merely mentions the
  # forbidden name (e.g. "imgui_impl_vulkan" typed directly into some
  # future target_link_libraries() call before any such target exists) --
  # either way, a core target's own build-graph declaration naming
  # something ImGui-flavored is the violation this check exists to catch,
  # independent of whether that name currently resolves to a real target.
  string(TOLOWER "${_target}" _targetLower)
  string(TOLOWER "${_forbidden}" _forbiddenLower)
  string(FIND "${_targetLower}" "${_forbiddenLower}" _selfMatch)
  if(NOT _selfMatch EQUAL -1)
    set(${_out_found} TRUE PARENT_SCOPE)
    set(${_out_chain} "${_target}" PARENT_SCOPE)
    return()
  endif()

  if(NOT TARGET ${_target})
    # Not a real target (an interface/system library name like "m",
    # "dl", or a generator-expression fragment this walk could not
    # resolve) -- nothing further to walk, and no match already found
    # above, so this branch is clean.
    set(${_out_found} FALSE PARENT_SCOPE)
    return()
  endif()

  math(EXPR _childDepth "${_depth} + 1")

  set(_deps "")
  foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_val ${_target} ${_prop})
    if(_val)
      list(APPEND _deps ${_val})
    endif()
  endforeach()

  foreach(_dep ${_deps})
    # Strip the generator-expression wrappers this project's own
    # dependency graph can plausibly emit ($<LINK_ONLY:tgt> from a
    # PRIVATE/INTERFACE usage-requirement propagation; $<BUILD_INTERFACE:
    # tgt>/$<INSTALL_INTERFACE:tgt> from an exported package's own
    # generated targets, e.g. fastgltf/draco/KTX above) so the walk can
    # still recurse into the real target name underneath. Any OTHER,
    # unrecognized generator expression is left alone and simply skipped
    # below -- configure-time analysis cannot evaluate an arbitrary
    # generator expression's final value anyway (it may depend on
    # $<CONFIG:...> or similar, resolved only at generate time).
    string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _dep "${_dep}")
    string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" _dep "${_dep}")
    string(REGEX REPLACE "^\\$<INSTALL_INTERFACE:(.*)>$" "\\1" _dep "${_dep}")
    if(_dep MATCHES "\\$<")
      continue()
    endif()
    if("${_dep}" STREQUAL "${_target}")
      continue()  # a self-referential entry -- skip rather than recurse forever.
    endif()

    _rx_dep_closure_contains("${_dep}" "${_forbidden}" "${_childDepth}" _childFound _childChain)
    if(_childFound)
      set(${_out_found} TRUE PARENT_SCOPE)
      set(${_out_chain} "${_target} -> ${_childChain}" PARENT_SCOPE)
      return()
    endif()
  endforeach()

  set(${_out_found} FALSE PARENT_SCOPE)
endfunction()

# rx_assert_target_excludes_dependency(<target-name> <forbidden-substring>)
#
# Public entry point. `<target-name>` must already be a real, defined
# target (a target that does not exist yet is an ordering bug in the
# caller, not a vacuously-passing check -- fails loudly rather than
# silently skipping). Case-insensitive substring match against every name
# in `<target-name>`'s transitive link closure; FATAL_ERRORs, naming the
# full dependency chain, on any match.
function(rx_assert_target_excludes_dependency TARGET_NAME FORBIDDEN)
  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR
      "[dependency-boundary-check] target '${TARGET_NAME}' does not exist at the point this check runs -- "
      "this is a root CMakeLists.txt add_subdirectory-ordering bug (the check must run AFTER every target "
      "it examines is defined), not a passing check.")
  endif()

  _rx_dep_closure_contains("${TARGET_NAME}" "${FORBIDDEN}" 0 _rx_found _rx_chain)
  if(_rx_found)
    message(FATAL_ERROR
      "[dependency-boundary-check] '${TARGET_NAME}' transitively depends on something matching "
      "'${FORBIDDEN}' -- this violates the core-libraries-stay-ImGui-free hard boundary (spec D20, gate "
      "ruling #16). Dependency chain: ${_rx_chain}")
  endif()
endfunction()
