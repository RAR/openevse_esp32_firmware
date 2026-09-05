# fake_evse_host — a fake OpenEVSE controller that runs on your PC

Speaks RAPI over a USB-serial dongle so an ESP32 running this firmware sees a
live charger with **no OpenEVSE controller attached**. Every frame is logged
both ways with a timestamp; that log is the point of the tool.

The protocol brain is `src/fake_evse_core.{h,cpp}`, shared verbatim with the
in-firmware `FAKE_EVSE` build and with the doctest suite in
`test/test_fake_evse/` (`pio test -e native_test -f test_fake_evse`). Only the
transport, the control channel and the logging live here.

## Build

```sh
make -C tools/fake_evse_host
```

Plain `g++`; no PlatformIO, no Arduino, no dependencies.

## Run

```sh
tools/fake_evse_host/fake_evse_host --port /dev/ttyUSB0 --log /tmp/rapi.log
```

Options: `--baud` (115200), `--control-port` (9910, `0` disables), `--quiet`
(log to `--log` only), `--vehicle` / `--voltage` / `--current` for the initial
state, and `--pty`, which allocates a PTY and prints its name on the first
stdout line instead of opening a port — handy for testing the tool itself or
for pointing a native firmware build at it.

The tool never touches DTR/RTS. On a board wired straight to an ESP32 UART
those lines are the reset/boot straps and toggling them reboots the target.

## Control channel

Line commands on stdin and on `127.0.0.1:<control-port>`; each one replies with
the full state. These mirror the old in-firmware `POST /fakeevse` knobs.

```
vehicle on|off      plug / unplug the car (plugging in resets the session)
fault none|gfci|noground|stuck|overtemp
voltage <volts>
current <amps>      pilot/charge current, the same field $SC writes
flicker on|off      pilot-state flicker (event-log flood repro)
temp <degC>         ambient temperature reported by $GP
status | help | quit
```

```sh
exec 3<>/dev/tcp/127.0.0.1/9910; printf 'vehicle on\n' >&3; head -2 <&3; exec 3<&-
```

## Wiring

ESP32-S3 LCD board, header **JP1** (JST-PH-6, labels are cable-centric):

| JP1 pin | signal | dongle |
| --- | --- | --- |
| 1 | GND | GND |
| 3 | +5 V (`VBUS_EVSE`) | leave unconnected when the board is on USB |
| 4 | board RX (IO2) | **TXD** |
| 5 | board TX (IO1) | **RXD** |

115200 8N1. On v1.2 boards the RX line needs the ESP32's internal pull-up; the
firmware applies it (`RAPI_RX_PULLUP`).

On the older WROOM/TFT boards the RAPI link is UART0 — the same pins as the
flashing header and the debug console — so the tool sees the firmware's debug
output interleaved with the RAPI frames. It logs those lines as `NOISE` and
carries on.

## Reading the log

```
RX  <- $GS^30      TX -> $OK 3 41 3 140^1F\r     a command and its reply
TICK-> $AT 1 1 32 0^00 (state 3->1)              async state change, 1 Hz
NOISE  <- [45540533][E][Wire.cpp:532] ...        non-RAPI text on the UART
UNHANDLED $GZ -- core has no case, replying bare $OK
```

`UNHANDLED` is the line to watch for. `fake_evse_handle()` answers `$OK` to
anything it does not recognise, which keeps the link alive but hides a firmware
that has started asking for something new; the tool cross-checks each verb
against the core's explicit cases and says so once per verb.
