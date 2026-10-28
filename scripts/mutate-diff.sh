#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make mutate-diff: run the mutation gate where it can run in parallel.
#
# mutate_diff.py mutates several hunks at once only from reflink copies of the
# tree (GNU cp --reflink, on btrfs, XFS or ZFS). Anywhere else it runs one hunk
# at a time, and a batch takes hours. Such a host hands the run to the test host
# (scripts/test-host-run.sh mutate), which has the copies and the cores. That run
# mutates the PUSHED HEAD, so this refuses when HEAD is not pushed or tracked code
# is uncommitted, rather than reporting on something other than the tree here.
#
#   MUTATE_LOCAL=1   run here anyway, serially
#
# Arguments pass to mutate_diff.py unchanged.
set -u

cd "$(git rev-parse --show-toplevel)" || exit 2

# Exit 0 when cp can make a reflink copy inside build/, where the workers' copies go.
can_reflink() {
    local d rc
    mkdir -p build
    d=$(mktemp -d build/.reflink-probe.XXXXXX) || return 1
    printf 'x' > "$d/a"
    cp --reflink=always "$d/a" "$d/b" 2>/dev/null
    rc=$?
    rm -rf -- "$d"
    return $rc
}

if [ "${MUTATE_LOCAL:-0}" = 1 ] || can_reflink; then
    exec python3 scripts/mutate_diff.py "$@"
fi

# shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/build_hosts.sh"
load_build_hosts
if [ -z "${HELIX_TEST_HOST:-}" ]; then
    echo "mutate-diff: this host cannot make reflink copies, so the gate would run one" >&2
    echo "  hunk at a time, and no test host is configured to hand it to" >&2
    echo "  (docs/devel/BUILD_SYSTEM.md, \"Using your own build/test host\")." >&2
    echo "  MUTATE_LOCAL=1 runs it here serially." >&2
    exit 2
fi

dirty=$(git status --porcelain --untracked-files=no -- src include ui_xml assets/config scripts)
if [ -n "$dirty" ]; then
    echo "mutate-diff: the test host mutates the pushed HEAD, and these are uncommitted:" >&2
    printf '%s\n' "$dirty" | sed 's/^/    /' >&2
    echo "  Commit and push them, or MUTATE_LOCAL=1 to run here serially." >&2
    exit 2
fi
if [ -z "$(git branch -r --contains HEAD 2>/dev/null)" ]; then
    echo "mutate-diff: the test host mutates the pushed HEAD, and $(git rev-parse --short HEAD) is not pushed." >&2
    echo "  Push it, or MUTATE_LOCAL=1 to run here serially." >&2
    exit 2
fi

echo "mutate-diff: no reflink copies here; running on the test host ($HELIX_TEST_HOST)" >&2
exec scripts/test-host-run.sh mutate "$@"
