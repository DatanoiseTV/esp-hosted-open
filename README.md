# esp-hosted-open

**A patched fork of [esp-hosted-mcu] that lets a host MCU drive
*every* radio of an Espressif co-processor — including the parts
Espressif's stock firmware deliberately hides.**

[esp-hosted-mcu]: https://github.com/espressif/esp-hosted-mcu

Stock esp-hosted gives you `esp_wifi_*` over SDIO. That's wonderful
for making a P4 act like a Wi-Fi STA, and useless if what you actually
want is a 5.9 GHz V2X radio, a monitor-mode sensor, an ESP-NOW peer,
a Thread/Zigbee co-processor, an FTM ranging master, a CSI sensor, or
anything else that lives below the regulated `esp_wifi_*` surface.

This fork keeps upstream esp-hosted-mcu *untouched* in `vendor/` and
adds:

- one slave-side overlay component (`phy_rpc_overlay/`) that calls
  into `libphy.a` directly,
- one host-side stub library (`esp_hosted_open`) that ships **46
  request RPCs and 8 async event channels** over esp-hosted's
  generic peer-data transport.

---

## ⚠️ Experimental — research / lab use only

Read this carefully.

| | |
|---|---|
| **Undocumented internals.** | We call symbols (`phy_11p_set`, `phy_change_channel`, `phy_force_rx_gain`, `phy_disable_cca`, `phy_bb_bss_cbw40`, …) that aren't in any public header. Espressif may rename, remove, or change them between IDF releases without warning. We pin against IDF v6.0 and treat each upgrade as something that needs re-validation. |
| **Regulatory risk is on you.** | This firmware will happily tune to 5.9 GHz ITS channels, disable carrier sense, override the regulatory country database, and force RX gain past safe operating limits. Doing any of that on the air without a license is illegal almost everywhere. Use a shielded enclosure, a fully-cabled setup with attenuators, or a regulator-issued research permit. |
| **Not safety-certified.** | Don't put this in a vehicle, drone, ISM-band product, medical device, or anything else that hurts people or property when it misbehaves. We provide no warranty, express or implied — Apache-2.0, "AS IS". |
| **Don't blame Espressif.** | The bugs you find here are *ours*, not theirs. Don't open issues against esp-hosted-mcu or the IDF for behaviour we introduced. |
| **Spectrum manners.** | If you actually emit on the air, do it deliberately, briefly, and on bands you're entitled to. Stomping on real V2X traffic is dangerous; stomping on real Wi-Fi or Zigbee is rude. |

By using this code you accept that you alone are responsible for any
RF emissions and for compliance with your local regulations.

---

## All wireless tech the silicon can do

This repo aims to expose **everything** the chip can transmit and
receive, not just regular Wi-Fi:

| Stack            | Coverage                                                                          | Where         |
|------------------|-----------------------------------------------------------------------------------|---------------|
| 802.11 a/g/n/ax  | channel/freq/band/bandwidth/rate, raw TX, promiscuous RX with filter, MAC override, protocol selection | this repo |
| 802.11p OCB      | best-effort via `phy_11p_set` + `phy_change_channel(172..184)`                    | this repo     |
| 802.11mc FTM     | initiator API + per-burst report event (rtt_raw, rtt_est, dist_est)               | this repo     |
| CSI sensing      | `phy_csidump_force_lltf_cfg` + per-frame CSI capture event                        | this repo     |
| ESP-NOW          | full lifecycle: init/peers/send + RX/TX-status events                             | this repo     |
| 802.15.4 / Thread / Zigbee | enable, channel (11–26), pan ID, promisc, raw TX + RX events. Works on C6/H2/H4; returns `NOT_SUPPORTED` on C5/S2/S3/C2/C3 | this repo |
| Bluetooth LE / HCI | full HCI bridge + Bluedroid stack                                               | upstream esp-hosted (`esp_hosted_bt.h`, `esp_hosted_bluedroid.h`) |
| BT-radio low-level | `phy_bt_set_tx_gain_new`, `phy_bt_filter_reg` (for nRF24L01+-style hacks)        | this repo (partial — full shockburst sequencer pending) |

Runtime **capability discovery** lets the same host code work
unchanged across chip choices: `esp_hosted_open_get_caps(caps)`
returns a 4-byte bitmap, and `esp_hosted_open_has_capability(REQ_ID, caps)`
tells you whether the slave's firmware actually exposes that RPC. Swap
a C5 slave for a C6 and Thread support lights up automatically while
the 5 GHz / 802.11p calls start returning `NOT_SUPPORTED`.

For the universal map of **which `libphy.a` symbol is exported on
which chip**, see [docs/symbol-reference.md](docs/symbol-reference.md).

---

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

### 3. Pull host + vendored upstream into your project

```yaml
# main/idf_component.yml
dependencies:
  espressif/esp_wifi_remote: "^1.5"
  espressif/esp_hosted:
    override_path: "../../components/vendor/esp-hosted-mcu"
```

Copy `vendor/esp-hosted-mcu/` and `host/` from this repo into your
project under `components/vendor/` and `components/host/`. Then
`REQUIRES esp_hosted_open` in `main/CMakeLists.txt` and:

```c
#include "esp_hosted.h"          /* upstream — STA/AP/HCI/BLE       */
#include "esp_hosted_open.h"     /* this fork — extra PHY + radios  */

esp_hosted_init();               /* upstream link bring-up          */
esp_hosted_open_init();          /* register our extra RPC handlers */

/* Channel + 802.11p PHY toggle (C5 ITS-G5 example) */
esp_hosted_open_set_country_permissive();
esp_hosted_open_set_phy_11p(true);
esp_hosted_open_set_channel(180);
esp_hosted_open_set_agc_max_gain(255);
esp_hosted_open_set_low_rate(false);

/* Or send an ESP-NOW frame to a peer (works on every Wi-Fi chip) */
esp_hosted_open_espnow_init();
esp_hosted_open_espnow_add_peer(peer_mac, NULL, 0, 0, false);
esp_hosted_open_espnow_send(peer_mac, "hello", 5);

/* Or initiate FTM ranging */
esp_hosted_open_register_ftm_cb(on_ftm_report, NULL);
esp_hosted_open_ftm_initiate(ap_mac, /*frames*/16, /*period*/2, /*ch*/6);

/* Or capture every 802.11 frame on the air */
esp_hosted_open_register_raw_rx_cb(on_frame, NULL);
esp_hosted_open_set_promisc_filter(WIFI_PROMIS_FILTER_MASK_ALL);
esp_hosted_open_set_promisc(true);
```

The full API surface is documented in
[`host/include/esp_hosted_open.h`](host/include/esp_hosted_open.h).

---

## Architecture

```
   ┌──────────────────────────┐  SDIO 4-bit  ┌────────────────────────────┐
   │   Host MCU (P4, S3, …)   │   @ 50 MHz   │  Espressif co-processor    │
   │                          │              │  (C5: Wi-Fi 6 + BLE)       │
   │  your application code   │ ◄──────────► │  (C6: + 802.15.4)          │
   │  esp_hosted_open_*()     │              │  (H2/H4: 15.4 + BLE only)  │
   │                          │              │                            │
   │  ┌──────────────────┐    │              │  upstream esp-hosted-mcu   │
   │  │  this repo's     │    │              │  + slave/components/       │
   │  │  host/           │    │              │      phy_rpc_overlay/      │
   │  │  esp_hosted_open │    │              │    ├ phy_rpc_handlers.c    │
   │  └──────────────────┘    │              │    ├ phy_rpc_extras.c      │
   │  + upstream esp_hosted   │              │    └ phy_rpc_wireless.c    │
   └──────────────────────────┘              │  + libphy.a (binary blob)  │
                                              └────────────────────────────┘
                                                          │
                               ┌──────────────────────────┼──────────────────────────┐
                               ▼                          ▼                          ▼
                        2.4 GHz Wi-Fi          5 GHz Wi-Fi (C5/C61)        2.4 GHz BLE / 802.15.4
                        ESP-NOW                ITS / V2X channels          (per-chip support)
```

The upstream esp-hosted snapshot (under `vendor/esp-hosted-mcu/`) is
kept verbatim so rebases against future upstream releases stay
trivial. **No upstream files are modified** — every patch lives in
the sibling overlay directory. Same on the host side: our additions
are a separate `host/` component layered on top of upstream's
`esp_hosted` via a normal `REQUIRES`.

The slave-side overlay is split across three files for review-ability:

- `phy_rpc_handlers.c` — matrix-row PHY hacks (channel, gain, CCA, …)
- `phy_rpc_extras.c` — raw 802.11 TX/RX, promiscuous, CSI, FTM, MAC, protocol
- `phy_rpc_wireless.c` — ESP-NOW + 802.15.4

---

## What's covered today (and what isn't)

✅ **Build-verified** against ESP-IDF v6.0 via CI on every push:
slave for esp32c5, P4 host example, host-runnable proto tests. Slave
fits in the C5's 1.875 MB factory partition with 26 % free; host
example is ~580 kB on the P4 (43 % free in a 1 MB partition).

❌ **No hardware testing has been performed.** Nothing in this repo
has been put on a scope, an SDR, or against a real ITS / Thread /
nRF24 / FTM peer. The RPCs compile and the wire protocol round-trips
in CI; what each one *actually does at the radio level* is inferred
from `libphy.a` symbol names + community work, not measured. Don't
trust on-air behaviour until you've verified it yourself.

⚠️ **Particularly suspect** until proven on hardware:

- Whether `phy_11p_set(1, 0)` produces a waveform that real 802.11p
  front-ends demodulate. The C5 PHY is a 20 MHz OFDM design; ETSI
  802.11p uses 10 MHz channels. At best this is a 20-MHz Wi-Fi-shaped
  frame on a 5.9 GHz channel that *some* 11p radios will accept.
- The BT-radio TX-gain knob actually emitting on the band you expect.
- The 802.15.4 path on a C6/H2/H4 (we've never built for those targets).
- Whether the slave's queued event channel keeps up under load.

⚠️ **No safety hooks.** A single host-side typo can put the radio
into states the FCC / ETSI test masks would fail. Lock down RF
emission at the test-bench level, not in software.

❌ **Still missing** (PRs welcome — see
[docs/extending.md](docs/extending.md) for the recipe):

- A complete BT-radio shockburst sequencer (preamble + CRC + address
  registers) for nRF24L01+ emulation. The TX-gain knob is wrapped
  but the rest of the BT-PHY surface needs scope-confirmed RE.
- Mesh / SmartConfig sniff hooks.
- `esp_wifi_set_event_mask` (suppress noisy `WIFI_EVENT_*` on the
  slave so the host only gets what it cares about).

---

## Layout

```
vendor/esp-hosted-mcu/             upstream snapshot, untouched (Apache-2.0)
slave/                             slave firmware project (esp32c5 default)
  components/phy_rpc_overlay/      our fork patch — three files, see above
host/                              host-side stub library (esp_hosted_open)
  include/                         esp_hosted_open.h  +  phy_rpc_proto.h
firmware/                          prebuilt c5_slave_merged.bin + flash.sh
tests/host/                        gcc-only proto sanity checks
docs/                              extension recipe + universal symbol map
.github/workflows/build.yml        CI: host tests + slave + merged image
```

Every wireless RPC's underlying libphy.a / IDF symbol is enumerated
across all 10 Espressif chips in
[docs/symbol-reference.md](docs/symbol-reference.md).

---

## Use cases this enables

- **5.9 GHz / ITS-G5 / 802.11p** — protocol stack lives in
  [esp32-c5-v2x](https://github.com/DatanoiseTV/esp32-c5-v2x)
  (CAM/DENM encoders, GeoNetworking, BTP, asn1c-based codec). That
  repo depends on this one for everything below the LLC layer.
- **Monitor-mode capture** of arbitrary 802.11 traffic with
  per-frame metadata (RSSI, channel, rate, QoS flag, timestamp)
  forwarded to the host as `RAW_FRAME` events.
- **CSI-based sensing** (presence, gesture, breathing) with per-frame
  CSI buffers piped over SDIO.
- **802.11mc FTM ranging** — single-call initiator with
  millisecond-resolution distance estimates.
- **ESP-NOW peer networks** at low latency, no IP overhead, no AP
  needed — universal across every Wi-Fi-capable Espressif chip.
- **Thread / Zigbee** when the slave is a C6 / H2 / H4 — same host
  binary, capability bitmap takes care of the rest.
- **Channel-busy ratio (CBR)** measurement for spectrum studies.
- **Manual RX-gain sweeps** for sensitivity characterisation.
- **Disabling CCA** for back-to-back loopback or PHY conformance work.
- **Anything else the public esp-hosted API doesn't reach.** The
  symbol reference enumerates 1366 `phy_*` exports across the chip
  family — only a fraction is wrapped today.

---

## Contributing

This is research code. Issues, PRs, and "I scoped this and here's
what it looks like" reports are all welcome. Particularly useful:

- A new RPC for an unwrapped libphy symbol — follow
  [docs/extending.md](docs/extending.md). The recipe is mechanical:
  one packed struct + one host stub + one slave handler + one test
  assertion.
- Spectrum / scope evidence for what an undocumented symbol actually
  does on a particular chip.
- A documented success / failure on different module batches —
  on-die radio tweaks vary across silicon revisions.

---

## Acknowledgements

- Espressif for the underlying esp-hosted-mcu transport. This fork
  keeps their code untouched in `vendor/`.
- [opentrafficmap/its-g5-receiver-firmware](https://codeberg.org/opentrafficmap/its-g5-receiver-firmware)
  for documenting the original `phy_11p_set(1, 0)` +
  `phy_change_channel(ch, 1, 0, 0)` bootstrap sequence on the C5.

---

## License

Apache-2.0, matching upstream esp-hosted-mcu. See [LICENSE](LICENSE).
