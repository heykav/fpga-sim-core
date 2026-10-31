#!/usr/bin/env bash
# Download a bounded prefix of a NASDAQ TotalView-ITCH 5.0 sample file, decompress it, and replay it.
#
#   scripts/fetch_itch_sample.sh [BYTES] [FILE]
#     BYTES  compressed bytes to fetch (default 134217728 = 128 MiB)
#     FILE   sample name (default 01302019.NASDAQ_ITCH50.gz, 4,764,426,091 bytes in full)
#
# Source: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ (NASDAQ's public sample directory).
# The directory carries no licence and NASDAQ's data policy lists Historical TotalView-ITCH as not
# eligible for redistribution, so this repository stores NO NASDAQ bytes. Fetch it yourself.
# A gzip prefix decompresses to a valid prefix of the stream; the last message is cut at the boundary
# and the tool reports that. Needs: curl, gzip, and a build in ./build (cmake -S . -B build && cmake --build build).
set -euo pipefail
BYTES="${1:-134217728}"
FILE="${2:-01302019.NASDAQ_ITCH50.gz}"
OUT="${ITCH_DIR:-itch-sample}"
mkdir -p "$OUT"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/$FILE"
echo "fetching bytes 0-$((BYTES - 1)) of $FILE from $URL"
curl -fL -r "0-$((BYTES - 1))" -o "$OUT/$FILE.part" "$URL"
shasum -a 256 "$OUT/$FILE.part" 2>/dev/null || sha256sum "$OUT/$FILE.part"
# gzip reports "unexpected end of file" for the cut stream; the decompressed prefix is what we want.
gzip -dc "$OUT/$FILE.part" > "$OUT/slice.itch" 2>/dev/null || true
ls -l "$OUT/slice.itch"
./build/fpga-sim-itch-replay "$OUT/slice.itch" "${@:3}"
