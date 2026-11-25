#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
cd esphome

PORT="${1:-COM6}"
FW="$(find .esphome/build -path '*/build/firmware.factory.bin' | head -n 1)"

if [ -z "$FW" ]; then
  echo "firmware.factory.bin not found; run scripts/build.sh first" >&2
  exit 1
fi

esptool.py --chip esp32s3 --port "$PORT" write_flash 0x0 "$FW"
