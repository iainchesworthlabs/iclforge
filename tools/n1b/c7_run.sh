#!/bin/bash
# C7: the whole ctest of the host trees and the bare-metal probes, on the tree before a stage and the
# tree after (planning/monorepo.md, "How C7 is run"). Run c7_record.sh on both first.
#
#   tools/n1b/c7_run.sh ctest                 # gcc and llvm, JUnit files in build/work/c7/{before,after}/
#   TREES=gcc tools/n1b/c7_run.sh ctest
#   tools/n1b/c7_run.sh probes                # run_baremetal_probe.sh: decoder, encoder, AC-4 float and
#                                             # fixed, and the decoder with stage timers, each --icount
#
# The probes' output is compared by its key=value lines (instructions per frame, heap, stack), which are
# the same on any host: QEMU's clock is the instruction count.
set -u
REPO=$(cd "$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")" && pwd)
[ -f "$REPO/build/env.sh" ] && source "$REPO/build/env.sh"
W=$REPO/build/wt
O=$REPO/build/work/c7
BEFORE=${BEFORE_WT:-c7-before}
AFTER=${AFTER_WT:-merge}

case "${1:-}" in
ctest)
    for t in ${TREES:-gcc llvm}; do
        for side in before after; do
            wt=$BEFORE
            [ "$side" = after ] && wt=$AFTER
            mkdir -p "$O/$side"
            start=$(date +%s)
            ctest --test-dir "$W/$wt/build/$t" -j "${JOBS:-12}" --timeout 900 \
                --output-junit "$O/$side/$t-ctest.xml" > "$O/$side/$t-ctest.log" 2>&1
            echo "$side $t ctest exit $? in $(( $(date +%s) - start )) s: $(grep -E 'tests passed|tests failed' "$O/$side/$t-ctest.log" | tail -1)"
        done
    done
    ;;
probes)
    declare -a MODES=("dec:--icount" "enc:--encoder --icount" "ac4:--ac4 --icount" "ac4fixed:--ac4 --icount --scalar=fixed" "dec-timers:--stage-timers --icount")
    for side in before after; do
        wt=$BEFORE
        [ "$side" = after ] && wt=$AFTER
        mkdir -p "$O/$side"
        for m in "${MODES[@]}"; do
            name=${m%%:*}
            args=${m#*:}
            (cd "$W/$wt" && bash tools/checks/run_baremetal_probe.sh $args) > "$O/$side/probe-$name.log" 2>&1
            echo "$side $name exit $?: $(grep -E 'result=' "$O/$side/probe-$name.log" | tail -1)"
        done
    done
    for m in dec enc ac4 ac4fixed dec-timers; do
        a=$(grep -E '^[A-Za-z0-9_.-]+=[^ ]*$' "$O/before/probe-$m.log" | grep -v -E '^(CMAKE|VCPKG)' | sort | md5sum)
        b=$(grep -E '^[A-Za-z0-9_.-]+=[^ ]*$' "$O/after/probe-$m.log" | grep -v -E '^(CMAKE|VCPKG)' | sort | md5sum)
        echo "$m: $([ "$a" = "$b" ] && echo identical || echo DIFFERENT)"
    done
    ;;
*)
    echo "usage: c7_run.sh ctest|probes" >&2
    exit 2
    ;;
esac
echo DONE
