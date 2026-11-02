#!/bin/sh
set -eu
apt-get update
apt-get install -y --no-install-recommends python3-pyte qemu-user
