#!/usr/bin/env bash
#
# One-shot flasher for the C5 slave firmware. No ESP-IDF install needed.
#
# Usage:
#   firmware/flash.sh <serial-port> [baud]
#
# Examples:
#   firmware/flash.sh /dev/cu.usbserial-1410         # macOS, default 460800
#   firmware/flash.sh /dev/ttyUSB0 921600            # Linux, faster
#
# Requires `esptool` (pip install esptool, or via IDF's bundled tooling).

set -euo pipefail

PORT="${1:-}"
BAUD="${2:-460800}"

if [[ -z "$PORT" ]]; then
    echo "usage: $0 <serial-port> [baud]" >&2
    echo "ports likely to be your C5:" >&2
    if [[ "$OSTYPE" == "darwin"* ]]; then
        ls /dev/cu.usbserial-* /dev/cu.usbmodem* 2>/dev/null | sed 's/^/  /' >&2 || true
    else
        ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | sed 's/^/  /' >&2 || true
    fi
    exit 64
fi

BIN="$(cd "$(dirname "$0")" && pwd)/c5_slave_merged.bin"
if [[ ! -f "$BIN" ]]; then
    echo "missing: $BIN" >&2
    echo "regenerate with: cd examples/p4_c5_hosted/slave_c5 && idf.py build && cp build/c5_slave_merged.bin ../../../firmware/" >&2
    exit 65
fi

if ! command -v esptool >/dev/null && ! command -v esptool.py >/dev/null; then
    echo "esptool not found in PATH. Install with: pip install esptool" >&2
    exit 66
fi

ESPTOOL=$(command -v esptool || command -v esptool.py)

echo ">>> flashing $BIN to $PORT @ ${BAUD} baud..."
"$ESPTOOL" --chip esp32c5 -p "$PORT" -b "$BAUD" \
    --before default-reset --after hard-reset \
    write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m \
    0x0 "$BIN"

echo
echo ">>> done. The C5 should now be running the V2X slave firmware:"
echo "    - SDIO slave transport"
echo "    - 11p PHY hack armed on boot"
echo "    - default channel: CCH (180, 5.900 GHz; change via menuconfig + rebuild)"
