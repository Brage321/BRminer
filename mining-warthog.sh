#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
# Replace the wallet below with your Warthog payout address.
exec ./brminer -a warthog -p mine.bobfarm.icu:3033 -w ENTER-YOUR-WARTHOG-WALLET "$@"
