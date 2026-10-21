# VcpkgRoot.cmake - decides which vcpkg checkout this configure uses.
#
# vcpkg.json pins the registry with "builtin-baseline", and vcpkg can only read
# the ports of that commit from a checkout whose history holds it. A developer
# with several checkouts and worktrees does not want a copy of vcpkg in each of
# them, and must not point them all at one directory that something else might
# check out on another branch, shorten or delete. This module picks the root,
# first match wins:
#
#   1. CMAKE_TOOLCHAIN_FILE given on the command line, in a preset or by an IDE:
#      used as it is. (A vcpkg.cmake that does not exist gets a message that
#      says what is dead; a toolchain that is not vcpkg's, such as Emscripten's
#      or the vcpkg port's own, is none of this module's business.)
#   2. ICLFORGE_VCPKG_ROOT (CMake variable or environment): that directory.
#      A different commit from the baseline is a warning, not an error.
#   3. ICLFORGE_VCPKG_STORE, else ICL_VCPKG_STORE (each a CMake variable or an
#      environment variable): a shared store. ICL_VCPKG_STORE is the one to set
#      per machine; every Iain Chesworth Labs repository that carries this
#      module reads it, and ICLFORGE_VCPKG_STORE overrides it for this one.
#      The commit the baseline names gets its own checkout in
#      <store>/<first 12 of the commit>, made once from a shared clone of the
#      labs' vcpkg fork (<store>/mirror, or ICLFORGE_VCPKG_MIRROR /
#      ICL_VCPKG_MIRROR) with `git worktree add`. Every checkout, worktree and
#      repository that pins that commit reuses it; a new baseline gets a new
#      directory, and nothing that already exists is ever moved or modified.
#      (There is deliberately no org-wide root: one directory cannot be at the
#      commit every repository pins.)
#   4. VCPKG_ROOT (environment): a vcpkg you manage yourself, which is what CI
#      does (.github/actions/setup-vcpkg checks out the baseline and exports
#      it). This repository has no deps/vcpkg submodule to fall back on.
#
# With none of those, a configure that asks for vcpkg (a VCPKG_TARGET_TRIPLET,
# which every preset that inherits "core" sets) stops and lists the options. One
# that does not (a plain `cmake -S . -B build`, the Python wheel, the Conan
# recipe) carries on without it: fmt, Catch2, fast-float and nlohmann-json have
# FetchContent fallbacks.
#
# The fork the mirror is cloned from is ICLFORGE_VCPKG_URL (CMake variable or
# environment), by default the one CI clones.
#
# The chosen root is left in ICLFORGE_VCPKG_ROOT_DIR (cache, internal).

include_guard(GLOBAL)

# The commit vcpkg.json pins as its builtin-baseline ("" when it has none).
function(_iclforge_vcpkg_pin source_dir out)
    set(${out} "" PARENT_SCOPE)
    if(NOT EXISTS "${source_dir}/vcpkg.json")
        return()
    endif()
    file(READ "${source_dir}/vcpkg.json" manifest)
    string(JSON baseline ERROR_VARIABLE err GET "${manifest}" "builtin-baseline")
    if(err STREQUAL "NOTFOUND" AND baseline MATCHES "^[0-9a-f]+$")
        set(${out} "${baseline}" PARENT_SCOPE)
    endif()
endfunction()

function(_iclforge_vcpkg_head root out)
    set(${out} "" PARENT_SCOPE)
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${root}" rev-parse HEAD
        OUTPUT_VARIABLE head
        RESULT_VARIABLE rc
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(rc EQUAL 0)
        set(${out} "${head}" PARENT_SCOPE)
    endif()
endfunction()

# Says what is wrong with a path that should hold vcpkg, as text for a message.
function(_iclforge_vcpkg_describe path out)
    set(text "${path} does not hold vcpkg.")
    if(IS_SYMLINK "${path}")
        file(READ_SYMLINK "${path}" target)
        set(text "${path} is a link to ${target}, which ")
        if(EXISTS "${target}")
            string(APPEND text "has no scripts/buildsystems/vcpkg.cmake in it.")
        else()
            string(APPEND text "does not exist. Whatever it pointed at has been deleted or moved.")
        endif()
    elseif(EXISTS "${path}")
        string(APPEND text " The directory exists but has no scripts/buildsystems/vcpkg.cmake.")
    else()
        string(APPEND text " It does not exist.")
    endif()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(_iclforge_vcpkg_fail)
    list(JOIN ARGV "" what)
    message(FATAL_ERROR
        "${what}\n"
        "\n"
        "Options, from the least to the most setup:\n"
        "  * set ICL_VCPKG_STORE to a directory shared by your checkouts and repositories (a\n"
        "    copy per pinned vcpkg commit, made on first use; see docs/building.md)\n"
        "  * set ICLFORGE_VCPKG_ROOT (or VCPKG_ROOT) to a vcpkg checkout you manage yourself,\n"
        "    at the commit vcpkg.json's builtin-baseline names\n")
endfunction()

# Runs git in `dir`; leaves the exit code and output in _iclforge_git_rc and _iclforge_git_log.
function(_iclforge_vcpkg_git dir)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c core.longpaths=true -C "${dir}" ${ARGN}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE log
        ERROR_VARIABLE log)
    set(_iclforge_git_rc "${rc}" PARENT_SCOPE)
    set(_iclforge_git_log "${log}" PARENT_SCOPE)
endfunction()

# The first non-empty of the listed settings, each a CMake variable or an
# environment variable of the same name; "" when none is set.
function(_iclforge_vcpkg_setting out)
    set(${out} "" PARENT_SCOPE)
    foreach(name IN LISTS ARGN)
        if(DEFINED ${name} AND NOT "${${name}}" STREQUAL "")
            set(${out} "${${name}}" PARENT_SCOPE)
            return()
        elseif(DEFINED ENV{${name}} AND NOT "$ENV{${name}}" STREQUAL "")
            set(${out} "$ENV{${name}}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# The checkout of `pin` in the shared store, created on first use.
function(_iclforge_vcpkg_store_checkout store pin out)
    if(NOT pin)
        _iclforge_vcpkg_fail(
            "A vcpkg store is set but vcpkg.json has no builtin-baseline to make a checkout of.")
    endif()
    string(SUBSTRING "${pin}" 0 12 short)
    set(dest "${store}/${short}")
    set(ready "${dest}/.iclforge-ready")
    if(EXISTS "${ready}")
        set(${out} "${dest}" PARENT_SCOPE)
        return()
    endif()

    _iclforge_vcpkg_setting(mirror ICLFORGE_VCPKG_MIRROR ICL_VCPKG_MIRROR)
    if(mirror STREQUAL "")
        set(mirror "${store}/mirror")
    else()
        file(TO_CMAKE_PATH "${mirror}" mirror)
    endif()
    _iclforge_vcpkg_setting(url ICLFORGE_VCPKG_URL)
    if(url STREQUAL "")
        set(url "https://github.com/iainchesworthlabs/vcpkg.git")
    endif()

    find_package(Git REQUIRED)
    file(MAKE_DIRECTORY "${store}")
    message(STATUS "vcpkg ${short} is not in ${store} yet; taking the store lock")
    file(LOCK "${store}/.lock" GUARD FUNCTION TIMEOUT 3600 RESULT_VARIABLE lock_rc)
    if(NOT lock_rc STREQUAL "0")
        _iclforge_vcpkg_fail("Could not take ${store}/.lock (${lock_rc}); another configure may be creating the checkout.")
    endif()
    # Another configure may have finished while this one waited.
    if(EXISTS "${ready}")
        set(${out} "${dest}" PARENT_SCOPE)
        return()
    endif()

    # The shared clone of the fork.
    if(NOT EXISTS "${mirror}/HEAD" AND NOT EXISTS "${mirror}/.git")
        message(STATUS "Cloning ${url} to ${mirror} (once, for every checkout that shares ${store})")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -c core.longpaths=true clone --bare "${url}" "${mirror}"
            RESULT_VARIABLE rc ERROR_VARIABLE log OUTPUT_VARIABLE log)
        if(NOT rc EQUAL 0)
            _iclforge_vcpkg_fail("Cloning ${url} failed:\n${log}")
        endif()
        _iclforge_vcpkg_git("${mirror}" config remote.origin.fetch "+refs/heads/*:refs/heads/*")
    endif()

    _iclforge_vcpkg_git("${mirror}" cat-file -e "${pin}^{commit}")
    if(NOT _iclforge_git_rc EQUAL 0)
        message(STATUS "Fetching ${pin} into ${mirror}")
        _iclforge_vcpkg_git("${mirror}" fetch origin)
        _iclforge_vcpkg_git("${mirror}" cat-file -e "${pin}^{commit}")
        if(NOT _iclforge_git_rc EQUAL 0)
            # The mirror's origin is a different repository from the fork this
            # checkout names (another repository's fork, say): take the
            # branches of the named one too. They land under refs/fetched/ so
            # they cannot collide with the mirror's own branches.
            message(STATUS "Fetching ${url} into ${mirror}")
            _iclforge_vcpkg_git("${mirror}" fetch "${url}" "+refs/heads/*:refs/fetched/*")
            _iclforge_vcpkg_git("${mirror}" cat-file -e "${pin}^{commit}")
        endif()
        if(NOT _iclforge_git_rc EQUAL 0)
            _iclforge_vcpkg_fail("Commit ${pin} (vcpkg.json's builtin-baseline) is not in ${mirror}, even after fetching its origin and ${url}.")
        endif()
    endif()

    # A half-made checkout from an interrupted configure: git knows it as a
    # worktree, so it can remove it; anything else is left for a human.
    _iclforge_vcpkg_git("${mirror}" worktree prune)
    if(EXISTS "${dest}")
        _iclforge_vcpkg_git("${mirror}" worktree remove --force "${dest}")
        if(EXISTS "${dest}")
            _iclforge_vcpkg_fail("${dest} exists but is not a finished checkout, and git will not remove it. Delete that directory and configure again.")
        endif()
    endif()

    message(STATUS "Checking out vcpkg ${short} into ${dest} (a few minutes, once)")
    _iclforge_vcpkg_git("${mirror}" worktree add --detach "${dest}" "${pin}")
    if(NOT _iclforge_git_rc EQUAL 0)
        _iclforge_vcpkg_fail("git worktree add ${dest} ${pin} failed:\n${_iclforge_git_log}")
    endif()
    file(WRITE "${ready}" "${pin}\n")
    set(${out} "${dest}" PARENT_SCOPE)
endfunction()

# Sets CMAKE_TOOLCHAIN_FILE (unless the caller supplied one) and
# ICLFORGE_VCPKG_ROOT_DIR. Must run before project().
function(iclforge_resolve_vcpkg source_dir)
    set(rel "scripts/buildsystems/vcpkg.cmake")

    # A toolchain given from outside wins. One this function chose on an
    # earlier configure comes back from the cache looking the same, so it is
    # remembered and resolved again: the baseline may have moved since.
    if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "")
        if(NOT DEFINED ICLFORGE_VCPKG_AUTO_TOOLCHAIN
           OR NOT CMAKE_TOOLCHAIN_FILE STREQUAL ICLFORGE_VCPKG_AUTO_TOOLCHAIN)
            get_filename_component(name "${CMAKE_TOOLCHAIN_FILE}" NAME)
            if(NOT name STREQUAL "vcpkg.cmake")
                return()
            endif()
            if(NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
                get_filename_component(root "${CMAKE_TOOLCHAIN_FILE}/../../.." ABSOLUTE)
                _iclforge_vcpkg_describe("${root}" why)
                _iclforge_vcpkg_fail("CMAKE_TOOLCHAIN_FILE is ${CMAKE_TOOLCHAIN_FILE}, which does not exist.\n${why}")
            endif()
            get_filename_component(root "${CMAKE_TOOLCHAIN_FILE}/../../.." ABSOLUTE)
            set(ICLFORGE_VCPKG_ROOT_DIR "${root}" CACHE INTERNAL "")
            return()
        endif()
    endif()

    _iclforge_vcpkg_pin("${source_dir}" pin)
    set(root "")
    set(named "")

    _iclforge_vcpkg_setting(want_root ICLFORGE_VCPKG_ROOT)
    _iclforge_vcpkg_setting(want_store ICLFORGE_VCPKG_STORE ICL_VCPKG_STORE)

    if(want_root)
        file(TO_CMAKE_PATH "${want_root}" root)
        if(NOT EXISTS "${root}/${rel}")
            _iclforge_vcpkg_describe("${root}" why)
            _iclforge_vcpkg_fail("ICLFORGE_VCPKG_ROOT is set to ${root}.\n${why}")
        endif()
        set(named "ICLFORGE_VCPKG_ROOT")
    elseif(want_store)
        file(TO_CMAKE_PATH "${want_store}" store)
        _iclforge_vcpkg_store_checkout("${store}" "${pin}" root)
        if(NOT EXISTS "${root}/${rel}")
            _iclforge_vcpkg_describe("${root}" why)
            _iclforge_vcpkg_fail("The shared checkout ${root} is unusable.\n${why}")
        endif()
    elseif(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" root)
        if(NOT EXISTS "${root}/${rel}")
            _iclforge_vcpkg_describe("${root}" why)
            _iclforge_vcpkg_fail("VCPKG_ROOT is set to ${root}.\n${why}")
        endif()
        set(named "VCPKG_ROOT")
    else()
        # Nothing names a vcpkg. A configure that asks for one by its triplet
        # cannot go on; any other does without (the FetchContent fallbacks).
        if(DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET STREQUAL "")
            _iclforge_vcpkg_fail("This configure uses vcpkg (VCPKG_TARGET_TRIPLET is ${VCPKG_TARGET_TRIPLET}) but none is set.")
        endif()
        return()
    endif()

    if(named AND pin)
        _iclforge_vcpkg_head("${root}" head)
        if(head AND NOT head STREQUAL pin)
            message(WARNING
                "${named} is at ${head}, but vcpkg.json's builtin-baseline is ${pin}. "
                "Ports and versions that do not match the checkout's history can fail to resolve. "
                "A vcpkg store (ICL_VCPKG_STORE) keeps a checkout per baseline instead.")
        endif()
    endif()

    set(toolchain "${root}/${rel}")
    # CMake keeps the toolchain file this build directory was first configured
    # with and ignores a different one on a later configure, so a baseline that
    # moved (or a store that was switched on) would leave the old scripts in
    # use while everything else follows the new ones.
    if(DEFINED ICLFORGE_VCPKG_AUTO_TOOLCHAIN AND NOT ICLFORGE_VCPKG_AUTO_TOOLCHAIN STREQUAL toolchain)
        message(FATAL_ERROR
            "vcpkg now resolves to\n"
            "  ${toolchain}\n"
            "but this build directory was configured with\n"
            "  ${ICLFORGE_VCPKG_AUTO_TOOLCHAIN}\n"
            "(the builtin-baseline in vcpkg.json moved, or ICLFORGE_VCPKG_* / ICL_VCPKG_STORE / "
            "VCPKG_ROOT changed). CMake keeps the toolchain file of a build directory's first "
            "configure, so configure again with --fresh (cmake --fresh --preset <name>), which "
            "clears the cache but keeps the directory.")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "${toolchain}" CACHE FILEPATH "vcpkg toolchain file" FORCE)
    set(ICLFORGE_VCPKG_AUTO_TOOLCHAIN "${toolchain}" CACHE INTERNAL "")
    set(ICLFORGE_VCPKG_ROOT_DIR "${root}" CACHE INTERNAL "")
    message(STATUS "vcpkg: ${root}")
endfunction()
