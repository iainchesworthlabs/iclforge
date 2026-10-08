#!/bin/bash
# C7: configure, build and record the three host trees of a worktree, as C0 to C3 did
# (planning/monorepo.md, "How C7 is run").
#
#   WT=c7-before OUT=before tools/n1b/c7_record.sh        # the tree before a stage
#   WT=merge     OUT=after  tools/n1b/c7_record.sh        # the stage's tree
#   TREES="gcc llvm" ...                                  # only some of gcc, llvm, llvm-shared
#
# WT is a directory of build/wt/ in the main checkout; OUT a directory of build/work/c7/. Each tree
# is configured with the presets of CMakePresets.json (GCC 16 and Clang 22, Release; Clang 22 shared,
# Debug), built with `-k 0` so that every target that can build does, and recorded: the names ctest
# lists, and baseline.py's pinned hashes, the 44-command CLI corpus, the installed tree and, for the
# shared tree, the exported symbols. `baseline.py compare build/work/c7/before build/work/c7/after`
# compares two. The vcpkg installs of the C0 worktree are reused (VCPKG_MANIFEST_INSTALL=OFF).
set -u
REPO=$(cd "$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")" && pwd)
[ -f "$REPO/build/env.sh" ] && source "$REPO/build/env.sh"
W=$REPO/build/wt/${WT:-c7-before}
O=$REPO/build/work/c7/${OUT:-before}
mkdir -p "$O"
PKGS=${VCPKG_PKGS_FROM:-$REPO/build/wt/c0-before}
LLVM_PKGS=$PKGS/build/config-linux-llvm/vcpkg_installed
GCC_PKGS=$PKGS/build/config-linux-gcc/vcpkg_installed
LLVM="-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja -DCMAKE_CXX_COMPILER=/usr/bin/clang++-22 -DCMAKE_C_COMPILER=/usr/bin/clang-22 -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR=$LLVM_PKGS -DICLFORGE_BUILD_ADM=ON"
GCC="-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja -DCMAKE_CXX_COMPILER=/usr/bin/g++-16 -DCMAKE_C_COMPILER=/usr/bin/gcc-16 -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR=$GCC_PKGS -DICLFORGE_BUILD_ADM=ON"
cd "$W" || exit 1

tree() {  # tree <dir> <preset> <cmake args...>
    local dir=$1 preset=$2
    shift 2
    cmake --preset "$preset" -B "build/$dir" "$@" > "$O/$dir-configure.log" 2>&1 || { echo "$dir configure failed"; return 1; }
    cmake --build "build/$dir" -- -k 0 > "$O/$dir-build.log" 2>&1
    echo "$dir build $?"
    ctest --test-dir "build/$dir" -N | sed -n 's/^ *Test *#[0-9]*: //p' | sort > "$O/$dir-tests.txt"
    echo "$dir tests $(wc -l < "$O/$dir-tests.txt")"
}

for t in ${TREES:-gcc llvm llvm-shared}; do
    case $t in
        gcc) tree gcc config-linux-gcc $GCC && python3 tools/n1b/baseline.py record --build build/gcc --out "$O" --label g16 --only hashes,cli,install ;;
        llvm) tree llvm config-linux-llvm $LLVM && python3 tools/n1b/baseline.py record --build build/llvm --out "$O" --label c22 --only hashes,cli,install ;;
        llvm-shared) tree llvm-shared config-linux-llvm-shared $LLVM && python3 tools/n1b/baseline.py record --build build/llvm-shared --out "$O" --label c22s --only symbols,install ;;
    esac
done
python3 tools/n1b/baseline.py record --root . --only headers --out "$O"
echo DONE
