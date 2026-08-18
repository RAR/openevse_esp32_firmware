# OpenEVSE ESP32-S3 LCD board

Firmware notes for the ESP32-S3 WiFi/LCD board (`env:openevse_s3_lcd`).

Extracted from the schematics, not from §2 of the design doc — **§2 is stale**, it
still describes v1.1's `JP6` and `PILOTREAD`, neither of which exists.

**Two versions are live, and they differ.**

| | |
|---|---|
| **v1.2.0** | The boards that were **actually fabricated**. This is the firmware target. |
| **v1.3.0** | Designed, routed and verified, **never ordered**. Adds two GPIOs, four external pull-ups and fixes the backlight. |

Everything below is v1.2 unless marked **v1.3**. One firmware image is intended to
run on both.

## Module

**ESP32-S3-WROOM-1U-N16R8** — 16 MB quad SPI flash, 8 MB **octal** PSRAM, U.FL
external antenna (no PCB antenna).

- Octal PSRAM consumes **IO35, IO36, IO37** — unavailable.
- GPIO22–34 are not bonded out on WROOM-1 at all.
- Build config: `board_build.arduino.memory_type = qio_opi`, 16 MB flash,
  `-D BOARD_HAS_PSRAM`.

### What the 8 MB of PSRAM is actually doing

**It is not idle, and it is not something firmware assigns.** Arduino-ESP32 ships
prebuilt IDF libraries, so `qio_opi/include/sdkconfig.h` is fixed and not editable
from the app side. It sets:

```
CONFIG_SPIRAM_USE_MALLOC              1
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL   4096
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP  1
CONFIG_SPIRAM_MODE_OCT / SPEED_80M
```

PSRAM is therefore part of the ordinary heap, and **every allocation of 4 KB or more
goes there automatically** — TLS session buffers, Mongoose's connection and HTTP
buffers, log staging, and (via `TRY_ALLOCATE_WIFI_LWIP`) the WiFi and lwIP pools.
That is the memory headroom the `R8` part was bought for; it arrives by default
rather than by code.

Two consequences worth carrying:

- **Anything that must be internal has to say so.** The LVGL draw buffer already
  does — `heap_caps_malloc(..., MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` — because a
  plain 30 KB `malloc()` would now land in PSRAM. Same rule for any future DMA buffer.
- **`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` is 0** in this config, so no internal pool
  is held back for allocations that *must* be internal. Espressif ships it that way for
  every Arduino S3 PSRAM board, so it is not ours to change — but it means the draw
  buffer competes with everything else on a fragmented heap, and its failure branch is
  a real boot-time mode rather than a hypothetical. Internal-heap exhaustion stays the
  thing to watch and `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` stays the
  metric, exactly as on the ESP32 boards.

`CONFIG_SPIRAM_MODE_OCT` and `CONFIG_SPIRAM_SPEED 80` in the same file are consistent
with the N16R8's octal PSRAM at 80 MHz.

**The design doc's §1 justifies the `R8` part by the LVGL framebuffer not fitting
internally.** That is not how it plays out: the framebuffer stays internal
deliberately (the flush is CPU-bound, so PSRAM would only be slower), and the PSRAM
earns its place by absorbing the network stack automatically. The purchase is right
either way — the stated reason belongs on the hardware side to update, not to be
reasoned around here.

## Pin map

| GPIO | Net | Function | Firmware notes |
|---|---|---|---|
| 0 | `IO0_BOOT` | BOOT button → GND | strapping; also JP5.2; `WIFI_BUTTON` |
| 1 | `EVSE_TX` | UART1 TX → JP1.5 | 220 Ω series (R37); `TX1` |
| 2 | `EVSE_RX` | UART1 RX ← JP1.4 | `RX1`; **pull-up needed on v1.2** |
| 8 | `SDA` | I²C0 SDA | 4.7 kΩ pull-up on board |
| 9 | `SCL` | I²C0 SCL | 4.7 kΩ pull-up on board |
| 10 | `TFT_CS` | ILI9488 CSX | FSPI IO_MUX |
| 11 | `TFT_MOSI` | ILI9488 SDA | FSPI IO_MUX |
| 12 | `TFT_SCLK` | ILI9488 SCL | FSPI IO_MUX |
| 14 | `TFT_DC` | ILI9488 D/CX (RS) | |
| 15 | `BL_PWM` | backlight gate | **active HIGH**, 10 kΩ pull-down |
| **16** | `PWR_ST` | **v1.3** — TPS2116 status | **high = on USB, low = on EVSE 5 V** |
| **17** | `RTC_INT` | **v1.3** — DS3231 alarm | open-drain, **active low**, 10 kΩ pull-up |
| 18 | `LED_DIN` | WS2812B chain in | RMT |
| 19 | `USB_DM` | native USB D− | USB-Serial-JTAG |
| 20 | `USB_DP` | native USB D+ | USB-Serial-JTAG |
| 21 | `TFT_RST` | ILI9488 RESET | **active low** |
| 38 | `SD_CLK` | microSD CLK | |
| 39 | `SD_CMD` | microSD CMD | 10 kΩ pull-up |
| 40 | `SD_D0` | microSD DAT0 | 10 kΩ pull-up |
| 41 | `SD_CD` | card-detect switch | **active low, pull-up needed on v1.2** |
| 42 | `TEMP_ALERT` | MCP9808 ALERT | open-drain, 10 kΩ pull-up, **active low** |
| 43 | `S3_TXD0` | UART0 TX → JP5.5 | ROM/bootloader console; `Serial0` in app |
| 44 | `S3_RXD0` | UART0 RX ← JP5.4 | ROM/bootloader console; `Serial0` in app |
| 47 | `AUX_TX` | UART2 TX → JP2.5 | 220 Ω series (R38) |
| 48 | `AUX_RX` | UART2 RX ← JP2.4 | **pull-up needed on v1.2** |
| EN | `RESET` | RESET button | 10 kΩ + 1 µF, also JP5.6 |

**Unrouted / unavailable:** IO3, IO4, IO5, IO6, IO7, IO13, IO45, IO46 exist on the
module castellations but connect to nothing — bodge wire only. IO35–37 are consumed
by PSRAM. **IO16 and IO17 are also unrouted on v1.2**; they are spent in v1.3.

There is **no** `PILOTREAD` and **no ADC input of any kind** on either version. Pilot
state arrives over the RAPI link, not by measurement.

## Pull-ups — the difference that will bite you

**On v1.2 the internal pull-up is load-bearing on IO2, IO48 and IO41.** They read as
dead or stuck-low otherwise.

1. **IO2 (`EVSE_RX`)** and **IO48 (`AUX_RX`)** — level shifting is a bare 1N4148
   (D3, D2) with its **anode on the GPIO** and cathode on the 5 V side. The external
   driver pulls the node down through the diode; nothing pulls it back up. The low
   level sits at **~0.7 V, not 0 V** — comfortably under V<sub>IL</sub>, but not a
   rail-to-rail signal.
2. **IO41 (`SD_CD`)** — the detect switch shorts to GND on insertion. No external
   pull-up (R33's is on DAT3, a different net).

**v1.3** fits 10 kΩ externally on all three plus IO0 (`R39`–`R42`), so the internal
ones stop being load-bearing. Enabling them anyway is harmless and keeps one firmware
image working on both.

Firmware applies this for `EVSE_RX` via `RAPI_RX_PULLUP` (see `src/debug.cpp`).
It uses `gpio_set_pull_mode()`, **not `pinMode()`** — on Arduino-ESP32 3.x, `pinMode()`
runs the peripheral manager and would detach the UART from the pin it was just
configured on. `AUX_RX` and `SD_CD` are not handled yet only because nothing opens
UART2 and there is no SD driver — `SD_CD`'s pull-up becomes load-bearing the moment
one lands.

## I²C bus (IO8 SDA / IO9 SCL)

| Address | Device | Notes |
|---|---|---|
| `0x18` | MCP9808 temperature | A0/A1/A2 all tied GND; `ENABLE_MCP9808` |
| `0x68` | DS3231MZ RTC | CR2032 backup on BT1; `ENABLE_DS3231`, `src/rtc_ds3231.*` |
| — | Qwiic connector | external, 4-pin JST-SH |

The MCP9808's `ALERT` is wired to IO42 on both versions; the firmware polls over I²C
and does not use it.

The DS3231's `32KHZ` and `RST` are **not connected** on either version. Its `INT/SQW`
is **not connected on v1.2 — poll it**; on **v1.3** it reaches IO17, so alarms and
RTC-driven wake become available. The driver polls and uses no alarms, so it works
unchanged on both.

### Why the RTC is not optional

`tsdb_energy_logger` **discards every sample** until wall-clock passes
`TSDB_TIME_VALID_FLOOR` (2023-11-14), because writing a pre-NTP epoch would corrupt
the time index. This board is powered by the EVSE, so it loses power whenever the
EVSE does — and the case that costs the most data is the one where WiFi is also down
and NTP never answers. The coin cell (~225 mAh against the DS3231MZ's ~2.5 µA) is
what closes that hole.

- **Boot:** `rtc_begin()` + `rtc_seed_system_time()` run in `setup()` *before*
  `timeManager.begin()` and well before the logger starts, so `time(NULL)` is already
  past the floor and the logger needs no special case.
- **On sync:** `TimeManager::setTime()` writes back to the RTC (SNTP, `/time`, the
  login-supplied time), and so does `input.cpp`'s `handleRapiRead()` when it adopts
  the controller's clock. The controller is the weaker source — it is often the one
  *we* set — but it is also the only one available in exactly the case the cell
  exists for: WiFi down and NTP never answering. The floor check below is what keeps
  an unset controller from laundering a 1970 into the cell.
- **A dead or missing cell presents as "no time", never as 1970 or 2000-01-01.** The
  oscillator-stop flag is checked first, and the decoder rejects anything below the
  same floor. `RTC_TIME_VALID_FLOOR` and `TSDB_TIME_VALID_FLOOR` are held equal by a
  `static_assert`.

The register codec is pure and host-tested (`test/test_rtc_ds3231/`, run under
`native_test`) — it covers BCD validation, the 12-hour-mode read path, leap days,
the century bit, and non-existent dates such as 31 February. None of it is
hardware-validated yet.

## Power — v1.2 is blind, v1.3 is not

VBUS from USB-C and VBUS from the EVSE controller are merged by a TPS2116 priority
mux, then an AP63201 buck makes 3.3 V.

**On v1.2 no GPIO senses any of it** — firmware cannot tell USB from EVSE power, and
there is no coin-cell monitor.

**v1.3 wires the TPS2116's `ST` pin to IO16.** Per SLVSFG1A Table 5-1 it is an *"open
drain status pin, pulled low when VIN1 is not being used"*, and VIN1 is USB. So:

- **IO16 high** → running on **USB**
- **IO16 low** → running on the controller's **5 V**

10 kΩ pull-up (R43), 5 µs response. There is still no coin-cell monitor.

## Display — ILI9488, 320×480, 3.5"

**BuyDisplay ER-TFT035-6.** `IM[2:0]` all pulled to 3.3 V through 1 kΩ (R3/R6/R8) →
`111` = **4-wire 8-bit serial (SPI)**, per datasheet §4.1 Note 1. Identical on both
versions. Driven by the same LVGL renderer as the stock TFT
(`ENABLE_SCREEN_LVGL_TFT`, `src/lvgl_tft/`), over TFT_eSPI.

- **`TFT_MISO=-1` is correct, but the bus is not "write-only".** In the strapped mode
  the datasheet's own table reads `1 1 1 | 4-wire 8-bit data Serial Interface I |
  SCL, SDA/SDO, D/CX, CSX` — `SDA/SDO` is **one combined pin**, and pin 34 `SDA` is
  *"serial input or serial data input/output bi-direction."* **Readback returns on
  SDA, not SDO**, so wiring the (open) `SDO` would have bought nothing, and no MISO
  pin exists to give TFT_eSPI. TFT_eSPI keeps `-1` on the S3 (it only aliases MISO to
  MOSI on the C3/S2) and passes it straight to `spi_bus_config_t`.

  A bring-up ID check is therefore *possible*, just not through TFT_eSPI or
  `esp_lcd`'s panel-IO layer: it is a half-duplex read on IO11 (`SPI_DEVICE_3WIRE`
  drives MOSI bidirectionally), issued as a raw `spi_master` transaction **before**
  the panel driver takes the bus. Worth having on hardware that has never been
  powered on. Not implemented yet.
- **No tearing sync** — `TE` is not connected.
- **No touch** — the panel is behind a sealed cover; `XL`/`XR`/`YU`/`YD` are N/C, so
  `TOUCH_CS` is deliberately left undefined.
- All 18 parallel data pins plus `DE`, `PCLK`, `HSYNC`, `VSYNC` are tied to GND, as
  the datasheet requires for unused-bus operation.
- `RESET` on IO21 is independent of MCU reset — the panel can be re-inited without
  rebooting. (This was a v1.0.2 defect; don't assume the old behaviour.)
- IO10/11/12 are the FSPI **IO_MUX** pins, so the bus bypasses the GPIO matrix and
  can clock high — that was a deliberate board decision, not an accident. TFT_eSPI's
  default port on the S3 is `SPI` = FSPI = SPI2, which is what these pins belong to,
  so no `USE_HSPI_PORT`.
- **The renderer is shared with the shipped stock TFT board.** `openevse_wifi_tft_v1`
  and `openevse_s3_lcd` both pull in `${common.lvgl_tft_renderer_flags}`; there is no
  separate S3 panel layer. Any rework of `src/lvgl_tft/lvgl_panel.cpp` lands on
  hardware in the field unless it is conditioned on the board. Prove it on the S3
  first — the stock board has no PSRAM to stage a second buffer in, and if a change
  turns out marginal there, there is nothing to recall.
- **The build ships 40 MHz and 80 MHz needs measuring on the first board.** The wire
  format is 18 bpp (see below), so a full 15360-pixel draw buffer is 46 KB; at 40 MHz
  (5 MB/s) that is **~9.2 ms of blocked CPU per flush**, ~92 ms for a full repaint.
  80 MHz halves both. If it turns out marginal, that is the argument for 33 Ω series
  termination on `TFT_SCLK`/`TFT_MOSI` in v1.3 — **there is none anywhere on the TFT
  bus today**, and `TFT_MOSI` is 57.7 mm of board copper plus ~30 mm of flex tail
  against a ~75 mm critical length. v1.3 is designed but not ordered, so that window
  is open now and shuts at fab.

  **Do not backport a clock bump to the stock board on the strength of an S3 result.**
  80 MHz is available there in principle — its `TFT_MOSI=13` / `TFT_SCLK=14` /
  `TFT_CS=15` are the ESP32-classic HSPI IO_MUX pins, so it bypasses the GPIO matrix
  the same way — but that board is the QD354801 direct-solder 48-land part and this
  one is an ER-TFT035-6 on FPC through a ZIF. Same controller, different trace lengths
  and flex path. A DMA rework is a software risk that can be settled on a bench; a
  clock bump on shipped hardware is a signal-integrity bet that cannot be taken back.

### ⚠ Backlight: broken on v1.2, fixed on v1.3

`BL_PWM` on IO15 drives an AO3400A N-channel low-side switch — **active high**, 10 kΩ
pull-down, so the backlight is **off at reset**. Six LED cathodes each return through
a 120 Ω ballast.

**On v1.2, `J1.LEDA` is on the 3.3 V rail.** The design doc's fix (§12 item 8) calls
for `LEDA` → VBUS (5 V) *and* 3.9 Ω → 120 Ω; only the resistor half was applied, which
is the worst of the three combinations — 120 Ω only makes sense with 5 V above it.
Against a V<sub>F</sub> of 3.2 V typ you land on the knee of the diode curve at **~3–4
mA per string, ~20 mA total against 110 mA typ**, *dimmer than v1.0.2 managed*.

Expect a visibly dim backlight at 100 % duty, varying panel to panel. **PWM still
modulates it — this is a board defect, not a firmware problem. Do not chase it in
software.**

**v1.3 moves `LEDA` to VBUS**, giving 15 mA/string and 90 mA total, ±11 % across the
whole V<sub>F</sub> band.

## LEDs

Four **WS2812B** (3535) on IO18 via RMT, chained LED1 → LED2 → LED3 → LED4, and out to
**JP3** (3-pin: 3.3 V / GND / `LED_OUT`) for an external strip. Driven by the standard
`LedManagerTask` (`NEO_PIXEL_PIN=18`, `NEO_PIXEL_LENGTH=4`, `WIFI_PIXEL_NUMBER=1`),
same as the stock TFT board.

**They are specified below their own supply minimum on both versions.** The BOM part
is `WS2812B-MINI-V3/W` (`C527089`), rated **3.5 V–5.3 V**, running on a 3.3 V rail
whose own worst case is 3.27–3.42 V. Expect colour mixing to drift at low brightness,
and treat it as unreliable rather than broken.

Moving them to 5 V is *not* available as a workaround: V<sub>IH</sub> would become
0.7 × 5 V = 3.5 V, which a 3.3 V GPIO cannot drive. The identified fix is a die
revision — **`WS2812B-MINI-V6`, `C52941386`, rated 3.3 V–5.3 V**, same package and
same 1.9 mm height — but it is not applied to any BOM yet. Assume V3/W in hand.

## microSD — fitted

**J2 (DM3AT-SF-PEJM5) and R33 are fitted** on both versions — `parts.py:65` has
`DNP = {'JP5', 'JP6', 'J1'}` and J2 is not in it. Design doc §11: *"J2 and R33 are
fitted — the microSD was asked for, and R33 is not optional alongside it."*
IO38–41 are live pins, not documentation.

Wired for **SDMMC 1-bit**: CLK IO38, CMD IO39, D0 IO40, CD IO41, with 10 kΩ pull-ups
on CMD, D0 and DAT3.

- **R33 is functional, not decoration.** It holds `DAT3` high. If `DAT3` floats low
  during init the card latches into **SPI mode and never answers SDMMC**, which
  presents as a dead socket on good hardware. Check this first if bring-up fails.
- **1-bit is the only mode available.** DAT1/DAT2 reach no GPIO on either version.
  (**v1.3** adds pull-ups on them, R44/R45, but still no GPIO.)
- **Card-detect on IO41 is active low** and needs a pull-up: internal on v1.2,
  external R42 on v1.3.
- **A card swap requires opening the enclosure.** The socket mouth sits 1.46 mm
  inboard and push-push needs a full card length of travel. Design for a card that
  stays in, not one that gets rotated.
- **Any write can be cut mid-flight.** There is no power-fail warning on this board,
  and hold-up is ~400 µs against an SD card's 250 ms worst-case busy. This is not
  fixable in hardware — the on-card format has to be survivable (fixed-size records,
  sequence number, CRC; a torn record fails CRC and is skipped).

### Firmware

`ENABLE_SD_CARD`, `src/sd_card.*` (mount + presence) and `src/sdlog_store.*` (the
log itself).

**The card is preferred, not required.** With a card fitted the energy log lives on
it; with an empty slot the existing internal-flash tsdb path is used unchanged. The
two are never written together — one store owns the samples at a time, so doubling
the wear would buy nothing. `/status` reports `sd_status` and `sd_log`, because on a
board whose card sits behind a sealed enclosure, *"is it logging to the card or has
it quietly fallen back to flash?"* is not answerable by looking at it.

Fallback is also the runtime failure path: pulling a live card, or any write error,
marks the store not-ready and the next sample goes to flash. It does not retry a
broken card.

Capacity is 1 048 576 records × 32 B = 32 MB, about two years at one sample a
minute, against the internal tsdb's ~100 days in 2.5 MB. Holding more is the reason
to use the card at all. The file is created at full size up front so every later
write is an in-place overwrite rather than a growth that disturbs FAT metadata.

`/energy` serves from whichever store is live, through a small cursor that dispatches
between the two; the JSON is identical either way. Note the consequence: history
written to a card stays on that card, and swapping cards swaps the history with it.

The on-card format is `src/sdlog_record.*` and `src/sdlog_ring.*` — see the storage
section above for why it carries a CRC and a sequence number when the internal path
does not. Both are pure and host-tested; none of it is hardware-validated.

## Storage — where the energy history lives

The board design doc's §7 and the firmware disagree, and the disagreement is worth
resolving on paper before anyone acts on either. §7 says:

> **Do not use LittleFS for time series** — copy-on-write plus wear-levelling means
> write amplification on every append, and there is no indexing. Use a raw partition
> as a circular log of fixed-size records with a sequence number and CRC; a torn
> record fails CRC and is skipped.

`tsdb_energy_logger` writes `/littlefs/energy.tsdb` — a 2.5 MB ring, 1/min while
charging and 1/5 min idle. Checked against what `components/esp_tsdb` actually does,
**most of §7's prescription is already met and one part genuinely is not**:

| §7 asks for | esp_tsdb | |
|---|---|---|
| circular log | 1024-byte blocks of fixed-size records, LRU eviction by index | ✅ |
| fixed-size records | yes, `record_size` fixed at init | ✅ |
| indexing | `tsdb_index.c` maintains a time index | ✅ — the "no indexing" objection is about a naive file, not this |
| sequence number + CRC | `block_magic` only; **no CRC, no sequence** | ❌ |
| raw partition | hosted on LittleFS | ❌ |

So the open item is **torn-write survival**, not the ring structure. `block_magic`
distinguishes a never-written block from a written one; it cannot distinguish a
complete block from one cut in half. That matters more once P3-2 lands, because a
card write can be interrupted at any point (~400 µs of hold-up against a 250 ms
worst-case busy), and it is worth having regardless of where the bytes end up.

Two things constrain the "move it to a raw partition" half:

- **`tsdb_energy_logger` is shipped.** `openevse_wifi_tft_v1` and
  `openevse_wifi_v1_16mb` both build with `ENABLE_TSDB` and are hardware-validated.
  Changing the storage backend is not an S3-board change, and it carries a data
  migration for units already holding history.
- **`openevse_16mb.csv` is fully allocated** — app0 6 MB, app1 6 MB, spiffs 3.5 MB,
  coredump 64 KB, ending exactly at 0x1000000. There is no room for a new raw
  partition without repartitioning, which on shipped units means an OTA repartition.

**Recommendation, in order:** add the sequence number and CRC to the record format
first — it is the part that actually buys something, it is backend-independent, and
it can be done without touching the partition table. Treat the raw-partition move as
a separate question, driven by measured wear rather than by principle: at 1/min §7's
own arithmetic gives ~10 years and 244 years of wear headroom, so LittleFS
amplification is survivable either way on internal flash.

**On internal flash vs. the card:** §7's numbers favour keeping the append path on
internal flash — a 4 KB sector fills every 21 h at this cadence. The card is better
suited to what you physically remove: bulk retention and offload. Nothing about
P3-2 argues for moving the hot path onto it.

Whichever way it goes, **update the other document.** Right now a reader who finds
§7 first will conclude the firmware is wrong, and a reader who finds the firmware
first will conclude §7 is stale. Neither is quite true.

## Host interfaces

**JP1 — OpenEVSE RAPI** (JST-PH-6, mates with the controller's *FTDI Serial* header;
those labels are cable-centric, so "RX" is the pin the controller drives). This is
`RAPI_PORT = Serial1`:

| Pin | Controller | This board |
|---|---|---|
| 1 | Ground | GND |
| 2 | N/C | — |
| 3 | +5 V | `VBUS_EVSE` (powers this board) |
| 4 | RX | → D3 → IO2 |
| 5 | TX | ← IO1 |
| 6 | N/C | — |

**JP2 — AUX serial** — same connector and shape, on UART2 (IO47/IO48). Unused by
firmware.

**JP5 — programming pads**, 6 pads, silkscreened with **board-side** names: `GND` /
`IO0` / `3V3` / `RXD` / `TXD` / `RST`. An adapter's TX goes to the pad marked `RXD`.
No auto-reset circuit — drive DTR/RTS yourself or use the buttons. This is UART0 =
`DEBUG_PORT = Serial`.

**USB-C (JP4)** — native USB-Serial-JTAG on IO19/20 behind a USBLC6-2SC6. CC1 and CC2
have 5.1 kΩ pull-downs (sink only, no PD). Normal flashing path.

The board builds with `ARDUINO_USB_MODE=1` **and `ARDUINO_USB_CDC_ON_BOOT=1`**, so
`Serial` — and therefore `DEBUG_PORT` — is the **USB CDC device**: flash and read logs
over the one cable. Without CDC-on-boot, `Serial` would resolve to UART0 and you would
flash over USB-C while getting no output on it, needing a UART adapter on JP5 (which
has no auto-reset circuit).

Consequences of that choice:

- **UART0 is `Serial0`**, not `Serial`, if you ever want the JP5 pads from firmware.
- **ROM and second-stage bootloader output still goes to the JP5 pads**, since CDC
  only exists once the app is running. JP5 remains the place to watch a boot loop or
  a panic before `setup()`.

Boot buttons: **SW1 = BOOT** (IO0), **SW2 = RESET** (EN).

## Minor deviation worth knowing

Panel pin 35 `RD` is tied to **GND**; the datasheet asks for it to be fixed to IOVCC
when unused. It is not used at all in 4-wire serial mode, so this should be harmless —
but if the panel misbehaves at init, it is the one wiring choice that departs from the
datasheet.

## Building

```
pio run -e openevse_s3_lcd          # production
pio run -e openevse_s3_lcd_dev      # + debug flags, DEBUG_PORT on UART0
```

Flash over USB-C (`pio run -e openevse_s3_lcd -t upload`), or OTA once it is on the
network. `pio device monitor` on the same USB-C cable gives the firmware log
(`DEBUG_PORT` = USB CDC); the JP5 pads carry ROM/bootloader output. Partition table is `openevse_16mb.csv`, the same 6 MB/6 MB dual-OTA layout
the 16 MB WROOM and stock TFT boards use.
