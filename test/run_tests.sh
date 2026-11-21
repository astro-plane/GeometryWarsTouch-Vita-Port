#!/bin/sh
# Headless regression run (needs a C compiler and libpng headers):
#   test/run_tests.sh [app_data dir] [snapshot dir]
# Without app_data the game runs silent and the sound checks are skipped.
set -e
cd "$(dirname "$0")/.."
DATA="${1:-app_data}"
OUT="${2:-/tmp/gwvita_shots}"
case "$DATA" in */) ;; *) DATA="$DATA/" ;; esac
mkdir -p "$OUT"
${CC:-cc} -O1 -g -Wall -Wextra -fsanitize=address,undefined -o "$OUT/gwtest" \
    src/game.c test/plat_soft.c test/test_main.c $(pkg-config --cflags --libs libpng) -lm
GW_DATA="$DATA" "$OUT/gwtest" "$OUT"
echo "snapshots are in $OUT"
