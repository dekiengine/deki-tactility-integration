#pragma once

// Everything a Tactility app build needs, as components the editor's Build
// panel (or --install-toolchain) installs:
//
//   esp-idf        ESP-IDF v6.1, the SDK Tactility itself builds with. The same
//                  pinned component, at the same install path, as
//                  deki-esp32-integration declares, so a machine that builds
//                  for both has one copy. The two pins must move together.
//   tactility-sdk  The TactilitySDK for the platform, with the app build tool
//                  that comes with it. Tactility 0.8 has no released SDK, so
//                  it is built: Tactility's sources at a pinned commit are
//                  checked out and compiled (for a generic board of the chip,
//                  or as the simulator for a host platform), then packaged
//                  with Tactility's own release scripts, as its CI does. The
//                  result has the layout Tactility's tt uses:
//                      <root>/sdk/<version>/platform-<platform>/TactilitySDK
//                      <root>/sdk/<version>/tools/app.py
//                  so an app builds with TACTILITY_SDK_PATH=<root>. Once 0.8.0
//                  is released the device SDKs can be a pinned download.
//
// A host platform ("linux-x86_64") is the Tactility simulator, which runs on
// Linux only. It needs no ESP-IDF; its SDK is the simulator's own build, and an
// app for it is a shared object the simulator loads. The simulator has a few
// device profiles; the deploy step picks the one of the platform's screen size.
//
// The device SDK gets one Deki line in the app configuration it ships
// (sdkconfig.app.<chip>, which app.py copies into every app): the firmware's
// optimisation level, which the default (-Og) would cost a game several times
// over.

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

struct TactilityPin
{
    // Head of Tactility main on 2026-11-15, still 0.8.0-dev and built with
    // ESP-IDF v6.1.
    static constexpr const char* kRepository = "https://github.com/TactilityProject/Tactility.git";
    static constexpr const char* kCommit = "6e4b989f76cf39dd7a6b3bbc6416d69ffc059c72";
    static constexpr const char* kVersion = "0.8.0-dev";
    // What release-sdk-esp32.py records in the SDK and CI sets explicitly.
    static constexpr const char* kIdfMajorMinor = "6.1";
};

class TactilitySdk
{
public:
    /// <toolchains>/tactility/<commit, 12 chars>. What an app build sets
    /// TACTILITY_SDK_PATH to: the SDKs are in its sdk/<version>/.
    static std::string Root();
    static std::string SourceDir();
    /// The app build tool of the SDK: app.py and the tools it runs.
    static std::string ToolDir();
    /// The packaged SDK for one platform: a chip ("esp32s3") or the simulator
    /// ("linux-x86_64").
    static std::string SdkDir(const std::string& platform);

    /// Whether `platform` is the simulator, which Tactility names after the
    /// computer it runs on (<os>-<architecture>), rather than a chip.
    static bool IsPosix(const std::string& platform);

    /// Every symbol a device app may leave for the firmware to resolve: what
    /// the kernel modules of the pinned Tactility export to apps, read from
    /// its sources (Modules/, TactilityKernel/, the chip's platform; never a
    /// board's drivers, which only some devices have). Cached beside the SDK.
    /// Empty, with `error` set, when the sources are not there.
    static std::vector<std::string> AppExports(const std::string& platform, std::string& error);

    /// Script lines that enter `idfPath`'s ESP-IDF environment: a .bat
    /// fragment on Windows, a sh fragment elsewhere. Ends in the ESP-IDF
    /// directory; the caller cds where it needs to be.
    static std::string IdfEnvironmentScript(const std::string& idfPath);

    /// Windows: the first lines of a .bat that runs ESP-IDF, when the editor's
    /// PATH holds an entry with a stray quote (a Windows Kits installer leaves
    /// one). cmd reads from that quote on as one quoted run, so a
    /// `set "PATH=...%PATH%"` ends early and nothing behind the entry is found:
    /// export.bat then reports git missing. These lines set PATH without the
    /// quotes and say which entry had one. Empty when PATH is fine, and on
    /// other systems.
    static std::string PathRepairScript();

    /// Whether the SDK for this platform and its tools are built. Each release
    /// script writes a known file last, so that file means packaging finished.
    static bool IsBuilt(const std::string& platform);

    /// Checks out the pinned sources if needed, builds them for `platform` and
    /// packages the SDK. A chip builds in `idfPath`'s ESP-IDF environment; the
    /// simulator ignores it. Output goes to onLine. Returns an empty string on
    /// success, else what went wrong.
    static std::string Build(const std::string& platform, const std::string& idfPath,
                             const std::function<void(const std::string&)>& onLine, const std::atomic<bool>* cancel);

    /// Makes sure a simulator is answering on this machine: uses one that
    /// already is, or starts the built one with the device profile of
    /// `width`x`height` and its development service on, and waits for it.
    /// Returns an empty string on success.
    static std::string EnsureSimulator(int width, int height, const std::function<void(const std::string&)>& onLine,
                                       const std::atomic<bool>* cancel);
};

}  // namespace DekiEditor
