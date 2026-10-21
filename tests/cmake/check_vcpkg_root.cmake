# Script-mode CTest case (cmake -P): cmake/VcpkgRoot.cmake picks the vcpkg root
# the way its header says it does.
#
# Nothing here touches the network or a real vcpkg. A fake upstream repository
# stands in for the vcpkg fork (one file, a toolchain that prints a line), a
# fake source tree names a commit of it as its vcpkg.json builtin-baseline, and
# a tiny project that calls iclforge_resolve_vcpkg() before project() is
# configured against them in each of the situations that matter:
#
#   store          first use makes <store>/<baseline>, the second configure reuses it
#   baseline moved an old build directory is told to configure --fresh (CMake
#                  would otherwise keep the old toolchain), and does after it
#   ICL store      ICL_VCPKG_STORE does the same; ICLFORGE_VCPKG_STORE wins
#                  when both are set
#   other fork     a baseline that is not in the mirror's origin is fetched
#                  from ICLFORGE_VCPKG_URL, so repositories with different
#                  forks can share one store
#   root           ICLFORGE_VCPKG_ROOT at another commit warns, does not fail
#   VCPKG_ROOT     used when nothing else is set (what CI does), warns at
#                  another commit, and loses to a store: a stale one in the
#                  environment cannot undo the store
#   dead toolchain a CMAKE_TOOLCHAIN_FILE that is vcpkg's and does not exist
#                  says so; one that is not vcpkg's is left alone
#   dead link      ICLFORGE_VCPKG_ROOT as a link whose target is gone says so
#   nothing set    carries on without vcpkg, unless a triplet asks for it
#   no baseline    a store cannot make a checkout of nothing
#   real manifest  the repository's own vcpkg.json is read for its baseline
#
# Run: cmake -DREPO_ROOT=<repo> -DWORK_DIR=<scratch dir> -P check_vcpkg_root.cmake

if(NOT DEFINED REPO_ROOT OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "Pass -DREPO_ROOT=<repo> -DWORK_DIR=<scratch dir>.")
endif()
find_package(Git REQUIRED)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(upstream "${WORK_DIR}/upstream")
set(src "${WORK_DIR}/src")
set(proj "${WORK_DIR}/proj")
set(store "${WORK_DIR}/store")

function(git dir)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c user.name=test -c user.email=test@example.invalid
            -c commit.gpgsign=false -C "${dir}" ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} in ${dir} failed:\n${out}")
    endif()
    set(GIT_OUT "${out}" PARENT_SCOPE)
endfunction()

# A commit of the fake upstream; returns its hash in `out`.
function(upstream_commit label out)
    file(WRITE "${upstream}/scripts/buildsystems/vcpkg.cmake" "message(STATUS \"fake-vcpkg ${label}\")\n")
    git("${upstream}" add -A)
    git("${upstream}" commit -q -m "${label}")
    git("${upstream}" rev-parse HEAD)
    set(${out} "${GIT_OUT}" PARENT_SCOPE)
endfunction()

# What the source tree's vcpkg.json names as its builtin-baseline.
function(set_baseline sha)
    file(WRITE "${src}/vcpkg.json" "{\n  \"name\": \"t\",\n  \"builtin-baseline\": \"${sha}\"\n}\n")
endfunction()

file(MAKE_DIRECTORY "${upstream}" "${src}" "${proj}")
git("${upstream}" init -q)
upstream_commit("one" pin1)
set_baseline("${pin1}")
file(WRITE "${proj}/CMakeLists.txt" [[
cmake_minimum_required(VERSION 3.28)
include("${REPO}/cmake/VcpkgRoot.cmake")
iclforge_resolve_vcpkg("${SRC}")
project(t LANGUAGES NONE)
message(STATUS "root-dir=[${ICLFORGE_VCPKG_ROOT_DIR}]")
]])

# The fork the store's mirror is cloned from; a case that wants another sets it.
set(URL "${upstream}")

# Configures the scratch project; the result is in rc and log. `env` is a list
# of NAME=value for the child; every setting the module reads that is not in it
# is unset (the machine running this may have VCPKG_ROOT and ICL_VCPKG_STORE).
function(configure build env)
    set(unset)
    foreach(name ICLFORGE_VCPKG_ROOT ICLFORGE_VCPKG_STORE ICLFORGE_VCPKG_MIRROR ICLFORGE_VCPKG_URL
                ICL_VCPKG_STORE ICL_VCPKG_MIRROR VCPKG_ROOT)
        list(APPEND unset "--unset=${name}")
    endforeach()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${unset} "ICLFORGE_VCPKG_URL=${URL}" ${env}
            "${CMAKE_COMMAND}" -S "${proj}" -B "${WORK_DIR}/${build}" -G "${GEN}"
            "-DREPO=${REPO_ROOT}" "-DSRC=${src}" ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
    set(rc "${rc}" PARENT_SCOPE)
    set(log "${log}" PARENT_SCOPE)
endfunction()

function(expect_ok what)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${what}: the configure failed.\n${log}")
    endif()
endfunction()

function(expect_fail what)
    if(rc EQUAL 0)
        message(FATAL_ERROR "${what}: the configure should have failed.\n${log}")
    endif()
endfunction()

# CMake wraps long error text at 80 columns, so the output is compared with its
# runs of white space collapsed to one space.
function(_flat_log out)
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${log}")
    set(${out} "${flat}" PARENT_SCOPE)
endfunction()

function(expect_in_log what text)
    _flat_log(flat)
    string(FIND "${flat}" "${text}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${what}: expected '${text}' in the output.\n${log}")
    endif()
endfunction()

function(expect_not_in_log what text)
    _flat_log(flat)
    string(FIND "${flat}" "${text}" at)
    if(NOT at EQUAL -1)
        message(FATAL_ERROR "${what}: did not expect '${text}' in the output.\n${log}")
    endif()
endfunction()

set(GEN "${CMAKE_GENERATOR}")
if(GEN STREQUAL "")
    set(GEN "Ninja")
endif()
string(SUBSTRING "${pin1}" 0 12 short1)

# --- store: first use creates the checkout, the second reuses it ----------
configure(b-store1 "ICLFORGE_VCPKG_STORE=${store}")
expect_ok("store, first use")
expect_in_log("store, first use" "fake-vcpkg one")
expect_in_log("store, first use" "root-dir=[${store}/${short1}]")
if(NOT EXISTS "${store}/${short1}/.iclforge-ready")
    message(FATAL_ERROR "store: ${store}/${short1} was not marked ready.")
endif()

configure(b-store2 "ICLFORGE_VCPKG_STORE=${store}")
expect_ok("store, second build directory")
expect_not_in_log("store, second build directory" "Checking out vcpkg")
expect_in_log("store, second build directory" "root-dir=[${store}/${short1}]")

# --- baseline moved: an old build directory says to start fresh -----------
upstream_commit("two" pin2)
set_baseline("${pin2}")
string(SUBSTRING "${pin2}" 0 12 short2)
configure(b-store1 "ICLFORGE_VCPKG_STORE=${store}")
expect_fail("baseline moved: an old build directory must not keep the old toolchain silently")
expect_in_log("baseline moved" "--fresh")
configure(b-store1 "ICLFORGE_VCPKG_STORE=${store}" --fresh)
expect_ok("baseline moved, --fresh")
expect_in_log("baseline moved, --fresh" "fake-vcpkg two")
expect_in_log("baseline moved, --fresh" "root-dir=[${store}/${short2}]")
if(NOT EXISTS "${store}/${short1}/.iclforge-ready")
    message(FATAL_ERROR "baseline moved: the checkout of the old baseline must be left alone.")
endif()

# --- ICL_VCPKG_STORE, and the project's own setting wins ------------------
set(store_icl "${WORK_DIR}/store-icl")
configure(b-icl "ICL_VCPKG_STORE=${store_icl}")
expect_ok("ICL_VCPKG_STORE")
expect_in_log("ICL_VCPKG_STORE" "root-dir=[${store_icl}/${short2}]")
configure(b-both "ICL_VCPKG_STORE=${store_icl}-unused;ICLFORGE_VCPKG_STORE=${store}")
expect_ok("both stores set")
expect_in_log("both stores set" "root-dir=[${store}/${short2}]")
if(EXISTS "${store_icl}-unused")
    message(FATAL_ERROR "both stores set: ICL_VCPKG_STORE must be ignored when ICLFORGE_VCPKG_STORE is set.")
endif()

# --- other fork: the baseline is in a repository the mirror does not know -
set(upstream_b "${WORK_DIR}/upstream-b")
file(MAKE_DIRECTORY "${upstream_b}")
git("${upstream_b}" init -q)
set(upstream_save "${upstream}")
set(upstream "${upstream_b}")
upstream_commit("other fork" pin_b)
set(upstream "${upstream_save}")
set_baseline("${pin_b}")
string(SUBSTRING "${pin_b}" 0 12 short_b)
set(URL "${upstream_b}")
configure(b-other "ICLFORGE_VCPKG_STORE=${store}")
expect_ok("other fork")
expect_in_log("other fork" "fake-vcpkg other fork")
expect_in_log("other fork" "root-dir=[${store}/${short_b}]")
set(URL "${upstream}")
set_baseline("${pin2}")

# --- root: another commit than the baseline is a warning -----------------
configure(b-root "ICLFORGE_VCPKG_ROOT=${store}/${short1}")
expect_ok("root at another commit")
expect_in_log("root at another commit" "builtin-baseline is ${pin2}")
expect_in_log("root at another commit" "root-dir=[${store}/${short1}]")
configure(b-root-ok "ICLFORGE_VCPKG_ROOT=${store}/${short2}")
expect_ok("root at the baseline")
expect_not_in_log("root at the baseline" "builtin-baseline is")
expect_in_log("root at the baseline" "root-dir=[${store}/${short2}]")

# --- VCPKG_ROOT: the last resort, and never over a store ------------------
configure(b-env-ok "VCPKG_ROOT=${store}/${short2}")
expect_ok("VCPKG_ROOT at the baseline")
expect_not_in_log("VCPKG_ROOT at the baseline" "builtin-baseline is")
expect_in_log("VCPKG_ROOT at the baseline" "root-dir=[${store}/${short2}]")
configure(b-env-old "VCPKG_ROOT=${store}/${short1}")
expect_ok("VCPKG_ROOT at another commit")
expect_in_log("VCPKG_ROOT at another commit" "VCPKG_ROOT is at")
expect_in_log("VCPKG_ROOT at another commit" "builtin-baseline is ${pin2}")
configure(b-env-store "ICLFORGE_VCPKG_STORE=${store};VCPKG_ROOT=${WORK_DIR}/nowhere")
expect_ok("a store and a stale VCPKG_ROOT")
expect_in_log("a store and a stale VCPKG_ROOT" "root-dir=[${store}/${short2}]")
configure(b-env-dead "VCPKG_ROOT=${WORK_DIR}/nowhere")
expect_fail("VCPKG_ROOT that holds no vcpkg")
expect_in_log("VCPKG_ROOT that holds no vcpkg" "VCPKG_ROOT is set to ${WORK_DIR}/nowhere")
expect_in_log("VCPKG_ROOT that holds no vcpkg" "does not exist")

# --- toolchains given from outside ---------------------------------------
configure(b-dead "" "-DCMAKE_TOOLCHAIN_FILE=${WORK_DIR}/nowhere/scripts/buildsystems/vcpkg.cmake")
expect_fail("dead toolchain")
expect_in_log("dead toolchain" "does not exist")

file(WRITE "${WORK_DIR}/other.toolchain.cmake" "# not vcpkg's\n")
configure(b-other-tc "VCPKG_ROOT=${store}/${short2}" "-DCMAKE_TOOLCHAIN_FILE=${WORK_DIR}/other.toolchain.cmake")
expect_ok("a toolchain that is not vcpkg's")
expect_in_log("a toolchain that is not vcpkg's" "root-dir=[]")

# --- dead link at ICLFORGE_VCPKG_ROOT -------------------------------------
# The link is a symbolic link where the machine lets this user make one, and
# otherwise a directory junction (what the repo's Windows developers have),
# which needs no privilege. Which of the two is found out by trying, not by
# asking what the OS is: that is a CMake decision made elsewhere.
set(link "${WORK_DIR}/vcpkg-link")
set(target "${WORK_DIR}/link-target")
file(MAKE_DIRECTORY "${target}")
file(CREATE_LINK "${target}" "${link}" SYMBOLIC RESULT made)
if(NOT made STREQUAL "0")
    find_program(CMD_EXECUTABLE cmd)
    if(NOT CMD_EXECUTABLE)
        message(FATAL_ERROR "dead link: cannot make a symbolic link (${made}) and there is no cmd to make a junction.")
    endif()
    string(REPLACE "/" "\\" link_native "${link}")
    string(REPLACE "/" "\\" target_native "${target}")
    execute_process(COMMAND cmd /c mklink /J "${link_native}" "${target_native}"
        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "dead link: could not make a junction at ${link}.")
    endif()
endif()
file(REMOVE_RECURSE "${target}")
configure(b-link "ICLFORGE_VCPKG_ROOT=${link}")
expect_fail("dead link")
expect_in_log("dead link" "is a link to")
expect_in_log("dead link" "does not exist")
file(REMOVE "${link}")
if(IS_SYMLINK "${link}" AND CMD_EXECUTABLE)
    execute_process(COMMAND cmd /c rmdir "${link_native}" OUTPUT_QUIET ERROR_QUIET)
endif()

# --- nothing set: without vcpkg, unless a triplet asks for it -------------
configure(b-none "")
expect_ok("nothing set")
expect_in_log("nothing set" "root-dir=[]")
configure(b-triplet "" -DVCPKG_TARGET_TRIPLET=x64-linux)
expect_fail("a triplet and no vcpkg")
expect_in_log("a triplet and no vcpkg" "VCPKG_TARGET_TRIPLET is x64-linux")
expect_in_log("a triplet and no vcpkg" "ICL_VCPKG_STORE")

# --- no baseline: a store has nothing to make a checkout of ---------------
file(WRITE "${src}/vcpkg.json" "{\n  \"name\": \"t\"\n}\n")
configure(b-nobaseline "ICLFORGE_VCPKG_STORE=${store}")
expect_fail("no builtin-baseline")
expect_in_log("no builtin-baseline" "no builtin-baseline")

# --- the repository's own manifest ----------------------------------------
# Its baseline is the one CI checks out; reading it must work on the file as it
# is written (long comments, features), not only on the fake one above.
file(READ "${REPO_ROOT}/vcpkg.json" manifest)
string(JSON real_baseline GET "${manifest}" "builtin-baseline")
file(COPY "${REPO_ROOT}/vcpkg.json" DESTINATION "${WORK_DIR}/real")
set(src_save "${src}")
set(src "${WORK_DIR}/real")
configure(b-real "ICLFORGE_VCPKG_ROOT=${store}/${short1}")
set(src "${src_save}")
expect_ok("the repository's vcpkg.json")
expect_in_log("the repository's vcpkg.json" "builtin-baseline is ${real_baseline}")

message(STATUS "vcpkg root selection behaves as documented")
