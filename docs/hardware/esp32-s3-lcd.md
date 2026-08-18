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
| 43 | `S3_TXD0` | UART0 TX → JP5.5 | boot console; `DEBUG_PORT` |
| 44 | `S3_RXD0` | UART0 RX ← JP5.4 | boot console; `DEBUG_PORT` |
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
configured on. `AUX_RX` and `SD_CD` need no handling yet because nothing opens UART2
and the SD slot is unpopulated.

## I²C bus (IO8 SDA / IO9 SCL)

| Address | Device | Notes |
|---|---|---|
| `0x18` | MCP9808 temperature | A0/A1/A2 all tied GND; `ENABLE_MCP9808` |
| `0x68` | DS3231MZ RTC | CR2032 backup on BT1; **no firmware support yet** |
| — | Qwiic connector | external, 4-pin JST-SH |

The MCP9808's `ALERT` is wired to IO42 on both versions; the firmware polls over I²C
and does not use it.

The DS3231's `32KHZ` and `RST` are **not connected** on either version. Its `INT/SQW`
is **not connected on v1.2 — poll it**; on **v1.3** it reaches IO17, so alarms and
RTC-driven wake become available.

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

- **Write-only.** `SDO` is deliberately open (datasheet: "leave the pin open when not
  in use"), so the build sets **`TFT_MISO=-1`**. No register readback, no ID check, no
  `0x04`/`0x09` handshake. TFT_eSPI keeps `-1` on the S3 (it only aliases MISO to MOSI
  on the C3/S2) and passes it straight to `spi_bus_config_t`.
- **No tearing sync** — `TE` is not connected.
- **No touch** — the panel is behind a sealed cover; `XL`/`XR`/`YU`/`YD` are N/C, so
  `TOUCH_CS` is deliberately left undefined.
- All 18 parallel data pins plus `DE`, `PCLK`, `HSYNC`, `VSYNC` are tied to GND, as
  the datasheet requires for unused-bus operation.
- `RESET` on IO21 is independent of MCU reset — the panel can be re-inited without
  rebooting. (This was a v1.0.2 defect; don't assume the old behaviour.)
- IO10/11/12 are the FSPI **IO_MUX** pins, so the bus bypasses the GPIO matrix and can
  clock high. The build **starts at 40 MHz**; prove it on hardware before pushing to
  80. TFT_eSPI's default port on the S3 is `SPI` = FSPI = SPI2, which is what these
  pins belong to, so no `USE_HSPI_PORT`.

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

## microSD — **not populated**

J2 (DM3AT-SF-PEJM5) is marked **DNP** on both versions. Wired for **SDMMC 1-bit**
(CLK / CMD / D0, DAT3 held high by R33). No firmware support; if it is ever added,
guard the mount behind card-detect and treat absence as normal. **v1.3** adds pull-ups
on DAT1/DAT2 (R44/R45); neither reaches a GPIO on either version.

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
have 5.1 kΩ pull-downs (sink only, no PD). Normal flashing path. The board builds with
`ARDUINO_USB_MODE=1` and CDC-on-boot off, so `Serial` stays on UART0 and USB is the
JTAG/flash interface.

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
network. Partition table is `openevse_16mb.csv`, the same 6 MB/6 MB dual-OTA layout
the 16 MB WROOM and stock TFT boards use.
