# MacroZ

MacroZ is wireless firmware and a browser-based configurator for the Labib Macropad, a 3x3 pad
built around a nice!nano v2-compatible nRF52840 controller.

It uses ZMK for BLE/USB HID, bonding, low-power operation, matrix scanning, and UF2 output. A small
out-of-tree ZMK module adds persistent runtime key assignments, reusable macros, and an encrypted
Bluetooth GATT configuration service. The web app talks directly to that service with Web
Bluetooth; no account, native utility, or cloud API is involved.

## Features

- BLE and USB keyboard operation through ZMK
- Nine independently remappable keys
- Keyboard chords with Ctrl, Shift, Alt, and GUI modifiers
- Consumer/media controls
- Six reusable macro slots
- Up to 255 key, chord, or media actions per macro
- Configurable delay after every macro action
- Configuration saved in the controller's flash
- Chunked, versioned, validated, encrypted BLE configuration protocol
- Responsive web configurator for desktop and mobile Chromium browsers
- Reproducible ZMK build pinned to a known ZMK revision

## Hardware

The supplied `labib_macropad` shield uses the Labib Macropad's direct 3x3 matrix wiring:

| Matrix signals | nRF52840 GPIOs |
| --- | --- |
| Rows 0 through 2 | P1.11, P1.15, P0.29 |
| Columns 0 through 2 | P0.08, P0.20, P0.24 |
| Diode direction | Column to row |

All nine matrix coordinates are exposed in row-major order. The matrix and transform are defined in
`boards/shields/labib_macropad/labib_macropad.overlay`.

The build target is `nice_nano//zmk`, which currently means the nice!nano v2 revision. A clone must
have an nRF52840, a nice!nano-compatible pinout, and a compatible UF2 bootloader. Boards sold as a
nice!nano clone sometimes use different bootloaders, so verify that detail before flashing.

## Build Firmware

### GitHub Actions

Push the repository to GitHub and run **Build firmware** under Actions. The `firmware` artifact
contains `labib-macropad-nice-nano-v2.uf2`.

### Local ZMK build

Use an isolated west workspace so Zephyr's own `zephyr/` project does not overlap the module
metadata in this repository:

```sh
MACROZ=/path/to/macroz
WORKSPACE=$(mktemp -d)
mkdir "$WORKSPACE/config"
cp -R "$MACROZ/config/." "$WORKSPACE/config/"
cd "$WORKSPACE"
west init -l config
west update
west zephyr-export
west build -s zmk/app -b nice_nano//zmk -- \
  -DSHIELD=labib_macropad \
  -DZMK_CONFIG="$WORKSPACE/config" \
  -DZMK_EXTRA_MODULES="$MACROZ"
```

The UF2 file is produced under `build/zephyr/`.

## Flash And Pair

1. Connect the controller over USB.
2. Double-tap reset to mount the controller's bootloader drive.
3. Copy `labib-macropad-nice-nano-v2.uf2` to that drive.
4. Pair **Labib Macropad** in the operating system's Bluetooth settings.
5. Open the web configurator and choose **Connect pad**.

If this controller previously ran an older MacroZ build, its persisted assignments still take
priority over the new defaults. Choose **Factory layout** once to load `F13` through `F21`.

ZMK stores Bluetooth bonds. If a host cannot reconnect after substantial GATT or firmware changes,
remove the pad from the host's Bluetooth settings and clear the controller's ZMK settings before
pairing again.

## Web Configurator

```sh
cd web
corepack enable
pnpm install
pnpm run dev
```

Open the localhost URL printed by Vite. Web Bluetooth requires a secure context, so a deployed app
must use HTTPS; localhost is accepted for development. Chrome and Edge on ChromeOS, macOS, and
Windows, plus Android Chrome, support Web Bluetooth. On Linux Chrome, enable **Experimental Web
Platform features** at `chrome://flags/#enable-experimental-web-platform-features`, relaunch Chrome,
and reload the app. Firefox and Safari currently do not support Web Bluetooth.

The app can edit a layout without a device, but **Save to pad** becomes available only after a BLE
connection. Changes become active immediately after a successful save and are persisted shortly
afterward to reduce flash wear.

## Macro Semantics

A macro is a sequence of taps. Each action is pressed for 20 ms, released, and followed by its
configured delay. Macro invocations are queued, so repeated pad presses do not interrupt an action
already in progress. Six macro slots can be shared by any number of the nine physical keys.

The current protocol intentionally supports keyboard and consumer HID pages. Text macros are built
from key actions rather than stored as characters, which keeps behavior predictable across host
keyboard layouts.

## Project Layout

| Path | Purpose |
| --- | --- |
| `boards/shields/labib_macropad/` | Labib Macropad matrix, 3x3 layout, and keymap |
| `src/macroz_dynamic.c` | Dynamic behavior, macro runner, settings, and GATT service |
| `include/macroz/protocol.h` | Firmware wire-format definitions |
| `config/west.yml` | Pinned ZMK west manifest |
| `build.yaml` | ZMK build target |
| `web/` | TypeScript/Vite Web Bluetooth configurator |

## Protocol

Protocol v2 uses a fixed 9,272-byte little-endian record. Reads select and fetch sequential 256-byte
windows from one GATT characteristic. Writes use `BEGIN`, sequential 16-byte `CHUNK` packets, then
`COMMIT`; firmware validates the entire record before replacing the active configuration. Flash
persistence uses a core record plus one record per macro so every value fits in an NVS sector. The
firmware migrates valid protocol v1 assignments on first boot. `RESET` restores `F13` through `F21`.
GATT access requires an encrypted BLE connection.

Protocol UUIDs and the TypeScript codec are in `web/src/protocol.ts`. Any format change must bump
`MACROZ_PROTOCOL_VERSION` and `PROTOCOL_VERSION` together.
