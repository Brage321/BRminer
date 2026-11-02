#!/bin/sh
set -eu
mkdir -p /work/BRminer
for path in src cmake tests scripts licenses docs; do
    rsync -a "/workspace/$path/" "/work/BRminer/$path/"
done
cp /workspace/CMakeLists.txt /workspace/README.md /workspace/LICENSE /work/BRminer/
for launcher in mining-warthog.sh benchmark-warthog.sh calibrate-warthog.sh; do
    cp "/workspace/$launcher" "/work/BRminer/$launcher"
done
# Reuse development caches when present; a fresh clone fetches the pinned sources.
set -- -S /work/BRminer -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
for entry in "connectcoin_source:/workspace/build-msvc/_deps/connectcoin_source-src" \
             "janusminer_source:/workspace/out/build/verify-msvc/_deps/janusminer_source-src"; do
    name=${entry%%:*}
    cache=${entry#*:}
    if [ -d "$cache" ]; then
        destination="/work/deps/$name"
        mkdir -p "$destination"
        rsync -a --exclude=.git "$cache/" "$destination/"
        upper=$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')
        set -- "$@" "-DFETCHCONTENT_SOURCE_DIR_$upper=$destination"
    fi
done
cmake "$@"
cmake --build /work/build --parallel 4
ctest --test-dir /work/build --output-on-failure
