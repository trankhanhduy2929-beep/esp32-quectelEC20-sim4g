#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
cd esphome

if [ "$#" -ge 1 ]; then
  esphome run esp32-s3-ec20.yaml --device "$1"
else
  esphome run esp32-s3-ec20.yaml
fi
