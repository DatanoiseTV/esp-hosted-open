# Espressif PHY symbol reference (universal)

Every Espressif Wi-Fi-capable chip ships a binary blob — typically
`libphy.a` plus `libpp.a` and `libnet80211.a` — that contains a much
larger surface than the public `esp_wifi_*` API exposes. This page
maps the **undocumented but exported** symbols across the whole
family, identifies which features each chip supports, and points at
what's wrappable from a host MCU via this project's RPC channel.

The data is regenerated mechanically from IDF v6.0 — see [Methodology](#methodology) at the bottom.

> **Why this matters.** If you see a feature ticked for your chip
> below, the libphy.a it ships with already contains the code path.
> You don't need a kernel patch or a custom IDF — just the
> matching `phy_*` extern declaration and a call site. The
> esp-hosted-open slave does this work for you: `phy_rpc_handlers.c`
> calls these symbols, and our host stub library lets a different
> MCU drive them over SDIO.
>
> **What it doesn't mean.** Espressif gives no contract on these
> symbols — names, signatures, and behaviour can shift between IDF
> releases. Treat each new IDF version as a re-validation step.

---

## Cross-chip feature matrix

Symbols counted only when present in `libphy.a` for that target on
IDF v6.0. A `✓` means the symbol is exported; calling it still
needs a correct prototype (some args are unknown without RE) and
the right context (Wi-Fi must be running, etc.).

| Feature                                       | ESP32 | S2 | S3 | C2 | C3 | C5 | C6 | C61 | H2 | H4 |
|-----------------------------------------------|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| Force channel (regulatory bypass)             | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓  | ✓ | ✓ |
| Direct frequency tuning (Hz)                  |   | ✓ | ✓ |   | ✓ | ✓ | ✓ | ✓  | ✓ | ✓ |
| Band switch (2.4 / 5 GHz)                     |   |   |   |   |   | ✓ |   |    |   |   |
| 802.11p PHY mode (`phy_11p_set`)              |   | ✓ | ✓ |   | ✓ | ✓ | ✓ | ✓  |   |   |
| Force fixed TX rate                           |   | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓  |   |   |
| Low-rate filter override                      | ✓ |   | ✓ |   | ✓ | ✓ |   | ✓  | ✓ | ✓ |
| 20 / 40 MHz bandwidth                         |   |   |   |   |   | ✓ |   | ✓  |   |   |
| Manual RX gain (`phy_force_rx_gain`)          |   |   | ✓ | ✓ | ✓ | ✓ | ✓ | ✓  |   |   |
| AGC max-gain ceiling                          |   |   |   |   |   | ✓ |   |    |   |   |
| RX gain table swap                            |   |   |   |   |   | ✓ |   | ✓  |   |   |
| AGC enable / disable                          |   |   |   |   |   | ✓ |   | ✓  |   |   |
| CCA enable / disable                          |   |   |   |   |   | ✓ |   | ✓  | ✓ | ✓ |
| Channel-busy counters (CBR)                   | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓  |   |   |
| Instantaneous PHY RSSI                        | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓  |   |   |
| Noise-floor read                              | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓  |   |   |
| TX power direct set (`phy_set_most_tpw`)      | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓  |   |   |
| Forced LLTF CSI dump                          |   |   |   |   |   | ✓ |   | ✓  |   |   |
| Internal loopback (self-test)                 |   |   |   |   |   | ✓ |   | ✓  |   |   |
| Channel filter override                       | ✓ |   | ✓ |   | ✓ | ✓ | ✓ | ✓  | ✓ | ✓ |
| BT-radio TX gain (nRF24-style hacks)          |   |   |   |   |   | ✓ |   | ✓  |   |   |
| BT-radio filter override                      |   |   |   |   |   | ✓ |   | ✓  |   |   |
| 802.11mc FTM compensation                     |   | ✓ | ✓ |   |   | ✓ | ✓ | ✓  |   |   |
| Japan ch14 micro-config                       |   |   |   |   |   | ✓ |   | ✓  |   |   |
| Temperature sensor read (`phy_xpd_tsens`)     |   | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓  | ✓ |   |

H2 and H4 are 802.15.4 / Thread parts with very limited Wi-Fi PHY,
which is why most rows are blank.

---

## Per-feature notes

These are short summaries of what each row in the matrix does, what
arguments the call expects (where known), and what to watch for.
"Inferred" means we know the symbol exists but haven't confirmed
the argument list against scope evidence.

### Force channel (`phy_change_channel`)

```c
extern void phy_change_channel(int channel, int /*?*/, int /*?*/, int ht_mode);
```

Bypasses the IDF regulatory channel allow-list. Lets you tune to
channels the country code disallows (5 GHz UNII-2C / 3, ITS 172–184,
Japanese ch14, etc.). The first arg is the IEEE channel; the last
is HT mode (0 = HT20). Bootstrap with a regular `esp_wifi_set_channel`
to a *nearby* allowed channel first so RF cal runs.

### Direct frequency tuning (`phy_set_freq`)

Sub-channel-grid tuning, useful if you need an off-grid centre
frequency for testing. Args are reverse-engineered per chip; on the
C5 it's a 4-byte integer in MHz units.

### Band switch (`phy_band_change`, `phy_band_sel`)

C5-only. Toggles the radio between 2.4 and 5 GHz. Implicitly invoked
by `phy_change_channel` if the target channel is on the other band,
so most callers don't need to use it directly.

### 802.11p PHY mode (`phy_11p_set`)

```c
extern void phy_11p_set(int enable, int reserved /* 0 */);
```

Six chips have this symbol. The PHY-layer effect is partially
reverse-engineered from community work (originally on ESP32):

- relaxes regulatory band-edge checks
- adjusts OFDM front-end timing toward 802.11p-style behaviour

Only the C5 also has a 5 GHz radio, so it's the only chip where
"802.11p-on-Espressif" can mean genuine 5.9 GHz V2X. On 2.4 GHz
chips (S2/S3/C3/C6/C61) the same call still produces an
802.11p-shaped waveform on the 2.4 GHz band — useful for
loopback / protocol-stack development.

### Manual RX gain (`phy_force_rx_gain`, `phy_rx_gain_force`)

```c
extern void phy_force_rx_gain(int gain_index);   /* 0..max          */
extern void phy_rx_gain_force(int enable);       /* 1 = lock        */
extern void phy_disable_agc(void);
extern void phy_enable_agc(void);
```

Disables AGC and pins the receiver at a fixed gain. Useful for
sensitivity testing, fixed-gain monitor mode, and capturing weak
signals AGC would otherwise track away from. Re-arm AGC by calling
`phy_enable_agc()` and `phy_rx_gain_force(0)`.

### AGC max-gain ceiling (`phy_agc_max_gain_set`)

C5-only. Caps how much amplification the AGC can apply. Less
disruptive than disabling AGC; useful for saying "track signal level,
but never go above this".

### CCA enable / disable (`phy_disable_cca`, `phy_enable_cca`)

Turns Clear-Channel-Assessment off. The radio will then transmit
without listening first — you've **just disabled the only
spectrum-courtesy mechanism in 802.11**. Use only on a wire / in a
chamber. Pair with `phy_get_cca_cnt` for proper CBR measurement
when re-enabled.

### CBR counters (`phy_get_cca_cnt`, `phy_set_cca_cnt`)

```c
extern uint32_t phy_get_cca_cnt(void);
extern void     phy_set_cca_cnt(uint32_t value);
```

Universal across all Wi-Fi chips. Returns a monotonic "busy ticks"
counter; combine with elapsed wall time for a CBR percentage. Reset
with `phy_set_cca_cnt(0)`.

### Instantaneous PHY RSSI (`phy_get_rssi`, `phy_get_sigrssi`)

PHY-side RSSI poll, distinct from per-frame RSSI in the promiscuous
callback. `phy_get_sigrssi` returns the most-recent signal frame's
RSSI; `phy_get_rssi` returns a wider-band reading that also picks
up noise.

### Noise-floor read (`phy_get_noise_floor`)

Returns the current noise-floor estimate in dBm. Combined with PHY
RSSI gives a real-time SNR.

### TX power direct set (`phy_set_most_tpw`, `phy_get_most_tpw`)

Bypasses the regulatory power table. Lower-level than
`esp_wifi_set_max_tx_power`, which goes through the country
database. "Tpw" appears to be quarter-dBm.

### Forced LLTF CSI dump (`phy_csidump_force_lltf_cfg`)

C5/C61 only. Forces CSI capture on every frame's L-LTF (legacy
training field). Combined with the public `esp_wifi_set_csi_rx_cb`
this gives unconditional CSI — useful for sensing applications that
can't rely on the default conditional capture.

### Internal loopback (`phy_set_loopback_gain`)

C5/C61. Routes the TX output back into the RX chain at a configurable
gain for self-test. No spectrum emission.

### BT-radio TX gain (`phy_bt_set_tx_gain_new`, `phy_bt_tx_gain_set`)

C5/C61. The BT 2.4 GHz radio's TX gain is exposed separately from
the Wi-Fi radio's. With the right preamble and CRC handling, this is
the path that lets you emit nRF24L01+-style Shockburst frames from
an ESP32 — see also `phy_bt_filter_reg`.

### 802.11mc FTM compensation (`phy_ftm_comp`)

S2/S3/C5/C6/C61. PHY-side timestamp correction for
Fine-Time-Measurement ranging. The high-level FTM API
(`esp_wifi_ftm_initiate_session`, etc.) is public and lives in
`libnet80211.a`; this PHY symbol is what improves accuracy.

---

## What esp-hosted-open wraps today

The slave's `phy_rpc_handlers.c` ships handlers for these features
on the C5 by default. Adding more chips is mostly Kconfig + matching
the right symbol prototype — the dispatcher is target-agnostic.

| RPC                                   | Underlying symbol                |
|---------------------------------------|----------------------------------|
| `set_channel`                         | `phy_change_channel`             |
| `set_phy_11p`                         | `phy_11p_set`                    |
| `set_country_permissive`              | `esp_wifi_set_country` (public)  |
| `set_tx_power`                        | `esp_wifi_set_max_tx_power` (public) |
| `set_rx_gain`                         | `phy_force_rx_gain` + AGC off    |
| `set_agc_max_gain`                    | `phy_agc_max_gain_set`           |
| `set_cca`                             | `phy_disable_cca` / `phy_enable_cca` |
| `get_cca_counters` / `reset_cca_counters` | `phy_get_cca_cnt` / `phy_set_cca_cnt` |
| `set_low_rate`                        | `phy_disable_low_rate`           |
| `set_bandwidth`                       | `phy_bb_bss_cbw40`               |
| `get_phy_rssi`                        | `phy_get_sigrssi` / `phy_get_rssi` |

## Wrappable but not yet wrapped (PRs welcome)

These are present in the C5 blob and look broadly useful — adding
them to the RPC layer is one packed struct + one host stub + one
slave handler each.

- `phy_get_noise_floor` — instantaneous noise-floor poll
- `phy_set_freq` — direct frequency tuning (off-grid testing)
- `phy_set_loopback_gain` — internal RX/TX loopback for self-test
- `phy_set_rate` — force fixed TX rate / MCS
- `phy_csidump_force_lltf_cfg` — forced CSI dump
- `phy_bt_set_tx_gain_new` + `phy_bt_filter_reg` — nRF24-style hacks
  on the BT radio
- `phy_xpd_tsens` — die temperature read (handy for long captures)

See [extending.md](extending.md) for the recipe.

## Per-chip raw symbol lists

Pre-extracted dumps of every `T phy_*` symbol per chip live under
[symbols/](symbols/). They're regenerated on every release of
this repo. Useful when investigating something chip-specific — grep
for a name pattern in the file matching your target.

```sh
ls docs/symbols/
# → esp32.phy.txt esp32c5.phy.txt esp32c61.phy.txt ...
```

Symbol counts (ballpark — a measure of how much of the radio
Espressif keeps internal):

| chip        | `phy_*` exports |
|-------------|-----------------|
| esp32       |  63 |
| esp32s2     |  52 |
| esp32s3     |  74 |
| esp32c2     |  47 |
| esp32c3     |  75 |
| **esp32c5** | **465** |
| esp32c6     |  81 |
| **esp32c61**| **390** |
| esp32h2     |  52 |
| esp32h4     |  67 |

C5 and C61 (newest Wi-Fi 6 silicon) carry an order of magnitude
more PHY surface than the older parts — that's where most of the
interesting opportunities are.

## MAC-layer surface

`libpp.a` (low-level packet processor) exports ~30 `pp_/wifi_`
functions per chip, fairly stable across the family.
`libnet80211.a` is much larger (488–641 exports) and contains the
802.11 MAC machinery: scan management, TX/RX paths, AMPDU,
mesh, FTM, monitor mode. Some of these are reachable from a host
via existing public IDF APIs; others (raw 80211 TX without the
CSA / sequence-number machinery, monitor-mode filter masks the
public API doesn't surface) need wrapping.

We have not enumerated `libnet80211.a` here yet because the symbol
namespace is messier — `ieee80211_*`, `ftm_*`, `wifi_*`,
`pp_post_to_wifi_task_*`, ad hoc helpers — and most callers shouldn't
need to reach below `esp_wifi_*` for MAC concerns. If you have a
specific ask, open an issue.

---

## Methodology

Anyone can reproduce this map. From an IDF v6.0 install:

```sh
# Per-chip phy_* symbol dump:
for chip in esp32 esp32s2 esp32s3 esp32c2 esp32c3 esp32c5 esp32c6 esp32c61 esp32h2 esp32h4; do
  nm -gU $IDF_PATH/components/esp_phy/lib/$chip/libphy.a 2>/dev/null \
    | awk '/T phy_/ {print $3}' | sort -u > $chip.phy.txt
done

# Symbols present on every chip (universal):
sort *.phy.txt | uniq -c | awk '$1==10 {print $2}'

# Symbols unique to C5:
comm -23 esp32c5.phy.txt <(cat esp32{,s2,s3,c2,c3,c6,c61,h2,h4}.phy.txt | sort -u)
```

`nm -gU` shows externally-visible (`-g`) defined-only (`-U`) symbols.
Filtering on `T phy_` keeps text-section exports prefixed with `phy_`;
adjust the regex for `pp_` / `ieee80211_` / `ftm_` if you're surveying
MAC-layer symbols.

For symbols whose argument list is unknown, scope the call against
known-good libpcap / SDR captures before relying on it.
