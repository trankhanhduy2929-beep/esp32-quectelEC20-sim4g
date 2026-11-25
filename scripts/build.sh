#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
cd esphome

esphome config esp32-s3-ec20.yaml
esphome compile esp32-s3-ec20.yaml
