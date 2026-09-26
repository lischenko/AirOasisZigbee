# AirOasis AOIA NCS / Zephyr Zigbee firmware

This is the current NCS/Zephyr implementation of the AirOasis Zigbee
controller. It targets the documented Teyleten Pro Micro-form-factor nRF52840
board, which identifies as a nice!nano-compatible device and retains its
installed Adafruit S140 v6 / UF2 bootloader. It uses **NCS 3.4.0** and
Nordic's supported **Zigbee R23 add-on 1.4.0** (ZBOSS 4.2.2.5), never the
legacy nRF5 SDK.

Read the repository-level [hardware, build, and flashing guide](../HARDWARE.md)
before using this firmware on an appliance.

## Hardware interface

All purifier interaction is through `uart0`; there are no direct fan, UV,
ionizer, or PM-sensor GPIOs.

| Item | Value |
|---|---|
| nRF target | nRF52840 QIAA, Teyleten/nice!nano-compatible board |
| UART | UART0, 9600 baud, 8 data bits, no parity, one stop bit, no flow control |
| nRF TX | P0.08 / board `D0` -> J1 `UART_TX` (purifier RX) |
| nRF RX | P0.06 / board `D1` <- J1 `UART_RX` (purifier TX) |
| Ground | J1 GND connected to nRF GND |
| Frame RX | 16 bytes: `AC ... 44`; first 15 bytes sum modulo 256 to `AC` |
| Frame TX | 12 bytes: `BB 02 flags speed 03 00... checksum 44` |
| PM2.5 | response byte 5, whole-number µg/m³ |
| Filter life | response bytes 9--10, `round(counter * 100 / 4400)`, capped at 100 |

The interrupt RX ring is retained because polling lost bytes under Zigbee
load. Commands are only created after a valid telemetry frame; the current
confirmed flags/speed are preserved in an option command. One command may be
outstanding. A new state frame confirms it; there is no speculative Zigbee
state change and no potentially harmful blind retry. Commands are inhibited
when telemetry is older than 35 seconds. This also allows changes originating
at the physical appliance to propagate to ZCL.

For the complete connection and power-safety procedure, see
[HARDWARE.md](../HARDWARE.md). In particular, do **not** connect J1 3V3 while
USB powers the board; use J1 3V3 -> board VCC only with USB disconnected, and
disconnect the OEM controller before connecting this board.

## Zigbee model

The node is a mains-powered **end device**, not a router and not a sleepy end
device (`RxOnWhenIdle=true`). It has six HA-profile endpoints. Each endpoint
has Basic metadata (manufacturer `AirOasis`, model `AOIA-NCS-ZB-v1`, a
per-endpoint ProductLabel) and Identify. ProductLabel is ZCL metadata, not a
request to arrange an HA UI.

| EP | HA device ID | Server clusters | Purpose |
|---:|---:|---|---|
| 1 | 0x0007 Combined Interface | Basic `0000`, Identify `0003`, Fan Control `0202`, PM2.5 Measurement `042A`, Analog Input `000C` | Primary purifier/fan, PM2.5, retained filter-life sensor |
| 2 | 0x0002 Generic Output | Basic, Identify, Binary Output `0010` | Ionizer |
| 3 | 0x0002 Generic Output | Basic, Identify, Binary Output | UV |
| 4 | 0x0002 Generic Output | Basic, Identify, Binary Output | Night / child lock |
| 5 | 0x0002 Generic Output | Basic, Identify, Binary Output | Max boost (preserves the physical fourth speed) |
| 6 | 0x0002 Generic Output | Basic, Identify, Binary Output | Main appliance power |

There are no manufacturer-specific clusters or attributes.

### Fan mapping

EP1 `0x0202` is a real Fan Control server: ZHA discovers it as a fan, writes
`FanMode`, and binds/reports that attribute. `FanModeSequence=0x02`
(`Low/Medium/High/Auto`) truthfully announces the standard modes that the
cluster can select:

| Purifier mode | RX code | ZCL FanMode | HA/ZHA representation |
|---|---:|---:|---|
| Off / power off | any | `0x00 Off` | fan off |
| Low | `0x13` | `0x01 Low` | low / 33% |
| Medium | `0x23` | `0x02 Medium` | medium / 66% |
| High | `0x33` | `0x03 High` | high / 100% |
| Auto | `0x11` (TX `0x41`) | `0x05 Auto` | `auto` preset |
| Max | `0x43` | no standard fourth speed | EP5 `Max boost` binary switch |

`FanMode=0x04` is called **On** by ZCL; it is not Max and is not present in
sequence 2, so it is rejected rather than silently misrepresenting Max. ZHA's
current generic Fan implementation may nevertheless list an `on` preset (and
may list `smart`) without consulting FanModeSequence. Such a selection gets a
standard Invalid Value response. This is a ZHA limitation, not a numeric
0--4 speed slider or a custom quirk requirement. The `Max boost` switch turns
Max on; turning it off from Max selects High because the appliance protocol
has no separate “exit boost, restore previous manual speed” command.

### Measurements and binary features

* **PM2.5**: EP1 `0x042A` MeasuredValue (`0x0000`, read-only/reportable,
  `single`) receives the existing raw whole µg/m³ value. Stock ZHA assigns its
  `pm25` device class and µg/m³ unit to this cluster. Min/Max are 0/255, the
  observed 8-bit appliance protocol range. No accuracy was established, so
  optional Tolerance is omitted.
* **Filter life**: EP1 Analog Input (`0x000C`)
  PresentValue (`0x0055`, percent, reportable) with Description `Filter life`.
  It is physically read-only: remote PresentValue writes are rejected while
  `OutOfService` stays false. ZHA exposes it as a measurement sensor.
* **Ionizer, UV, Night/child lock, Power and Max boost**: ZCL has no reasonable
  standard semantic cluster/attribute for these purifier-specific functions.
  Each is therefore a standard **Binary Output** (`0x0010`) server on its own
  functional endpoint. ZHA writes its standard `PresentValue` attribute and
  uses the standard Binary Output `Description` attributes `Ionizer`, `UV`,
  `Night / child lock`, `Max boost`, and `Power` as switch fallback names.
  This is why Binary Output is chosen over On/Off: both are standard binary
  controls, but On/Off has no equivalent ZHA-consumed descriptive attribute.

Firmware supplies device semantics, not dashboard layout, but this standard
cluster gives stock ZHA useful switch names without a quirk or a
manufacturer-specific cluster.

#### PM2.5 specification note

Public ZCL r8 text describes generic concentration `single` values as a
fraction of one, while stock ZHA's `0x042A` handler explicitly interprets the
value as µg/m³. The appliance reports an integer µg/m³, so this firmware sends
that direct value and a raw reading of 17 appears as **17 µg/m³** in stock ZHA.
This matches established ZHA PM2.5 interoperability, although it is a
standards ambiguity worth recording.

ZBOSS R23 4.2.2.5 does not treat `single` as an analog type when parsing or
reporting, so ZHA's standard Configure Reporting request would otherwise be
wrong. At configure time `scripts/patch_zboss.py` creates build-local copies
of the two Nordic-distributed ZBOSS source files, adds IEEE-754 `single`
parsing/delta reporting, and fails if expected v1.4.0 anchors are absent. It
does not alter or vendor licensed Nordic/ZBOSS SDK source.

## Reporting and state

The device honors coordinator Configure Reporting. First commissioning gives
safe defaults until ZHA replaces them:

| Attribute | Default min / max | Reportable change |
|---|---:|---:|
| FanMode | 1 s / 900 s | any |
| PM2.5 MeasuredValue | 30 s / 300 s | 1 µg/m³ |
| Filter PresentValue | 30 s / 3600 s | 1% |
| Filter StatusFlags | 1 s / 900 s | any |
| Each Binary Output PresentValue | 1 s / 900 s | any |

Attributes are only published from valid appliance responses. On stale
telemetry PM2.5 and filter life become unknown and filter reliability becomes
unreliable; fan and switch attributes retain their last confirmed, valid ZCL
value while all controls are inhibited. This prevents optimistic state changes when telemetry is unavailable.

## Build

Use a workspace with the exact manifest pinned in `ncs/west.yml`. With an NCS
3.4.0 toolchain installed (Zephyr SDK 1.0.1 is the matching toolchain used by
Zigbee add-on 1.4.0):

```sh
# From repository root; gets NCS 3.4.0 and ncs-zigbee 1.4.0 plus dependencies.
west init -l ncs
west update
west zephyr-export

# Build without sysbuild/MCUboot: the existing nice!nano UF2 bootloader owns
# 0xf4000..0xfffff and the preinstalled S140 occupies 0x00000..0x25fff.
west build --no-sysbuild -p always -b aoia_nrf52840 ncs -d ncs/build

# Optional host-only protocol test.
make -C tests test
```

`ncs/build/zephyr/zephyr.uf2` is the artifact to flash. Verified build layout:
application starts at `0x26000`, ends below `0x73400`; its reserved slot is
`0x26000..0xe8fff`, and it does not overwrite the SoftDevice or UF2
bootloader. Follow [HARDWARE.md](../HARDWARE.md) to enter the existing UF2
bootloader and copy the UF2 to the mounted `NICENANO` volume. Never mass erase
this board unless the bootloader and SoftDevice can be independently restored.

The USB-C port offers CDC ACM diagnostics/shell; it deliberately is not UART0,
so it cannot corrupt appliance traffic. `aoia status` prints confirmed state;
`aoia factory_reset confirm` removes Zigbee network state only. A factory
reset requires re-pairing but does not change appliance settings.

## Manual ZHA verification

1. Wire TX/RX crossed and common ground as above, power safely, and confirm
   `aoia status` progresses from unknown to valid state frames.
2. Factory-reset Zigbee only if the device was previously paired. In ZHA,
   enable Add Device, then boot the board; wait for the BDB interview.
3. Inspect the endpoint/cluster interview. Confirm one device has a Fan entity,
   a PM2.5 sensor, filter-life sensor, and named Binary Output switches. There must be no
   raw 0--4 fan slider.
4. Set Low, Medium, High and Auto in the fan entity. Confirm the physical
   mode, the valid reply frame, and a reported FanMode. Do not use ZHA's
   generic `on`/`smart` preset if it shows one; it is deliberately invalid.
5. Toggle Ionizer, UV, Night/child lock, main Power and Max boost individually;
   confirm each frame reply, physical result and reported Binary Output state. Toggle
   Max boost off and confirm it changes to High as documented.
6. Change a mode using the appliance panel/OEM interface, wait for its normal
   state frame, and check that ZHA updates without a manual refresh.
7. Compare PM2.5 and filter life to the appliance indication or another
   trusted reference. Check that
   PM2.5 is a µg/m³ sensor and reports after a ≥1 µg/m³ change or 300 seconds.
8. Reboot without factory reset and confirm ZHA re-interviews/rejoins without
   losing state. If a command is issued while no valid frame is available,
   verify it fails rather than changing UI state optimistically.

## Verification performed in this repository

* `make -C tests test` — passed. It tests frame checksum parsing/resync,
  filter conversion, Auto's asymmetric TX/RX encoding, all standard fan
  mappings, Max boost, option-bit preservation, and command checksums.
* `west build --no-sysbuild -b aoia_nrf52840 ncs -d ncs/build` — passed with
  NCS 3.4.0, Zigbee add-on 1.4.0 / ZBOSS 4.2.2.5 and Zephyr SDK 1.0.1. Final
  usage was 316,288 B / 780 KiB flash and 58,604 B / 256 KiB RAM.

The firmware has commissioned and operated successfully in three home
installations. The verification steps above remain recommended for each new
board and appliance revision.
