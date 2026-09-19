# Changelog

## 0.17.0

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
- `TactilityApp.cpp`: the app's `main()`, which Tactility calls on the app's
  own task. It installs the memory, filesystem and time providers
  (`TactilityTimeProvider`, over FreeRTOS ticks), loads the asset export from
  `F:/storage/`, runs `Deki::Main()`, and stops the engine's loop when the OS
  sends `APP_EVENT_CLOSE`. The app id comes from the platform's `appId`.
- **The simulator builds and runs end to end.** `platforms/tactility_simulator/`
  (`tactilityPlatform: posix-x86_64`, 640x480 RGB565) needs no ESP-IDF: its
  TactilitySDK is the simulator itself, built from the pinned sources and
  packaged with `release-sdk-posix.py`. The build compiles the engine,
  packages and game into the shared object the simulator loads, through the
  pinned `tactility.py`; the deploy step installs and runs it over the
  simulator's development service. Linux only (WSL works); the editor on
  Windows refuses the platform with that reason.
- An app is compiled with `-fno-gnu-unique`. GCC's unique symbols make glibc
  keep a library loaded forever, so a relaunch in the simulator ran on the
  previous launch's state and crashed.
- The backend checks `appId`, `appName` and `appVersion` against Tactility's
  own manifest rules, which otherwise refuse the app only at install. The
  app's name is the platform's `appName`, else the project's name.
- A deploy fails when `tactility.py` prints its failure mark: its exit code is
  0 for a refused install, and the device then answers `run` for an app it
  does not have.
- `TactilityDisplay` reports a draw the panel refuses (once, then a count);
  it ignored the result.
- **Window mode**: `TactilityDisplaySetup.mode` is `Panel` (take the whole
  display over and stop LVGL, as before; the default) or `Window`
  (`TactilityWindowDisplay`): the game runs in an app window from Tactility's
  window manager, with LVGL and the OS - status bar, launcher - still running,
  shown 1:1 at the platform's screen size in a canvas LVGL draws. Input then
  comes from LVGL events on that canvas, since LVGL owns the pointer and
  keyboard; LVGL reports a key only as it goes down, so it arrives as a press
  and an immediate release. Uses only functions Tactility exports to apps.
- The simulator platform runs in window mode at 320x240.
- In a window the game is placed where it would be on the bare panel,
  centred on the screen, with the OS's chrome drawn over it: a game the size
  of the screen fills it and the status bar hides its top rows.
- **The simulator is 320x240** (or whatever the platform says). Tactility
  fixes it at 640x480, so the SDK component applies one patch to the pinned
  sources: the size comes from `TACTILITY_SIMULATOR_RESOLUTION`. The patch is
  an exact match that refuses unrecognised sources, and the SDK records it,
  so one built before the patch shows as not installed.
- **Deploy installs, and only installs** ("Install"): the game appears in the
  OS's Apps menu under the project's name and is opened from there, instead
  of being started over whatever was on screen.
- Deploying to `localhost` starts the simulator at the platform's screen size,
  with its development service on, when none is running (log:
  `<toolchains>/tactility/<commit>/simulator.log`). One it started at another
  size is restarted at the platform's (it records what it started in
  `simulator.started`); one started any other way is used as it is.
- The key ids live in one header (`TactilityKeys.h`) for both input paths.

Verified in the simulator: deki-demo builds, installs, runs, relaunches and
closes. In window mode it renders and animates on the stock simulator with
the OS UI around it. In panel mode it runs but nothing shows there: the
stock simulator presents through an OpenGL SDL renderer bound to LVGL's
thread, so a raw-display app's draws from its own task go nowhere (a minimal
app with no Deki code shows the same; with the simulator's renderer switched
to software, panel mode renders too). Window-mode input is not exercised yet:
deki-demo takes none. Not yet run on hardware. A device app needs ESP32-S3 or ESP32-P4
(other chips cannot execute relocated app code from PSRAM), and building one
is not written yet: the game still has to be compiled as an ESP-IDF
component.
