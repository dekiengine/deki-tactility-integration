// Builds a Deki game as an external Tactility app instead of as firmware.
//
// Ships in this package and loads through the editor's builder-plugin path, so
// the editor knows nothing about Tactility. It is the first real consumer of
// that path, and deliberately so: Tactility is not under NDA, which makes it
// the only chance to prove the surface works while the code is still something
// anyone can read. A console backend gets no such luxury.
//
// What Tactility wants, and why this does not look like the ESP-IDF builder:
//   - the artifact is a relocatable ELF loaded at runtime, not a firmware image
//   - the app is built by ESP-IDF against a downloaded TactilitySDK, through a
//     root CMakeLists that calls tactility_project_pre/post
//   - there is no flashing: an app is installed over Wi-Fi, from an SD card, or
//     through the on-device App Hub

#include <deki-editor/build/BuilderPlugin.h>
#include <deki-editor/build/CMakeGenUtils.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/TargetBuilder.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace DekiEditor;

namespace
{

/// Chips whose silicon can execute a relocated ELF out of PSRAM.
///
/// Espressif's elf_loader can only map PSRAM onto the instruction bus on these
/// two. On the classic ESP32 and the C6 an app's code must live in internal
/// RAM, and an engine does not fit there — Tactility itself runs fine on those
/// boards, and small apps do too, but not this one. Refusing here is much
/// kinder than a -ENOMEM at load time on the device.
const std::vector<std::string>& ExecutableFromPsramTargets()
{
    static const std::vector<std::string> kTargets = { "esp32s3", "esp32p4" };
    return kTargets;
}

std::string Join(const std::vector<std::string>& v, const char* sep)
{
    std::string out;
    for (const auto& s : v)
        out += (out.empty() ? "" : sep) + s;
    return out;
}

class TactilityBuilder : public ITargetBuilder
{
   public:
    const char* GetName() const override { return "Tactility App"; }
    std::string GetFrameworkId() const override { return "tactility"; }
    const char* GetDescription() const override
    {
        return "Build as a loadable app for a device already running Tactility";
    }

    std::vector<std::string> ValidatePlatform(const PlatformConfig& config) const override
    {
        std::vector<std::string> problems;

        const std::string target = config.Option("idfTarget");
        if (target.empty())
        {
            problems.push_back("idfTarget is not set; a Tactility platform must name its chip");
        }
        else
        {
            const auto& ok = ExecutableFromPsramTargets();
            if (std::find(ok.begin(), ok.end(), target) == ok.end())
                problems.push_back(
                    "idfTarget '" + target +
                    "' cannot run a Deki app: an external app's code is relocated into RAM at "
                    "load, and only " + Join(ok, " and ") +
                    " can execute it from PSRAM. Tactility supports other chips, but an engine "
                    "does not fit in their internal RAM.");
        }

        if (config.Option("appId").empty())
            problems.push_back("appId is not set; Tactility keys an app's assets and user data "
                               "off it (e.g. 'com.example.mygame')");

        return problems;
    }

    // No deploy step yet. A Tactility app is installed over Wi-Fi
    // (tactility.py install <ip>), from an SD card or through the App Hub,
    // never flashed; until one of those is wired up the editor shows no
    // button rather than one that cannot work. When it is, the deploy targets
    // are device addresses, not serial ports.
    bool SupportsDeploy() const override { return false; }

    // The app bundle is assembled by tactility.py from the build directory;
    // nothing reads a separate boot payload yet, so none is asked for.
    std::string GetBootPayloadDirectory(const std::string&) const override { return ""; }

    bool GenerateBuildFiles(const std::string& projectPath, const PlatformConfig& config,
                            const std::vector<std::string>& packageDefines) override
    {
        const fs::path root = fs::path(GetBuildDirectory(projectPath));
        std::error_code ec;
        fs::create_directories(root / "main", ec);

        const std::string appId = config.Option("appId");
        const std::string appName = config.displayName.empty() ? appId : config.displayName;
        const std::string target = config.Option("idfTarget");

        // The tool reads this to build the .app bundle. target.platforms names
        // only the chips that can actually run it.
        {
            std::ofstream f(root / "manifest.properties");
            f << "manifest.version=0.3\n"
              << "target.sdk=" << config.Option("targetSdk", "0.8.0-dev") << "\n"
              << "target.platforms=" << Join(ExecutableFromPsramTargets(), ",") << "\n"
              << "id=" << appId << "\n"
              << "name=" << appName << "\n"
              << "version.name=" << config.Option("appVersion", "0.1.0") << "\n"
              << "version.code=" << config.Option("appVersionCode", "1") << "\n"
              << "app.0.id=" << appId << "\n"
              << "app.0.name=" << appName << "\n"
              << "app.0.binary=" << appId << "\n";
        }

        // Generated by the tool normally; the marker comment is what it looks
        // for to decide whether it may rewrite the file.
        {
            std::ofstream f(root / "CMakeLists.txt");
            f << "# tactility-cmakelists-version: 1\n"
              << "# Generated by the Deki Tactility builder. Do not edit.\n"
              << "cmake_minimum_required(VERSION 3.20)\n"
              << "include(\"$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake\")\n"
              << "tactility_project_pre(" << appId << ")\n"
              << "project(" << appId << ")\n"
              << "tactility_project_post(" << appId << ")\n";
        }

        {
            std::ofstream f(root / "main" / "CMakeLists.txt");
            f << "# Generated by the Deki Tactility builder. Do not edit.\n"
              << "include(\"$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake\")\n\n"
              << "file(GLOB_RECURSE SOURCE_FILES Source/*.c*)\n"
              << "tactility_component_register(SRCS ${SOURCE_FILES} INCLUDE_DIRS \"Include\")\n\n"
              << "target_compile_definitions(${PROJECT_NAME} PRIVATE\n"
              << "    DEKI_TACTILITY_TARGET\n"
              // Empty, deliberately: the app is a relocated ELF, so there is no
              // IRAM placement to ask for. Undefined, deki/Color.h will not
              // even compile.
              << "    DEKI_FAST_ATTR=\n";
            for (const auto& define : packageDefines)
                f << "    " << define << "\n";
            for (const auto& define : config.defines)
                f << "    " << define << "\n";
            f << ")\n";
        }

        return true;
    }

    std::string GetBuildDirectory(const std::string& projectPath) const override
    {
        return projectPath + "/generated/build/tactility";
    }

    void Build(const std::string& projectPath, BuildOutputCallback out,
               BuildProgressCallback) override
    {
        // Not yet: this drives tactility.py, which needs ESP-IDF 5.5 and a
        // TactilitySDK. Generation is what is proven so far, and claiming more
        // than that in a status line would be a lie.
        if (out)
            out("Tactility: build files generated in " + GetBuildDirectory(projectPath) +
                    ". Running tactility.py is not wired up yet.",
                false);
        m_State = BuildState::Completed;
    }

    // --- not yet meaningful for this target ---
    void Clean(const std::string& projectPath, BuildOutputCallback, BuildProgressCallback) override
    {
        std::error_code ec;
        fs::remove_all(GetBuildDirectory(projectPath), ec);
    }
    void Cancel() override {}
    bool IsBuilding() const override { return false; }
    BuildState GetState() const override { return m_State; }
    BuildProgress GetProgress() const override { return {}; }
    void SetBuildOptions(const BuildOptions& o) override { m_Options = o; }
    const BuildOptions& GetBuildOptions() const override { return m_Options; }
    void SetPlatformConfig(const PlatformConfig& c) override { m_Config = c; }
    void ClearPlatformConfig() override { m_Config = {}; }
    void SetPackageDefines(const std::vector<std::string>& d) override { m_Defines = d; }
    bool IsToolchainInstalled() const override { return true; }
    std::string GetToolchainStatus() const override
    {
        return "Needs ESP-IDF 5.5 and a TactilitySDK; not managed by the editor yet.";
    }
    void InstallToolchainComponent(const std::string&, BuildProgressCallback) override {}
    std::string GetEnginePath(const std::string&) const override { return ""; }
    std::string GetPlatformKey() const override { return "tactility"; }

   private:
    BuildState m_State = BuildState::Idle;
    BuildOptions m_Options;
    PlatformConfig m_Config;
    std::vector<std::string> m_Defines;
};

}  // namespace

extern "C" {

DEKI_BUILDER_API const DekiBuilderAbi* DekiBuilder_GetAbi(void)
{
    static const DekiBuilderAbi abi = DekiBuilder_ThisAbi((uint32_t)sizeof(PlatformConfig),
                                                           (uint32_t)sizeof(CMakeGen::PackageEntry));
    return &abi;
}

DEKI_BUILDER_API const char* DekiBuilder_GetName(void) { return "Deki Tactility Builder"; }
DEKI_BUILDER_API const char* DekiBuilder_GetVersion(void) { return "0.1.0"; }
DEKI_BUILDER_API int DekiBuilder_GetBuilderCount(void) { return 1; }

DEKI_BUILDER_API ITargetBuilder* DekiBuilder_CreateBuilder(int index)
{
    return index == 0 ? new TactilityBuilder() : nullptr;
}

DEKI_BUILDER_API void DekiBuilder_DestroyBuilder(ITargetBuilder* builder)
{
    delete builder;
}

}  // extern "C"
