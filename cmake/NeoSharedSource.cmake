# Standalone builds use the sibling sources shipped in the collection. An
# isolated repository automatically obtains NeoShared/main in an ignored cache.
# Existing developer checkouts are never updated or reset by this resolver.
get_filename_component(_neo_tool_source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
function(_neo_shared_source output tool_source)
    # Respect an explicit CMake/build-script override. This is important for CI,
    # packaged workspaces, and offline builds; silently replacing it with an
    # automatic checkout makes --neoshared-root ineffective.
    if(DEFINED NEOSHARED_ROOT AND NOT "${NEOSHARED_ROOT}" STREQUAL "")
        get_filename_component(_explicit "${NEOSHARED_ROOT}" ABSOLUTE BASE_DIR "${tool_source}")
        if(NOT EXISTS "${_explicit}/CMakeLists.txt")
            message(FATAL_ERROR "NEOSHARED_ROOT does not contain CMakeLists.txt: ${_explicit}")
        endif()
        set(${output} "${_explicit}" PARENT_SCOPE)
        return()
    endif()
    if(TARGET neoshared::wx)
        message(FATAL_ERROR "A workspace supplied neoshared::wx without NEOSHARED_ROOT.")
    endif()
    foreach(_name IN ITEMS neoshared NeoShared)
        get_filename_component(_candidate "${tool_source}/../${_name}" ABSOLUTE)
        if(EXISTS "${_candidate}/CMakeLists.txt")
            set(${output} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    find_package(Git QUIET REQUIRED)
    set(_cache "${tool_source}/.neo-deps")
    set(_source "${_cache}/neoshared")
    set(_repository "https://github.com/vrifftech/neoshared.git")
    file(MAKE_DIRECTORY "${_cache}")
    file(LOCK "${_cache}/neoshared.lock" GUARD FUNCTION TIMEOUT 180)
    if(NOT EXISTS "${_source}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" clone --quiet --depth 1
            --branch main --single-branch "${_repository}" "${_source}"
            RESULT_VARIABLE _result ERROR_VARIABLE _error TIMEOUT 180)
        if(NOT "${_result}" STREQUAL "0")
            # This call created the directory; it owns any incomplete clone.
            file(REMOVE_RECURSE "${_source}")
            message(FATAL_ERROR "Could not download NeoShared/main: ${_error}")
        endif()
    else()
        if(NOT IS_DIRECTORY "${_source}/.git")
            message(FATAL_ERROR "The automatic source cache is not a Git checkout: ${_source}. It has not been modified.")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_source}" config --get remote.origin.url
            OUTPUT_VARIABLE _origin OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _result)
        if(NOT "${_result}" STREQUAL "0" OR NOT _origin STREQUAL _repository)
            message(FATAL_ERROR "The automatic NeoShared cache has a different origin; it has not been modified: ${_source}")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_source}" status --porcelain --untracked-files=all
            OUTPUT_VARIABLE _dirty RESULT_VARIABLE _result)
        if(NOT "${_result}" STREQUAL "0" OR NOT _dirty STREQUAL "")
            message(FATAL_ERROR "The automatic NeoShared cache contains local changes; it has not been modified: ${_source}")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_source}" fetch --quiet --no-tags --depth 1
            origin "+refs/heads/main:refs/remotes/origin/main"
            RESULT_VARIABLE _result ERROR_VARIABLE _error TIMEOUT 180)
        if(NOT "${_result}" STREQUAL "0")
            message(FATAL_ERROR "Could not refresh NeoShared/main: ${_error}")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_source}" checkout --quiet --detach refs/remotes/origin/main
            RESULT_VARIABLE _result ERROR_VARIABLE _error)
        if(NOT "${_result}" STREQUAL "0")
            message(FATAL_ERROR "Could not select NeoShared/main: ${_error}")
        endif()
    endif()
    if(NOT EXISTS "${_source}/CMakeLists.txt")
        message(FATAL_ERROR "NeoShared/main is incomplete: CMakeLists.txt is missing.")
    endif()
    set(${output} "${_source}" PARENT_SCOPE)
endfunction()
_neo_shared_source(_neo_resolved_shared "${_neo_tool_source}")
# Publish the resolved source path to the cache for the rest of this configure.
set(NEOSHARED_ROOT "${_neo_resolved_shared}" CACHE PATH "Automatically resolved NeoShared sources" FORCE)
if(CMAKE_SCRIPT_MODE_FILE)
    # Shell/PowerShell build entry points use the same resolver before packaging.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${NEOSHARED_ROOT}")
endif()
