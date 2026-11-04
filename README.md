![logo](docs/images/ultra-logo.png)

**English** | [简体中文](README.zh-CN.md)

# ChameleonUltra firmware — Next fork

This is a fork of [RfidResearchGroup/ChameleonUltra](https://github.com/RfidResearchGroup/ChameleonUltra)
that adds **16 emulation slots** and **reader-driven slot polling** to the
firmware. It is the companion firmware of the
[Chameleon Ultra Next](https://github.com/yueyangw/ChameleonUltraNext) client,
and it stays protocol-compatible with the official clients: every upstream
command still behaves the same, the new features live behind new command IDs.

Download the firmware from [Releases](https://github.com/yueyangw/ChameleonUltra/releases).

![ultra picture](docs/images/ultra-overview.png)

## What is different from upstream

### 16 emulation slots

- `TAG_MAX_SLOT_NUM` is 16. The slot config record moves to version 9 and is
  migrated from v8 on first boot, existing slots 1–8 are kept as they are.
- The legacy `GET_SLOT_INFO` / `GET_ENABLED_SLOTS` replies stay at 8 slots and
  byte-identical, so older clients keep working. 16-slot clients use the new
  `*_V2` commands.
- The slot index is validated on load, a corrupted value can no longer index
  out of bounds. LED animations map slots 9–16 onto the 8 physical LEDs.

### Reader-driven slot polling

With polling enabled the device rotates through the enabled slots while it is
in a reader field. It does not switch on a fixed timer: it watches what the
reader does with the presented card (REQA/WUPA polls, select, HALT, MIFARE
Classic auth result, field resets) and leaves a slot only when the reader is
clearly done with it or has no use for it.

| Reader behaviour | What the engine does |
|---|---|
| Reads the UID and HALTs the card (access control, UID readers) | Lets the card be read `1 + represent` times, then moves on after `done quiet` ms of silence |
| MIFARE Classic auth succeeds | Keeps the slot while the reader keeps talking, releases it after `lock quiet` of silence |
| MIFARE Classic auth fails | Leaves after `auth fail count` failures, or earlier if the reader only polls for other cards; the slot is skipped once in the next round |
| Auth fails and the reader goes silent (a phone handing the tag to an app) | Keeps the slot for `patience` ms |
| Polls but never selects the slot | Moves on after about 3 poll periods (the period is measured from REQA/WUPA timing) |
| Field reset in the middle of a transaction | Presents the next slot when the field comes back |
| Phone that pauses its field every ~2 s | Keeps the slot it worked on for `sticky sessions` discovery sessions |
| Field without any polling (e.g. LF) | Rotates every `active dwell` ms |

Other details:

- Polling only starts on a power cycle that was woken by an RF field. Button,
  USB or BLE wake-ups do not start it, and a manual slot switch stops it for
  the rest of that cycle.
- Slot switches during polling keep the NFC sensing running and skip the
  140 ms LED fade, a switch takes well under 1 ms. The NFC peripheral is reset
  on every field loss while polling, which prevents it from answering garbage
  after many rapid swaps.
- A no-field timeout lets the device go back to sleep even if a slot switch
  disabled sensing without a field-lost event.
- The config is persisted in flash (24-byte record, version 7). Records written
  by earlier builds of this fork (v2–v6) are migrated, unknown fields keep
  their current value.

### Polling config (`SET_POLLING_CONFIG`, 24 bytes, little-endian)

| Offset | Field | Default | Meaning |
|---|---|---|---|
| 0 | `version` | 7 | record version |
| 1 | `enabled` | 0 | polling on/off |
| 2 | `slot_mask` | 0 | bit n = slot n, 0 = all enabled slots |
| 4 | `idle_dwell_ms` | 500 | rotation while no field is present |
| 6 | `active_dwell_ms` | 2000 | rotation while a field is present but nothing polls |
| 8 | `cooldown_ms` | 200 | pause after the field was lost |
| 10 | `adaptive` | 1 | jitter on the idle rotation |
| 11 | `jitter_percent` | 10 | jitter amount |
| 12 | `order` | 0 | 0 sequential, 1 random |
| 13 | `keep_awake` | 1 | keep the device awake while polling |
| 14 | `done_quiet_ms` | 100 | UID-only reader: silence before leaving |
| 16 | `patience_ms` | 800 | failed auth + silence: how long the slot is kept |
| 18 | `sticky_sessions` | 3 | phone re-discovery sessions the slot is kept for, 0 = off |
| 19 | `fail_poll_quiet_ms` | 150 | failed auth, reader polls for other cards: silence before leaving |
| 21 | `represent_count` | 1 | extra reads of a UID-only card per slot |
| 22 | `auth_fail_count` | 3 | failed auths that end a visit, 0 = off |
| 23 | `lock_quiet_100ms` | 10 | silence (×100 ms) before an authenticated slot is released |

### New commands

| ID | Command | Payload |
|---|---|---|
| 1900 | `GET_SLOT_INFO_V2` | HF/LF tag type (u16 BE each) of all 16 slots |
| 1901 | `GET_ENABLED_SLOTS_V2` | HF/LF enable flags of all 16 slots |
| 1902 | `GET_POLLING_CONFIG` | config record above |
| 1903 | `SET_POLLING_CONFIG` | config record above (older 16/24-byte layouts accepted) |
| 1904 | `GET_POLLING_STATUS` | 20 bytes: running, current slot, phase, hit count, last hit slot, skipped-slot mask, measured poll period |
| 1905 | `POLLING_LAB_SET` | development only: lab flags (any field, event log, hold idle) |
| 1906 | `POLLING_LAB_LOG` | development only: read the polling event ring buffer |

The lab commands and tooling (Proxmark3 as reader, trial matrix and analyzer)
are in [software/script/polling_lab](software/script/polling_lab/README.md).

### Other fixes

- Polling config never persisted on early builds (14-byte record vs. 16-byte
  FDS write), fixed with a word-sized record and a size assertion.
- LED out-of-bounds access for slots 9–16 in the sweep and symmetric animations.

## Flashing

The release contains `ultra-dfu-app.zip`, an application-only DFU package
signed with the standard ChameleonUltra DFU key, so the stock bootloader
accepts it and you can go back to the official firmware the same way.

- **Chameleon Ultra Next**: Tools → Firmware update, pick the zip, keep USB connected.
- **Other clients / nrfutil**: any client that flashes a DFU zip works, e.g.
  enter DFU with `hw dfu` in the CLI and flash with `nrfutil dfu usb-serial`.

Only the Ultra build is published. Lite can be built from source
(`CURRENT_DEVICE_TYPE=lite`) but has not been tested.

## Building

```bash
cd firmware/application
make -j8 GNU_INSTALL_ROOT=/path/to/arm-none-eabi/bin/
cd ../objects
nrfutil pkg generate --hw-version 0 --key-file ../../resource/dfu_key/chameleon.pem \
  --application application.hex --application-version 1 --sd-req 0x0100 ultra-dfu-app.zip
```

`firmware/build.sh` builds the full package (bootloader + softdevice) as upstream does.

---

# Upstream project

Documentation of the device itself: [ChameleonUltra wiki](https://github.com/RfidResearchGroup/ChameleonUltra/wiki)
· [docs repo](https://github.com/RfidResearchGroup/ChameleonUltraDocs).

## Compatible applications

* [Chameleon Ultra Next](https://github.com/yueyangw/ChameleonUltraNext) — 16 slots and polling configuration
* [ChameleonUltraGUI](https://github.com/GameTec-live/ChameleonUltraGUI)
* [MTools BLE](https://github.com/RfidResearchGroup/ChameleonUltra/wiki/mtoolsble)
* [Mifare Chameleon Tool (iOS only, Beta)](https://apps.apple.com/it/app/mifare-chameleon-tool/id6761231484)
* [Chameleon Ultra (Sailfish OS only)](https://sailfishos-chum.github.io/apps/harbour-chameleon-ultra)
* [Chameleon GO (iOS only)](https://apps.apple.com/us/app/id6802015306)
* [Device console for ChameleonUltra (Web)](https://sec1.dk/phreakbyte.html)

Clients other than Chameleon Ultra Next only see slots 1–8 and cannot configure polling.

## Authorized distributors

Lyon, France: [Lab401](https://lab401.com/)

Santa Ana, United States: [Hackerwarehouse](https://hackerwarehouse.com/)

Hastings, UK: [KSEC](https://labs.ksec.co.uk/product/proxgrind-chameleon-ultra/)

Montreal, Canada: [TechSecurityTools](https://techsecuritytools.com/product/chameleon-ultra/)

Shenzhen, China: [Sneaktechnology](https://sneaktechnology.com)

Guangdong, China: [MTools Tec](https://shop.mtoolstec.com/)

Lazada One, Singapore: [Aliexpress by RRG](https://proxgrind.aliexpress.com/store/1101312023)

## Videos

*Beware some of the instructions might have changed since recording, check the current documentation when in doubt!*

* [Downloading and compiling the official CLI](https://www.youtube.com/watch?v=VGpAeitNXH0)
* [Downloading ChameleonUltraGUI](https://www.youtube.com/watch?v=rHH7iqbX3nY)
* [ChameleonUltraGUI features overview](https://www.youtube.com/watch?v=YqE8wyVSse4)
* [Using ChameleonUltraGUI and the Chameleon Ultra](https://www.youtube.com/watch?v=9jtKNJ5-kVY)
* [MTools BLE - How to clone a card with ChameleonUltra](https://youtu.be/IvH-xtdW1Wk?si=4exqgAAeJ-kxU3aN)

## Community

* [RFID Hacking community discord server](https://t.ly/d4_C)
  * Software/chameleon-dev for firmware and clients development discussions
  * Devices/chameleon-ultra for usage discussions
* [GameTec_live discord server](https://discord.gg/DJ2A4wxncK)

Other notable projects:
* [Fantasi](https://fantasi.cloud) ([Discord](https://fantasi.cloud/discord))
* [chameleon-ultra.js](https://github.com/taichunmin/chameleon-ultra.js/)
