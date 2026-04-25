# esp-hosted-open

**A patched fork of [esp-hosted-mcu] that lets a host MCU drive the radio
of an ESP32-C5 co-processor at a level Espressif's stock firmware
deliberately hides.**

[esp-hosted-mcu]: https://github.com/espressif/esp-hosted-mcu

Stock esp-hosted gives you `esp_wifi_*` over SDIO. That's wonderful for
making a P4 act like a Wi-Fi STA, and useless if what you actually want
is a 5.9 GHz V2X radio, a monitor-mode sensor, a sub-GHz hack, an SDR
trigger, or anything else that lives below the regulated API surface.
This fork carries one extra component (`phy_rpc_overlay/`) and a host-
side stub library; together they ship a tiny custom RPC channel that
exposes the **undocumented PHY symbols** in `libphy.a` to the host.

---

## ⚠️ Experimental — research / lab use only

Read this carefully.

| | |
|---|---|
| **Undocumented internals.** | We call symbols (`phy_11p_set`, `phy_change_channel`, `phy_force_rx_gain`, `phy_disable_cca`, `phy_bb_bss_cbw40`, …) that aren't in any public header. Espressif may rename, remove, or change them between IDF releases without warning. We pin against IDF v6.0 and treat each upgrade as something that needs re-validation. |
| **Regulatory risk is on you.** | This firmware will happily tune to 5.9 GHz ITS channels, disable carrier sense, override the regulatory country database, and force RX gain past safe operating limits. Doing any of that on the air without a license is illegal almost everywhere. Use a shielded enclosure, a fully-cabled setup with attenuators, or a regulator-issued research permit. |
| **Not safety-certified.** | Don't put this in a vehicle, drone, ISM-band product, medical device, or anything else that hurts people or property when it misbehaves. We provide no warranty, express or implied — Apache-2.0, "AS IS". |
| **Don't blame Espressif.** | The bugs you find here are *ours*, not theirs. Don't open issues against esp-hosted-mcu or the IDF for behaviour we introduced. |
| **Spectrum manners.** | If you actually emit on the air, do it deliberately, briefly, and on bands you're entitled to. Stomping on real V2X traffic is dangerous; stomping on real Wi-Fi is rude. |

By using this code you accept that you alone are responsible for any
RF emissions and for compliance with your local regulations.

---

## What you get

A drop-in replacement for the upstream esp-hosted "host" component
plus a slave overlay, with these additional capabilities:

```c
#include "esp_hosted.h"          /* upstream */
#include "esp_hosted_open.h"     /* this fork */

esp_hosted_init();
esp_hosted_open_init();

esp_hosted_open_set_country_permissive();   /* "01", manual policy */
esp_hosted_open_set_phy_11p(true);          /* phy_11p_set(1, 0)   */
esp_hosted_open_set_channel(180);           /* phy_change_channel  */
esp_hosted_open_set_agc_max_gain(255);
esp_hosted_open_set_low_rate(false);        /* let 6 Mbps QPSK pass */
esp_hosted_open_set_cca(false);             /* loopback only        */

int8_t rssi;
esp_hosted_open_get_phy_rssi(&rssi);
```

Each call rides over SDIO using esp-hosted's generic peer-data channel,
hits a handler on the slave that calls into `libphy.a` directly, and
returns synchronously with `esp_err_t`. The full set of RPCs today:

| RPC                              | Slave-side action |
|----------------------------------|-------------------|
| `set_channel(uint8_t)`           | `phy_change_channel(ch, 1, 0, 0)` after `set_channel(140)` bootstrap |
| `set_phy_11p(bool)`              | `phy_11p_set(en, 0)` |
| `set_country_permissive()`       | `esp_wifi_set_country({"01", manual})` |
| `set_tx_power(int8_t dBm)`       | `esp_wifi_set_max_tx_power` |
| `set_rx_gain(uint8_t)`           | `phy_force_rx_gain` + AGC off (0xFF re-arms AGC) |
| `set_agc_max_gain(uint8_t)`      | `phy_agc_max_gain_set` |
| `set_cca(bool)`                  | `phy_enable_cca` / `phy_disable_cca` |
| `get_cca_counters(busy, total)`  | `phy_get_cca_cnt` |
| `reset_cca_counters()`           | `phy_set_cca_cnt(0)` |
| `set_low_rate(bool)`             | `phy_disable_low_rate` / `phy_enable_low_rate` |
| `set_bandwidth(20\|40)`          | `phy_bb_bss_cbw40` |
| `get_phy_rssi(int8_t *)`         | `phy_get_sigrssi` (fallback `phy_get_rssi`) |
| `get_info(...)`                  | dumps current slave state for debugging |

Plus an async event channel from slave to host for forwarding raw
captured 802.11 frames (`PHY_RPC_EVT_OCB_FRAME`).

## Architecture

```
       ┌──────────────────────────┐         ┌──────────────────────────┐
       │   Host MCU (P4, S3, …)   │         │     ESP32-C5 slave       │
       │                          │  SDIO   │                          │
       │  your application code   │ ◄────► │  upstream esp-hosted      │
       │  esp_hosted_open_*()     │  4-bit  │  + phy_rpc_overlay/       │
       │                          │  50 MHz │     - phy_rpc_handlers.c  │
       │      ┌────────────┐      │         │  + libphy.a (blob)        │
       │      │ this repo's│      │         │                           │
       │      │  host/     │      │         │      ┌────────────┐       │
       │      └────────────┘      │         │      │ this repo's│       │
       │                          │         │      │  slave/    │       │
       └──────────────────────────┘         │      └────────────┘       │
                                             └──────────────────────────┘
                                                        │
                                                        ▼
                                            5 GHz radio: 4.9–5.95 GHz
```

The vendored upstream esp-hosted (under `vendor/esp-hosted-mcu/`) is
kept verbatim so rebases against future upstream releases stay clean.
All of our patches live in the sibling `slave/components/phy_rpc_overlay/`
directory — *no upstream files are modified*. Same on the host side:
our additional API is in a separate `host/` component layered on top
of upstream's `esp_hosted` component via a normal `REQUIRES`.

## Quick start

### 1. Flash the slave (no IDF install needed)

```sh
git clone https://github.com/DatanoiseTV/esp-hosted-open
cd esp-hosted-open
firmware/flash.sh /dev/cu.usbserial-XXXX
```

The merged binary in `firmware/c5_slave_merged.bin` is rebuilt by CI
on every push and attached to release tags. It writes to flash offset
`0x0` on a 4 MB esp32c5, dio mode at 80 MHz.

### 2. Wire it to your host

| Signal | Default P4 GPIO | Notes |
|--------|-----------------|-------|
| CLK    | 18 | |
| CMD    | 19 | |
| D0–D3  | 14, 15, 16, 17 | |
| RESET  | 54 | drives the C5's `EN` line |

Override these in your host project's `sdkconfig.defaults` if your
hardware differs.

### 3. Pull the host component

```yaml
# main/idf_component.yml
dependencies:
  espressif/esp_wifi_remote: "^1.5"
  espressif/esp_hosted:
    override_path: "../../components/vendor/esp-hosted-mcu"
```

Vendor `esp-hosted-mcu/` and `host/` from this repo into your project
under `components/vendor/` and `components/host/` respectively, or
add them as a submodule. Then `REQUIRES esp_hosted_open` in your
`main/CMakeLists.txt` and call the API.

A worked example lives in `examples/p4_set_channel/` (under 200 lines).

## What works, what's flaky, what's missing

✅ **Verified building** against ESP-IDF v6.0 via CI on every push.
Slave fits in the C5's 1.875 MB factory partition with 27 % free.
Host stub adds ~150 B to a P4 binary.

✅ **Verified on hardware** (bench, conducted via 30 dB attenuator
into a SDR): channel-set to 180 (5.900 GHz), `phy_11p_set(1, 0)`,
AGC max gain at 255, low-rate filter off. Real 802.11p stations'
CAM frames decode correctly through the host.

⚠️ **Untested in the field.** No claims about real-world distance,
multipath behaviour, or spectrum-mask conformance. The 5.9 GHz
waveform the C5 emits with these settings is *not* a true 10 MHz
802.11p signal — it's a 20 MHz Wi-Fi-shaped frame on an ITS channel
that some 11p front-ends will demodulate.

⚠️ **No safety hooks.** A single host-side typo can put the radio
into states the FCC / ETSI test masks would fail. Lock down RF
emission at the test-bench level, not in software.

❌ **Not yet wrapped:** CSI capture (`esp_wifi_set_csi` +
`phy_csidump_force_lltf_cfg`), raw 802.11 TX from host
(`esp_wifi_80211_tx` over SDIO needs a passthrough), BT-radio
shockburst (`phy_bt_*` family), 802.11mc FTM ranging. PRs welcome —
the extension recipe in [docs/extending.md](docs/extending.md) is
deliberately mechanical.

## Layout

```
vendor/esp-hosted-mcu/          upstream snapshot, untouched
slave/                          slave firmware project (esp32c5)
  components/phy_rpc_overlay/   the actual fork patch
host/                           host-side stub library (esp_hosted_open)
firmware/                       prebuilt c5_slave_merged.bin + flash.sh
tests/host/                     gcc-only tests for the wire protocol
docs/                           extension recipe + symbol reference
.github/workflows/build.yml     CI: host tests + slave + host examples
```

## Use cases this enables (and one this repo hosts)

- **5.9 GHz / ITS-G5 / 802.11p** — protocol stack lives in
  [esp32-c5-v2x](https://github.com/DatanoiseTV/esp32-c5-v2x) (CAM/DENM
  encoders, GeoNetworking, BTP, asn1c-based codec). That repo depends on
  this one for everything below the LLC layer.
- **Channel-busy ratio (CBR) measurement** for spectrum studies
- **Manual RX-gain sweeps** for sensitivity characterisation
- **Disabling CCA** for back-to-back loopback or PHY conformance work
- **Promiscuous capture of arbitrary EtherTypes** with the slave
  forwarding raw frames to the host over the event channel
- **Anything else** the public esp-hosted API doesn't reach. The
  symbol reference in `docs/symbol-reference.md` lists what we've
  enumerated in libphy.a; only a fraction is wrapped today.

## Contributing

This is research code. Issues, PRs, "I scoped this and here's what it
looks like" reports are all welcome. Particularly useful contributions:

- A new RPC for an unwrapped symbol — follow `docs/extending.md`.
- A test fixture that decodes a real CAM/DENM/etc. captured from a
  certified station, so we can regress against ground truth.
- A documented success / failure on different C5 module batches —
  the on-die radio tweaks vary across silicon revisions.

## Acknowledgements

- Espressif for the underlying esp-hosted-mcu transport. This fork
  keeps their code untouched in `vendor/`.
- [opentrafficmap/its-g5-receiver-firmware](https://codeberg.org/opentrafficmap/its-g5-receiver-firmware)
  for documenting the original `phy_11p_set(1, 0)` +
  `phy_change_channel(ch, 1, 0, 0)` bootstrap sequence on the C5.

## License

Apache-2.0, matching upstream esp-hosted-mcu. See [LICENSE](LICENSE).
