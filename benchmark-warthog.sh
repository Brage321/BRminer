#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
# Offline benchmark: 30 seconds warmup, then five measured minutes.
exec ./brminer --benchmark --backend cuda --duration 300 --warmup 30 --json benchmark.json "$@"
