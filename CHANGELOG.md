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
- Build backend (`editor/TactilityBuilder.cpp`, framework `tactility`),
  registered when a project has this package installed. It validates the
  platform (esp32s3/esp32p4, an `appId`) and generates the app project.
  Building fails with a reason: the generated project does not yet contain
  the game.
- Its toolchain installs through the Build panel or `--install-toolchain`:
  ESP-IDF 6.1 (the same pinned component and install path as
  deki-esp32-integration, so the two share one copy), and the TactilitySDK,
  which Tactility 0.8 does not publish yet. The SDK is BUILT: Tactility's
  sources at a pinned commit (`65ee2238764d`, CI-green, ESP-IDF 6.1) are
  checked out, compiled for the chip's generic board and packaged with
  Tactility's own release script. The same component checks out TactilityTool
  at a pinned commit (`ad2d8a79e21e`, the ESP-IDF 6.1 update) for
  `tactility.py` and its `CDN/sdkconfig.app.<chip>` files, so an app build
  never needs Tactility's CDN.
- `platforms/tactility_esp32s3/`: the platform template and its boot scene.

Not yet run on hardware or in the simulator. Requires ESP32-S3 or ESP32-P4:
other supported chips cannot execute relocated app code from PSRAM.
