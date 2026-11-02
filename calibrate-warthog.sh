#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
# Quick provisional plan. Add --full to validate and save a profile.
exec ./brminer --calibrate "$@"
