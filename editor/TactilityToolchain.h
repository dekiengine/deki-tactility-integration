#pragma once

// Everything a Tactility app build needs, as components the editor's Build
// panel (or --install-toolchain) installs:
//
//   esp-idf        ESP-IDF v6.1, the SDK Tactility itself builds with. The SAME
//                  pinned component, at the SAME install path, as
//                  deki-esp32-integration declares, so a machine that builds
//                  for both has one copy. The two pins must move together.
//   tactility-sdk  The TactilitySDK for the platform's chip. Tactility 0.8 has no
//                  published SDK yet, so this one is BUILT: Tactility's sources
//                  at a pinned commit are checked out and compiled for a generic
//                  board of that chip, then packaged with Tactility's own
//                  release script - exactly what its CI does to publish one.
//                  When 0.8.0 is released this becomes a pinned download.
//
// The app build tool comes from a second pinned checkout, TactilityTool: its
// tactility.py, and its CDN/sdkconfig.app.<chip> files - the app configuration
// tactility.py otherwise downloads from Tactility's CDN on first use. Placing
// them in an app's .tactility/ first means building an app never touches that
// CDN, which is not always reachable (it was not, from this machine, on the
// day this was written).

#include <atomic>
#include <functional>
#include <string>

namespace DekiEditor
{

struct TactilityPin
{
    // Head of Tactility main on 2026-09-13; firmware, simulator and test CI all
    // green, and built with ESP-IDF v6.1.
    static constexpr const char* kRepository = "https://github.com/TactilityProject/Tactility.git";
    static constexpr const char* kCommit = "65ee2238764d4e860fc8a3484c26b533b3975c90";
    static constexpr const char* kVersion = "0.8.0-dev";
    // What release-sdk-esp32.py records in the SDK and CI sets explicitly.
    static constexpr const char* kIdfMajorMinor = "6.1";

    // TactilityTool "Update sdkconfig for esp-idf v6.1" (2026-09-13): tool
    // v6.0.0, the version this Tactility commit's own copy reports.
    static constexpr const char* kToolRepository = "https://github.com/TactilityProject/TactilityTool.git";
    static constexpr const char* kToolCommit = "ad2d8a79e21ee14d0bafa967ac1cee45517d6d15";
};

class TactilitySdk
{
public:
    /// <toolchains>/tactility/<commit, 12 chars>
    static std::string Root();
    static std::string SourceDir();
    /// The TactilityTool checkout: tactility.py and CDN/sdkconfig.app.<chip>.
    static std::string ToolDir();
    /// Where the packaged SDKs live, in the layout tactility.py's --local-sdk
    /// expects: <base>/<version>-<target>/TactilitySDK. It is what an app build
    /// sets TACTILITY_SDK_PATH to.
    static std::string SdkBase();
    /// The packaged SDK for one chip.
    static std::string SdkDir(const std::string& idfTarget);

    /// Whether the SDK for this chip has been built and the tool checked out.
    /// The release script writes idf-version.txt last, so its presence means
    /// packaging finished.
    static bool IsBuilt(const std::string& idfTarget);

    /// Check out the pinned sources if needed, build them for `idfTarget` and
    /// package the SDK. Runs in `idfPath`'s environment. Output goes to onLine.
    /// Returns an empty string on success, else what went wrong.
    static std::string Build(const std::string& idfTarget, const std::string& idfPath,
                             const std::function<void(const std::string&)>& onLine,
                             const std::atomic<bool>* cancel);
};

}  // namespace DekiEditor
