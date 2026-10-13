#!/usr/bin/env bash
# Prunes this repository's GitHub Actions caches so the 10 GB quota is spent on
# entries something can still restore. Run by the cache-hygiene workflow and
# usable by hand (needs `gh`, authenticated with actions:write, and `jq`).
#
#   prune-actions-cache.sh --dry-run               # show what would go
#   prune-actions-cache.sh                         # delete it
#   prune-actions-cache.sh --purge vcpkg-Windows-  # a manual reset, see rule 3
#
# The same file is kept in every repository of the family (cubeulator, ICL Forge,
# aqualink-automate, CountdownSolver); the key shapes it knows are listed under
# rule 2, and a repository simply has none of the ones it does not use.
#
# What it removes, in order:
#
#   1. Entries whose ref no longer exists: a deleted branch, a closed or merged
#      pull request (refs/pull/N/merge), a deleted tag, a merge-queue branch.
#      An entry is readable only from its own ref, the PR's base and the default
#      branch, so once its ref is gone nothing can ever restore it - it just
#      holds quota until GitHub's LRU eviction gets to it, and that eviction
#      does not look at who is useful: it took a one-hour macOS vcpkg cache
#      within two hours of it being saved.
#   2. Superseded generations. A cache that is saved under a new key whenever
#      its content changes (a content hash, a commit, a manifest hash) leaves a
#      full copy behind each time, and a restore takes only the newest. Per ref,
#      per family and per cache version (the version is a hash of the cached
#      paths, so a run with other paths never restores that entry anyway) the
#      newest entry is kept and older ones go, unless something has read them
#      in the last --grace-minutes (a job that is mid-restore must not lose the
#      entry it is downloading). The families, by key shape:
#
#        vcpkg-binary-cache-<triplet>-<16 hex>        cubeulator
#        vcpkg-binary-cache-<preset>-<64 hex>         CountdownSolver
#        vcpkg[-downloads]-<OS>-<ARCH>-<40 hex>-<64 hex>[-<lineage>][-<16 hex>]
#                                                     ICL Forge, aqualink-automate
#        vcpkg-downloads-<40 hex>-<64 hex>            aqualink-automate
#        ccache-<job>-<OS>-<ARCH>-<40 hex>            ICL Forge
#        install-qt-action-<os>-...-<desktop|android|ios>-...
#        cache-trivy-<date>                           aqualink-automate
#   3. Anything matching --purge PREFIX, on every ref. This is the manual reset
#      for a vcpkg cache that has carried stale archives for too long: archives
#      are never garbage-collected, because vcpkg has no notion of "no longer
#      used". The next run on that triplet rebuilds from cold.
#
# Entries of any other family (setup-uv, setup-node, CodeQL, ...) are only ever
# removed by rule 1 or --purge.
#
# For tests and offline use the four live lookups can be replaced with files:
# --caches-file (JSON array as `gh cache list --json` prints it), --refs-file
# (one live ref per line: refs/heads/x, refs/tags/x, refs/pull/N/merge).
set -euo pipefail

REPO="${GITHUB_REPOSITORY:-}"
DRY_RUN=0
GRACE_MIN=20
QUOTA_BYTES=$((10 * 1024 * 1024 * 1024))
WARN_RATIO_PERCENT=80
CACHES_FILE=""
REFS_FILE=""
PURGE=()

usage() {
    sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --repo) REPO="${2:?--repo needs OWNER/REPO}"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        --grace-minutes) GRACE_MIN="${2:?--grace-minutes needs a number}"; shift 2 ;;
        --purge) PURGE+=("${2:?--purge needs a key prefix}"); shift 2 ;;
        --caches-file) CACHES_FILE="${2:?}"; shift 2 ;;
        --refs-file) REFS_FILE="${2:?}"; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "prune-actions-cache.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$CACHES_FILE$REFS_FILE" ] && [ -z "$REPO" ]; then
    echo "prune-actions-cache.sh: set GITHUB_REPOSITORY or pass --repo OWNER/REPO." >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# jq built for Windows ends its text output lines with CRLF, which would turn a
# cache id into "123\r" and a number into a non-number.
jq_text() { jq "$@" | tr -d '\r'; }

# --- What exists --------------------------------------------------------------

if [ -n "$CACHES_FILE" ]; then
    cp "$CACHES_FILE" "$WORK/caches.json"
else
    gh cache list -R "$REPO" --limit 1000 \
        --json id,key,ref,version,sizeInBytes,createdAt,lastAccessedAt > "$WORK/caches.json"
fi

# --- What is still alive ------------------------------------------------------
#
# Refuse to run on an empty answer: an API hiccup that returned no branches
# would otherwise read as "every branch was deleted" and wipe the lot.

if [ -n "$REFS_FILE" ]; then
    cp "$REFS_FILE" "$WORK/refs.txt"
else
    {
        gh api --paginate "repos/$REPO/git/matching-refs/heads/" --jq '.[].ref'
        gh api --paginate "repos/$REPO/git/matching-refs/tags/" --jq '.[].ref'
        gh pr list -R "$REPO" --state open --limit 1000 --json number \
            --jq '.[] | "refs/pull/\(.number)/merge"'
    } > "$WORK/refs.txt"
fi

if ! grep -q '^refs/heads/' "$WORK/refs.txt"; then
    echo "prune-actions-cache.sh: the live ref list has no branches; refusing to prune on that." >&2
    exit 1
fi

# --- What to remove -----------------------------------------------------------

PURGE_JSON="$(printf '%s\n' "${PURGE[@]+"${PURGE[@]}"}" | jq -R . | jq -s 'map(select(length > 0))')"

# shellcheck disable=SC2016  # jq program, not shell
JQ_PLAN='
def family:
  .key as $k
  | if   ($k | test("^vcpkg-binary-cache-.+?-[0-9a-f]{64}"))
      then "vcpkg:" + ($k | capture("^vcpkg-binary-cache-(?<p>.+?)-[0-9a-f]{64}").p)
    elif ($k | test("^vcpkg-binary-cache-.+-[0-9a-f]{16}$"))
      then "vcpkg:" + ($k | capture("^vcpkg-binary-cache-(?<t>.+)-[0-9a-f]{16}$").t)
    elif ($k | test("^vcpkg(-downloads)?-[A-Za-z0-9]+-[A-Za-z0-9]+-[0-9a-f]{40}-[0-9a-f]{64}"))
      then ($k | capture("^(?<kind>vcpkg(?:-downloads)?)-(?<os>[A-Za-z0-9]+)-(?<arch>[A-Za-z0-9]+)-[0-9a-f]{40}-[0-9a-f]{64}(?:-(?<suffix>.+?))?(?:-[0-9a-f]{16})?$")
            | .kind + ":" + .os + "-" + .arch + (if .suffix then "-" + .suffix else "" end))
    elif ($k | test("^vcpkg-downloads-[0-9a-f]{40}-[0-9a-f]{64}"))
      then "vcpkg-downloads"
    elif ($k | test("^ccache-.+-[0-9a-f]{40}$"))
      then "ccache:" + ($k | capture("^ccache-(?<j>.+)-[0-9a-f]{40}$").j)
    elif ($k | test("^install-qt-action-"))
      then (($k | capture("^install-qt-action-(?<os>[a-z0-9_]+)-.*?-(?<target>desktop|android|ios)-")? // null)
            | if . then "qt:" + .os + ":" + .target else null end)
    elif ($k | test("^cache-trivy-[0-9]{4}-[0-9]{2}-[0-9]{2}$"))
      then "trivy"
    else null end;

def orphan:
  (.ref | test("^refs/(heads|tags|pull)/")) and ((.ref | IN($live[])) | not);

def purged:
  .key as $k | any($purge[]; . as $p | $k | startswith($p));

# Rules 3 and 1: decided entry by entry.
map(. + {family: family})
| map(. + {drop: (if purged then "purged" elif orphan then "ref gone" else null end)})
# Rule 2: among the survivors, newest per (ref, family) stays.
| ( map(select(.drop == null and .family != null))
    | group_by([.ref, .family, (.version // "")])
    | map(sort_by(.createdAt) | reverse | .[1:][])
    | map(.id) ) as $older
| map(
    if .drop == null and (.id | IN($older[])) then
      (if (($now - (.lastAccessedAt | fromdateiso8601)) < $grace)
       then . + {drop: null, kept: "read in the last grace window"}
       else . + {drop: "superseded"} end)
    else . end)
'

jq -n \
    --slurpfile caches "$WORK/caches.json" \
    --rawfile refs "$WORK/refs.txt" \
    --argjson purge "$PURGE_JSON" \
    --argjson now "$(date +%s)" \
    --argjson grace "$((GRACE_MIN * 60))" \
    '($refs | split("\n") | map(select(length > 0))) as $live
     | ($caches[0] | map(. + {lastAccessedAt: (.lastAccessedAt | sub("\\.[0-9]+Z$"; "Z")), createdAt: (.createdAt | sub("\\.[0-9]+Z$"; "Z"))}))
     | '"$JQ_PLAN" > "$WORK/plan.json"

mb() { awk -v b="$1" 'BEGIN { printf "%.0f", b / 1048576 }'; }

TOTAL_BEFORE="$(jq_text '[.[].sizeInBytes] | add // 0' "$WORK/plan.json")"
COUNT_BEFORE="$(jq_text 'length' "$WORK/plan.json")"
DROP_BYTES="$(jq_text '[.[] | select(.drop != null) | .sizeInBytes] | add // 0' "$WORK/plan.json")"
DROP_COUNT="$(jq_text '[.[] | select(.drop != null)] | length' "$WORK/plan.json")"
TOTAL_AFTER=$((TOTAL_BEFORE - DROP_BYTES))

{
    echo "### Actions cache"
    echo
    echo "| | entries | size |"
    echo "| --- | ---: | ---: |"
    echo "| before | $COUNT_BEFORE | $(mb "$TOTAL_BEFORE") MB |"
    echo "| $([ "$DRY_RUN" = 1 ] && echo 'would remove' || echo 'removed') | $DROP_COUNT | $(mb "$DROP_BYTES") MB |"
    echo "| after | $((COUNT_BEFORE - DROP_COUNT)) | $(mb "$TOTAL_AFTER") MB of $(mb "$QUOTA_BYTES") MB |"
    if [ "$DROP_COUNT" -gt 0 ]; then
        echo
        echo "| reason | size | ref | key |"
        echo "| --- | ---: | --- | --- |"
        # shellcheck disable=SC2016  # the backticks are markdown inside a jq string, not shell
        jq_text -r '.[] | select(.drop != null)
               | "| \(.drop) | \((.sizeInBytes / 1048576 | floor)) MB | `\(.ref)` | `\(.key | .[0:90])` |"' "$WORK/plan.json"
    fi
    echo
    echo "Largest families kept:"
    echo
    echo "| family | entries | size |"
    echo "| --- | ---: | ---: |"
    jq_text -r '[.[] | select(.drop == null)]
           | group_by(.family // "other")
           | map({f: (.[0].family // "other"), n: length, b: (map(.sizeInBytes) | add)})
           | sort_by(-.b) | .[:8][]
           | "| \(.f) | \(.n) | \((.b / 1048576 | floor)) MB |"' "$WORK/plan.json"
} > "$WORK/summary.md"

cat "$WORK/summary.md"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    cat "$WORK/summary.md" >> "$GITHUB_STEP_SUMMARY"
fi

# --- Remove -------------------------------------------------------------------

if [ "$DRY_RUN" = 1 ]; then
    echo "dry run: nothing deleted."
else
    failed=0
    while IFS= read -r id; do
        if ! err="$(gh cache delete "$id" -R "$REPO" 2>&1)"; then
            # Evicted or deleted by a concurrent run in the meantime is not worth
            # failing the job for; anything else is in the message.
            echo "::warning::could not delete cache $id: $err"
            failed=$((failed + 1))
        fi
    done < <(jq_text -r '.[] | select(.drop != null) | .id' "$WORK/plan.json")
    echo "deleted $((DROP_COUNT - failed)) of $DROP_COUNT cache entries."
fi

if [ $((TOTAL_AFTER * 100)) -gt $((QUOTA_BYTES * WARN_RATIO_PERCENT)) ]; then
    echo "::warning::Actions cache holds $(mb "$TOTAL_AFTER") MB of the $(mb "$QUOTA_BYTES") MB quota after pruning; GitHub evicts least-recently-used entries past the limit, which is how a good vcpkg cache gets lost. Look at the 'Largest families kept' table."
fi
