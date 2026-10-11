# tools/fuzz/harnesses.sh - sourced by run.sh and generate-seeds.sh: where a harness lives.
# shellcheck shell=bash
#
# Each harness is its library's (planning/monorepo.md, C7-1): libs/<lib>/fuzz/<harness>.cpp, with its
# seeds in libs/<lib>/fuzz/seeds/<harness>/ and the inputs that once broke it in
# libs/<lib>/fuzz/regressions/<harness>/.

# fuzz_dir_of <harness>: the fuzz/ directory that holds it.
fuzz_dir_of() {
    local source
    for source in "$REPO_ROOT"/libs/*/fuzz/"$1".cpp; do
        if [[ -f "$source" ]]; then
            dirname "$source"
            return 0
        fi
    done
    echo "error: no harness $1 under libs/*/fuzz/" >&2
    return 1
}
