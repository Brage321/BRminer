#!/bin/sh
set -eu
duration=$1
node=$2
wallet=$3
threads=$4
mkdir -p /work/live
cp /work/BRminer/brminer /work/live/brminer
exec timeout --signal=INT --kill-after=10s "$duration" /work/live/brminer \
    -a warthog -p "$node" -w "$wallet" --cpu_threads "$threads"
