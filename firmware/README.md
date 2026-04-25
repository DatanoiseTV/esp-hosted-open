# Pre-built firmware

This directory ships the C5 slave firmware as a single merged image
(`c5_slave_merged.bin`, ~1.5 MB) and a one-line flasher
(`flash.sh`). End users do **not** need ESP-IDF installed — only
`esptool` (`pip install esptool`).

## Quick flash

```sh
firmware/flash.sh /dev/cu.usbserial-XXXX
```

The script targets esp32c5, flash size 4 MB, DIO mode at 80 MHz,
writing the merged image to flash offset 0x0.

## What's in the binary

`c5_slave_merged.bin` packs:

| Offset | Component        | Size   |
|--------|------------------|--------|
| 0x2000 | bootloader       | 22 kB  |
| 0xC000 | partition table  |  3 kB  |
| 0x16000| ota_data         |  8 kB  |
| 0x20000| network_adapter  | 1.4 MB |

It is built from `examples/p4_c5_hosted/slave_c5/`, which is the
upstream `esp-hosted-mcu` slave firmware patched with our PHY-hack
overlay (`components/phy_rpc_overlay/`):

- esp-hosted-mcu SDIO slave transport (compatible with stock host
  firmware *plus* our custom CITS RPCs once enabled)
- `phy_11p_set(1, 0)` armed at boot
- channel locked to CCH (180, 5.900 GHz) by default
- AGC max gain → 255 (most permissive RX)
- low-rate filter off (6 Mbps QPSK pass)
- power save off

## Verifying the binary

```sh
shasum -a 256 firmware/c5_slave_merged.bin
```

Compare with the SHA-256 published in the matching release tag.
The file in the repo is the build at the most recent commit; CI also
publishes per-push artefacts under the GitHub Actions tab.

## Rebuilding from source

```sh
. $IDF_PATH/export.sh                # IDF v6.0+
cd examples/p4_c5_hosted/slave_c5
idf.py set-target esp32c5
idf.py menuconfig                    # optional: change channel, etc.
idf.py build
cp build/c5_slave_merged.bin ../../../firmware/
```
