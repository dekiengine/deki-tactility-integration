# deki-tactility-integration

Runs a Deki game as an external [Tactility](https://tactilityproject.org/) app,
instead of as firmware that owns the whole device.

Tactility is an operating system for ESP32 boards. It loads apps as relocatable
ELF files at runtime, so a game built this way installs onto a device that is
already running - over Wi-Fi, from an SD card, or through the on-device App
Hub - rather than replacing its firmware.

## What this package provides

| Deki provider | Backed by |
|---|---|
| `IDisplay` | `display_draw_bitmap()` on the raw panel driver |
| `IDekiInput` | `pointer_*` for touch, `keyboard_read_key()` for keys |
| `IFileSystem` | `F:/` → the app's assets directory, `S:/` → its user data |

Two boot SetupComponents bring them up: `TactilityDisplaySetup` and
`TactilityInputSetup`.

## Straight to the panel

Tactility ships LVGL and most apps draw with it. This one drives the panel
directly, the way Tactility's own `GraphicsDemo` does: the display setup stops
the LVGL module and blits the finished framebuffer itself.

Deki already renders a complete frame, so an LVGL canvas would just be a
surface to blit into. Going direct also keeps the app off the curated subset of
LVGL functions the `lvgl-module` exports, where a missing symbol fails when the
app is *loaded* rather than when it is built.

Input comes from the pointer and keyboard drivers, which are the *source* of
key events; LVGL only translates them into `LV_KEY_*`. Physical keyboards on
the T-Deck and Cardputer work.

## Band staging, and why

`display_draw_bitmap()` may DMA straight out of the pointer it is given:
Tactility defines `DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM` as a panel's promise
that it does *not*. Without that capability the source must be DMA-capable
memory, which on ESP32 means internal RAM - and a 320×240 RGB565 frame is 150 KB,
far too much to pin there.

So the framebuffer stays where the engine put it and rows are copied out through
a small internal staging band (8 rows, ~5 KB), which is also where a byte-swapped
panel format is converted. Panels that do advertise the capability skip the copy
entirely. This mirrors what `deki-lovyangfx-integration` already does.

`Deki::Rect` is `{left, top, right, bottom}` and half-open, which is exactly what
`display_draw_bitmap()` expects - its end coordinates are exclusive too - so
dirty rectangles pass through with no conversion.

## Supported hardware

**ESP32-S3 and ESP32-P4 only.**

An external app's code is relocated into RAM at load time, and only those two
chips can map PSRAM onto the instruction bus. The classic ESP32 and the C6 would
have to place app code in internal RAM, which is far too small for an engine.
This is a property of the chips, not of Tactility - Tactility itself supports
those boards, and small apps run on them fine.

## Status

**Unreleased and untested on hardware.** Every source file compiles against the
Tactility SDK (0.8.0-dev, built from `main`), but no build has yet run on a
device or in the simulator. The Tactility export target also needs an editor
change before it can be produced: builder registration goes through a closed
`MCUFamily` enum, and `BuilderRegistry::LoadBuilderPlugins()` - which already
exists - is never called.

A Tactility build must define `DEKI_TACTILITY_TARGET`, and must define
`DEKI_FAST_ATTR` as **empty**: the app is a relocated ELF, so there is no IRAM
placement to request (ESP-IDF firmware sets it to `IRAM_ATTR`).

## Licence

Apache-2.0. Tactility's SDK is Apache-2.0 and its licence explicitly allows
closed-source external apps; the GPL parts of Tactility are the firmware, which
an app does not link against.
