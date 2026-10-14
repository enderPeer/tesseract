#!/usr/bin/env bash
# Runs alpha on a few Linux machines reachable by SSH (key login, so no password prompts), from
# Git Bash on Windows or a shell on Linux. Name your machines in tools/cluster.local.sh (see
# tools/cluster.local.example.sh), or on the command line.
#
# Everything on the nodes runs as `nice -n 19 ionice -c3 chrt -i 0` (SCHED_IDLE): alpha only gets
# cycles that nothing else wants, so every other workload keeps its CPU. On top of that, the core
# that a protected process runs on (PROTECT, if set) is left free. Nothing is installed, started as
# a service, or exposed on the network; the build lives in ~/<this folder's name>, e.g. ~/alpha.
#
#   tools/cluster.sh deploy [nodes]   copy the sources to ~/alpha
#   tools/cluster.sh test   [nodes]   build and run the tests
#   tools/cluster.sh scale  [nodes]   1 thread .. every free thread, one node after the other
#   tools/cluster.sh sweep  [nodes]   one thread, memory from L1 to RAM
#   tools/cluster.sh total  [nodes]   every node at once, summed; without named nodes the
#                                     workstation (bin/alpha.exe) runs at the same time
#   tools/cluster.sh tester1 [nodes]  tester 1 on every node (and the workstation) at once, each
#                                     with its own random programs; results added up
#   tools/cluster.sh search <alpha search arguments>   (also verify, equiv) the inputs are split
#                                     between every node and the workstation by speed (WEIGHTS)
#   tools/cluster.sh gamma  [nodes]   alpha gamma (step 11) on each node, one after the other:
#                                     the world checked against plain C, then timed
#
# Settings (environment, or tools/cluster.local.sh): CLUSTER_NODES (your machines, as ssh knows
# them), PROTECT (a process whose core stays free, e.g. a long simulation; default none), PROGRAM
# (default examples/adder8.asm), SECS (seconds per row, default 2), T1SECS (tester 1 run time,
# default 60), T1ARGS (more tester 1 options, e.g. "--cells 64,256,1024 --max-passes 4194304"),
# NODES (nodes for search/verify/equiv), WEIGHTS (shares per machine, from `total`, e.g.
# "workstation=15 node1=10"; default 5 each), GAMMA_ARGS (more alpha gamma options, e.g.
# "--size 64,32,32 --threads 8").
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$here/tools/cluster.local.sh" ] && . "$here/tools/cluster.local.sh"
CLUSTER_NODES=${CLUSTER_NODES:-}
PROTECT=${PROTECT:-}
PROGRAM=${PROGRAM:-examples/adder8.asm}
SECS=${SECS:-2}
dir=$(basename "$here")  # the folder on the nodes: ~/$dir
cmd=${1:-}
shift || true
named=$#
nodes=${*:-$CLUSTER_NODES}
case "$cmd" in
verify | equiv | search)  # the arguments are for alpha; nodes come from NODES
    nodes=${NODES:-$CLUSTER_NODES}
    named=${NODES:+1}
    named=${named:-0}
    ;;
esac
case "$cmd" in
deploy | test | scale | sweep | total | tester1 | search | verify | equiv | gamma)
    if [ -z "$nodes" ]; then
        echo "no nodes: name them (tools/cluster.sh $cmd node1 node2), or set CLUSTER_NODES in" >&2
        echo "tools/cluster.local.sh (copy tools/cluster.local.example.sh)" >&2
        exit 2
    fi
    ;;
esac
WEIGHTS=${WEIGHTS:-}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10"
local_alpha="$here/bin/alpha.exe"
[ -x "$local_alpha" ] || local_alpha="$here/bin/alpha"

weight() {  # machine -> its share of the work
    for kv in $WEIGHTS; do
        if [ "${kv%%=*}" = "$1" ]; then
            echo "${kv#*=}"
            return
        fi
    done
    echo 5
}

# Runs `bin/alpha $@` on the node at idle priority, leaving the protected process's core free.
remote_alpha='
set -e
cd ~/"$1"
shift
ex=""
for p in $( [ -n "$1" ] && pgrep -x "$1" || true); do
    c=$(ps -o psr= -p "$p" | tr -d " ")
    ex="${ex:+$ex,}$(cat /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list)"
done
shift
exec nice -n 19 ionice -c3 chrt -i 0 bin/alpha "$@" ${ex:+--exclude "$ex"}
'

run_alpha() {  # node, alpha arguments... (quoted, so arguments with spaces arrive whole)
    local h=$1 q
    shift
    q=$(printf '%q ' "$dir" "$PROTECT" "$@")
    $SSH "$h" "bash -s -- $q" <<<"$remote_alpha"
}

case "$cmd" in
deploy)
    for h in $nodes; do
        echo "== $h"
        tar -cf - --exclude="$dir/bin" --exclude="$dir/build" --exclude="$dir/.git" \
            -C "$(dirname "$here")" "$dir" |
            $SSH "$h" "tar -xf - -C ~ && echo copied to ~/$dir"
    done
    ;;
test)
    for h in $nodes; do
        echo "== $h"
        $SSH "$h" "cd ~/$dir && nice -n 19 ionice -c3 chrt -i 0 sh build.sh test 2>&1"
    done
    ;;
scale)
    for h in $nodes; do
        echo "== $h"
        run_alpha "$h" scale "$PROGRAM" --idle --seconds "$SECS"
        echo
    done
    ;;
sweep)
    for h in $nodes; do
        echo "== $h"
        run_alpha "$h" sweep "$PROGRAM" --idle --seconds 1
        echo
    done
    ;;
total)
    tmp=$(mktemp -d)
    for h in $nodes; do
        run_alpha "$h" scale "$PROGRAM" --idle --total --seconds "$SECS" >"$tmp/$h" 2>&1 &
    done
    if [ "$named" -eq 0 ] && [ -x "$local_alpha" ]; then
        "$local_alpha" scale "$here/$PROGRAM" --idle --total --seconds "$SECS" \
            >"$tmp/workstation" 2>&1 &
    fi
    wait
    cat "$tmp"/*
    grep -h '^TOTAL' "$tmp"/* | awk '{ ops += $3; threads += $4 }
        END { printf "\ncluster: %d threads, %.2f T 1-bit ops/s\n", threads, ops / 1e12 }'
    rm -rf "$tmp"
    ;;
tester1)
    T1SECS=${T1SECS:-60}
    echo "tester 1 on $nodes$([ "$named" -eq 0 ] && echo " and this workstation") for ${T1SECS} s:" \
        "results at about $(date -d "+$((T1SECS + 15)) seconds" +%H:%M). Ctrl+C stops it."
    tmp=$(mktemp -d)
    i=0
    for h in $nodes; do  # every machine gets its own seed, so its own programs
        i=$((i + 1))
        # shellcheck disable=SC2086
        run_alpha "$h" tester1 --idle --raw --seconds "$T1SECS" --seed "$i" ${T1ARGS:-} \
            >"$tmp/$h" 2>&1 &
    done
    if [ "$named" -eq 0 ] && [ -x "$local_alpha" ]; then
        # shellcheck disable=SC2086
        "$local_alpha" tester1 --idle --raw --seconds "$T1SECS" --seed 100 ${T1ARGS:-} \
            >"$tmp/workstation" 2>&1 &
    fi
    wait
    for f in "$tmp"/*; do  # anything that is not result data is an error message
        if grep -qv '^T1' "$f"; then
            echo "== $(basename "$f"):"
            grep -v '^T1' "$f"
        fi
    done
    "$local_alpha" tester1 --merge "$tmp"/*
    rm -rf "$tmp"
    ;;
verify | equiv | search)
    tmp=$(mktemp -d)
    machines=$nodes
    if [ "$named" -eq 0 ] && [ -x "$local_alpha" ]; then
        machines="$machines workstation"
    fi
    total=0
    for m in $machines; do
        total=$((total + $(weight "$m")))
    done
    echo "$cmd on $machines at idle priority, the inputs split $total ways by speed"
    at=0
    for m in $machines; do  # each machine takes a run of shares in proportion to its speed
        share="$at-$((at + $(weight "$m") - 1))/$total"
        at=$((at + $(weight "$m")))
        if [ "$m" = workstation ]; then
            (cd "$here" && "$local_alpha" "$cmd" "$@" --idle --raw --part "$share")                 >"$tmp/$m" 2>&1 &
        else
            run_alpha "$m" "$cmd" "$@" --idle --raw --part "$share" >"$tmp/$m" 2>&1 &
        fi
    done
    wait
    for f in "$tmp"/*; do
        if grep -qv '^EN' "$f"; then
            echo "== $(basename "$f"):"
            grep -v '^EN' "$f"
        fi
    done
    (cd "$here" && "$local_alpha" "$cmd" "$@" --merge "$tmp"/*)
    rm -rf "$tmp"
    ;;
gamma)
    for h in $nodes; do
        echo "== $h"
        # shellcheck disable=SC2086
        run_alpha "$h" gamma --idle --seconds "$SECS" ${GAMMA_ARGS:-}
        echo
    done
    ;;
*)
    sed -n '2,30p' "$0"
    exit 2
    ;;
esac
