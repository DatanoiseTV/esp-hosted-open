#!/usr/bin/env bash
#
# One-shot flasher for an esp-hosted-open slave firmware. No ESP-IDF
# install needed — only `esptool` (`pip install esptool`).
#
# Usage:
#   firmware/flash.sh <serial-port> [chip] [baud]
#
# Examples:
#   firmware/flash.sh /dev/cu.usbserial-1410                 # default chip=esp32c5
#   firmware/flash.sh /dev/cu.usbserial-1410 esp32c6         # C6 (Wi-Fi 6 + BLE + 802.15.4)
#   firmware/flash.sh /dev/ttyUSB0 esp32c5 921600            # Linux, fast

set -euo pipefail

PORT="${1:-}"
CHIP="${2:-esp32c5}"
BAUD="${3:-460800}"

if [[ -z "$PORT" ]]; then
    echo "usage: $0 <serial-port> [chip] [baud]" >&2
    echo "  chip defaults to esp32c5; supported: esp32c5, esp32c6" >&2
    if [[ "$OSTYPE" == "darwin"* ]]; then
        ls /dev/cu.usbserial-* /dev/cu.usbmodem* 2>/dev/null | sed 's/^/  /' >&2 || true
    else
        ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | sed 's/^/  /' >&2 || true
    fi
    exit 64
fi

case "$CHIP" in
    esp32c5) BIN_NAME="c5_slave_merged.bin" ;;
    esp32c6) BIN_NAME="c6_slave_merged.bin" ;;
    *)
        echo "unsupported chip: $CHIP" >&2
        echo "available: esp32c5, esp32c6" >&2
        exit 64
        ;;
esac

BIN="$(cd "$(dirname "$0")" && pwd)/$BIN_NAME"
if [[ ! -f "$BIN" ]]; then
    echo "missing: $BIN" >&2
    echo "regenerate with: cd slave && idf.py set-target $CHIP && idf.py build && cp build/${CHIP/esp32/c}_slave_merged.bin ../firmware/" >&2
    exit 65
fi

if ! command -v esptool >/dev/null && ! command -v esptool.py >/dev/null; then
    echo "esptool not found in PATH. Install with: pip install esptool" >&2
    exit 66
fi

ESPTOOL=$(command -v esptool || command -v esptool.py)

echo ">>> flashing $BIN_NAME to $PORT (chip=$CHIP, baud=$BAUD)..."
"$ESPTOOL" --chip "$CHIP" -p "$PORT" -b "$BAUD" \
    --before default-reset --after hard-reset \
    write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m \
    0x0 "$BIN"

echo
echo ">>> done. The slave is now running esp-hosted-open ($CHIP)."
echo "    - SDIO slave transport, peer-data channel armed"
case "$CHIP" in
    esp32c5)
        echo "    - 11p PHY hack armed at boot, channel = CCH (180, 5.900 GHz)"
        echo "    - 802.15.4 RPCs return NOT_SUPPORTED (no 15.4 silicon)"
        ;;
    esp32c6)
        echo "    - 802.15.4 RPCs ENABLED (Thread / Zigbee co-processor mode)"
        echo "    - 802.11p RPCs available, but no 5 GHz radio (2.4 GHz only)"
        ;;
esac
