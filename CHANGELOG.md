# Changelog

## Unreleased

Initial package. Runs a Deki game as an external Tactility app rather than as
firmware.

- `TactilityDisplay` — `IDisplay` over the raw panel driver, no LVGL. Band-staged
  `display_draw_bitmap()` because a panel may DMA from the caller's buffer
  unless it advertises `DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM`; the staging copy
  is skipped on panels that do. Handles byte-swapped RGB565 and RGB888.
- `TactilityInput` — `IDekiInput` over `pointer_*` and `keyboard_read_key()`.
  Touch transitions are synthesised, since Tactility reports current points
  rather than events. Keyboard codepoints map onto Deki's key ids; enter, escape
  and backspace already coincide because both sides use the C0 control codes.
- `TactilityFileSystem` — `IFileSystem` mapping `F:/` to the app's assets
  directory and `S:/` to its user data directory.
- `TactilityDisplaySetup`, `TactilityInputSetup` — boot components.

Not yet run on hardware or in the simulator. Requires ESP32-S3 or ESP32-P4:
other supported chips cannot execute relocated app code from PSRAM.
