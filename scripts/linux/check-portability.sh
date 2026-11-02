#!/bin/sh
set -eu
# These tests do not launch GPU kernels and can run beside the node-session check.
ctest --test-dir /work/build --output-on-failure -E 'warthog-(gpu|cuda)-tests'
