# AirOasis Zigbee controller

> [!WARNING]
> This is an unofficial, experimental hardware modification for selected
> AirOasis AOIA-2-series air purifiers. It can damage the appliance, void a
> warranty, or leave it unusable. It is not affiliated with AirOasis. Proceed
> only if you understand the risks and can restore your own hardware.

This repository contains a local **Zigbee end-device** replacement controller
for compatible AirOasis purifiers. It uses an nRF52840 board and the
appliance's internal UART; it does not use the AirOasis cloud.

The implementation is a Nordic nRF Connect SDK (NCS)/Zephyr application.
The related [AirOasisEsphome](https://github.com/lischenko/AirOasisEsphome)
project inspired this work and targets the original ESP32 Wi-Fi controller,
rather than this separate nRF52840 Zigbee replacement approach.

## Status

The NCS firmware builds and its hardware-independent protocol tests pass. It
has also commissioned and operated successfully in three home installations.
Treat this repository as a project for experienced tinkerers, not a
ready-to-install product.

Read [HARDWARE.md](HARDWARE.md) before connecting anything to a purifier.

## What it provides

- Zigbee HA-profile end device for Home Assistant ZHA
- Local control of power, fan mode, UV, ionizer, and night/child lock
- PM2.5 and filter-life telemetry from checksum-validated appliance UART frames
- Reply-reconciled state: a command is not represented as confirmed until the
  purifier returns a valid state frame
- Standard Zigbee clusters where practical, without a custom ZHA quirk

The device is configured as a mains-powered end device, not a Zigbee router or
sleepy device.

## Supported hardware

The current board definition targets a Teyleten Pro Micro-form-factor nRF52840
board that identifies as a nice!nano-compatible UF2 device. Its installed
bootloader and memory layout are part of the firmware contract. Other
nRF52840 boards, bootloaders, or purifier revisions are **not supported**
without porting and verification.

The appliance connection is a 3.3 V, 9600-baud UART. See
[HARDWARE.md](HARDWARE.md) for the pin mapping, safe power choices, and
installation photographs.

## Build and install

1. Read [HARDWARE.md](HARDWARE.md) in full.
2. Build the NCS application using the pinned workspace manifest:
   ```sh
   west init -l ncs
   west update
   west zephyr-export
   west build --no-sysbuild -p always -b aoia_nrf52840 ncs -d ncs/build
   ```
3. Run the host-side protocol test:
   ```sh
   make -C tests test
   ```
4. Copy `ncs/build/zephyr/zephyr.uf2` to the board's `NICENANO` UF2 volume as
   described in [HARDWARE.md](HARDWARE.md). Do not mass-erase the board.
5. Wire and commission the device only after confirming the build and the
   power arrangement.

`ncs/README.md` describes the Zigbee endpoint model, protocol behavior, and
NCS-specific details.

## Repository layout

- `ncs/` — current NCS/Zephyr Zigbee firmware and pinned west manifest
- `tests/` — host-only AOIA protocol/mapping tests
- `HARDWARE.md` — board, build, flashing, power, and wiring documentation

## Related project and inspiration

The [AirOasisEsphome](https://github.com/lischenko/AirOasisEsphome) project
contains the separate ESPHome implementation for the original ESP32 Wi-Fi
controller.
