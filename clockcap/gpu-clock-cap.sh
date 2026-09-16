#!/usr/bin/env bash
# Cap the GPU SM clock (system-wide, survives no reboot unless the unit below is installed).
#   sudo ./gpu-clock-cap.sh 2400      lock SM clock to <= 2400 MHz
#   sudo ./gpu-clock-cap.sh reset     back to default (3003 MHz on DGX Spark)
set -e
case "${1:-}" in
  reset) nvidia-smi -rgc >/dev/null && echo "GPU clock cap removed" ;;
  ''|*[!0-9]*) echo "usage: $0 <max MHz>|reset" >&2; exit 1 ;;
  *) nvidia-smi -pm 1 >/dev/null || true
     nvidia-smi -lgc 0,"$1" >/dev/null && echo "GPU SM clock capped at $1 MHz" ;;
esac
nvidia-smi --query-gpu=clocks.max.sm,clocks.sm,power.draw,temperature.gpu --format=csv
