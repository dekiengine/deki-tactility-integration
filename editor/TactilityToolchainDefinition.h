#pragma once

// The downloadable part of the Tactility toolchain: ESP-IDF v6.1.
//
// This is deki-esp32-integration's esp-idf component - same id, same pinned
// archive and digest, same install path - so the two packages share one ESP-IDF
// on disk. The one difference is the setup step, which also installs the
// ESP32-P4's RISC-V compiler, since an app can target either chip. Tactility
// main builds with v6.1 as well. If the two pins
// ever have to differ, the install paths must differ too, or each package would
// keep replacing the other's SDK.
//
// The TactilitySDK itself is not here: 0.8 has no published archive, so it is
// built from source (TactilityToolchain.h).

namespace DekiEditor
{

inline constexpr const char* kTactilityToolchainDefinition = R"json(
{
  "id": "tactility",
  "name": "Tactility",
  "family": "espressif",
  "components": [
    {
      "id": "esp-idf",
      "displayName": "ESP-IDF SDK",
      "required": true,
      "canInstall": true,
      "canSetup": true,
      "installPath": "espressif/esp-idf",
      "versionCheck": {
        "type": "github_release",
        "repo": "espressif/esp-idf",
        "assetPattern": ".zip"
      },
      "fallback": {
        "version": "v6.1",
        "url": "https://github.com/espressif/esp-idf/releases/download/v6.1/esp-idf-v6.1.zip",
        "sha256": "cdeea7db47b90064ef185b2a1f1b33d17bcb13469a9f8cc20e06c4c4cdb4cc16"
      },
      "detection": {
        "windows": "{installPath}/export.bat",
        "unix": "{installPath}/export.sh"
      },
      "postInstall": {
        "windows": "{installPath}/install.bat esp32s3,esp32p4",
        "unix": "{installPath}/install.sh esp32s3,esp32p4"
      },
      "tooltip": "Espressif IoT Development Framework (shared with the ESP32 package)"
    }
  ]
}
)json";

}  // namespace DekiEditor
