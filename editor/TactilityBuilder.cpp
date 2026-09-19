// Builds a Deki game as an external Tactility app instead of as firmware.
//
// Ships in this package's editor/ folder, so it is compiled into the package's
// editor-side DLL and registered when a project has the package installed -
// the same way deki-esp32-integration ships the ESP-IDF backend - and never
// into an app build, which leaves editor/ out. The editor knows nothing about
// Tactility.
//
// What Tactility wants, and why this does not look like the ESP-IDF builder:
//   - the artifact is an app loaded at runtime, not a firmware image: a
//     relocatable ELF on a device, a shared object in the simulator
//   - the app is built by Tactility's own tool, tactility.py, against a
//     TactilitySDK, through a root CMakeLists that calls tactility_project_*
//   - there is no flashing: an app is installed over the network (the
//     simulator's development service, a device's over Wi-Fi), from an SD
//     card, or through the on-device App Hub
//
// A platform names what it runs on with one framework option:
//   idfTarget          a chip, "esp32s3" or "esp32p4" - a device app
//   tactilityPlatform  "posix-x86_64" - the Tactility simulator, on Linux
//
// Only the simulator builds end to end today. A device app needs the same
// sources compiled as an ESP-IDF component instead of a CMake target, which
// is not written yet, and Build() says so.

#include <deki-editor/build/BuilderPlugin.h>
#include <deki-editor/build/BuilderDefinition.h>
#include <deki-editor/build/CMakeGenUtils.h>
#include <deki-editor/build/FirmwareBuilderBase.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/EditorSettings.h>
#include <deki-editor/FeatureResolver.h>
#include <deki-editor/Paths.h>
#include <deki-editor/ShellCommand.h>
#include <deki/LogSystem.h>
#include <nlohmann/json.hpp>

#include "TactilityToolchain.h"
#include "TactilityToolchainDefinition.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// Named, not `using namespace`: the package build compiles several sources as
// one translation unit, and a file-scope using-directive would leak into the
// runtime sources batched after this one.
namespace DekiEditor
{
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

/// The simulator platform this machine can build and run, or empty when it
/// cannot run the simulator at all. Tactility names it by architecture.
std::string HostSimulatorPlatform()
{
#if defined(__linux__) && defined(__x86_64__)
    return "posix-x86_64";
#else
    return "";
#endif
}

std::string Join(const std::vector<std::string>& v, const char* sep)
{
    std::string out;
    for (const auto& s : v)
        out += (out.empty() ? "" : sep) + s;
    return out;
}

std::string ToCMakePath(std::string p)
{
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

/// The TactilitySDK platform a platform config builds for: its chip, or its
/// simulator platform.
std::string SdkPlatformOf(const PlatformConfig& config)
{
    const std::string chip = config.Option("idfTarget");
    return chip.empty() ? config.Option("tactilityPlatform") : chip;
}

bool IsAlnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Tactility's own manifest rules (app-module manifest.cpp and
// package_manifest_parsing.cpp at the pinned commit). An app that breaks one
// builds and packages fine and is then refused at install, so they are
// checked here instead. Its fields are copied into 32-byte buffers that keep
// a terminator, hence 31.

/// app.0.id, and the binary name (app.0.binary is the same string): letters,
/// digits and '.', 5 to 31 characters. It also reaches a C string literal and
/// a CMake target name, which both accept those.
bool IsValidAppId(const std::string& id)
{
    if (id.size() < 5 || id.size() > 31)
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return IsAlnum(c) || c == '.'; });
}

/// app.0.name: letters, digits, space and '-', 2 to 31 characters.
bool IsValidAppName(const std::string& name)
{
    if (name.size() < 2 || name.size() > 31)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return IsAlnum(c) || c == ' ' || c == '-'; });
}

/// version.name: letters, digits, '.', '-' and '_', at most 16 characters.
bool IsValidVersionName(const std::string& version)
{
    if (version.empty() || version.size() > 16)
        return false;
    return std::all_of(version.begin(), version.end(),
                       [](char c) { return IsAlnum(c) || c == '.' || c == '-' || c == '_'; });
}

/// The name the launcher shows: the platform's appName, else the project's own
/// name from deki.json.
std::string AppNameFor(const std::string& projectPath, const PlatformConfig& config)
{
    const std::string explicitName = config.Option("appName");
    if (!explicitName.empty())
        return explicitName;
    std::ifstream in(fs::path(projectPath) / "deki.json");
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    return j.is_object() ? j.value("name", std::string()) : std::string();
}

class TactilityBuilder : public FirmwareBuilderBase
{
   public:
    TactilityBuilder()
    {
        BuilderDefinition def;
        std::string error;
        if (ParseBuilderDefinition(kTactilityToolchainDefinition, def, error))
            m_ToolchainMgr.Initialize(def);
        else
            DEKI_LOG_ERROR("Tactility backend: its own toolchain definition does not parse (%s)",
                           error.c_str());
    }

    ~TactilityBuilder() override
    {
        m_CancelRequested = true;
        m_SdkCancel = true;
        if (m_BuildThread.joinable())
            m_BuildThread.join();
        if (m_SdkThread.joinable())
            m_SdkThread.join();
    }

    const char* GetName() const override { return "Tactility App"; }
    std::string GetFrameworkId() const override { return "tactility"; }
    const char* GetDescription() const override
    {
        return "Build as a loadable app for a device running Tactility, or for its simulator";
    }

    std::vector<std::string> ValidatePlatform(const PlatformConfig& config) const override
    {
        std::vector<std::string> problems;

        const std::string chip = config.Option("idfTarget");
        const std::string sim = config.Option("tactilityPlatform");
        if (!chip.empty() && !sim.empty())
        {
            problems.push_back("set idfTarget (a device) or tactilityPlatform (the simulator), not both");
        }
        else if (chip.empty() && sim.empty())
        {
            problems.push_back("name what the app runs on: idfTarget for a device chip, or tactilityPlatform "
                               "for the simulator (posix-x86_64)");
        }
        else if (!sim.empty())
        {
            const std::string host = HostSimulatorPlatform();
            if (!TactilitySdk::IsPosix(sim))
                problems.push_back("tactilityPlatform '" + sim + "' is not a simulator platform (posix-<arch>)");
            else if (host.empty())
                problems.push_back("the Tactility simulator runs on x86-64 Linux only; build this platform with "
                                   "the editor on Linux (WSL works)");
            else if (sim != host)
                problems.push_back("tactilityPlatform '" + sim + "' does not match this machine, which runs " + host);
        }
        else
        {
            const auto& ok = ExecutableFromPsramTargets();
            if (std::find(ok.begin(), ok.end(), chip) == ok.end())
                problems.push_back(
                    "idfTarget '" + chip +
                    "' cannot run a Deki app: an external app's code is relocated into RAM at "
                    "load, and only " + Join(ok, " and ") +
                    " can execute it from PSRAM. Tactility supports other chips, but an engine "
                    "does not fit in their internal RAM.");
        }

        const std::string appId = config.Option("appId");
        if (appId.empty())
            problems.push_back("appId is not set; Tactility keys an app's assets and user data "
                               "off it (e.g. 'com.example.mygame')");
        else if (!IsValidAppId(appId))
            problems.push_back("appId '" + appId + "' is not a Tactility app id: 5 to 31 letters, digits and '.'");

        const std::string appName = config.Option("appName");
        if (!appName.empty() && !IsValidAppName(appName))
            problems.push_back("appName '" + appName +
                               "' is not a Tactility app name: 2 to 31 letters, digits, spaces and '-'");

        const std::string version = config.Option("appVersion", "0.1.0");
        if (!IsValidVersionName(version))
            problems.push_back("appVersion '" + version +
                               "' is not a Tactility version name: up to 16 letters, digits, '.', '-' and '_'");

        return problems;
    }

    // ---- deploy: the simulator only, over its development service ----------
    //
    // A device app is installed the same way (tactility.py install <address>),
    // but a device build does not exist yet, so a device platform shows no
    // button rather than one that cannot work.

    bool SupportsDeploy() const override { return IsSimulator(); }
    const char* GetDeployLabel() const override { return "Install and run"; }

    std::vector<DeployTarget> EnumerateDeployTargets() const override
    {
        if (!IsSimulator())
            return {};
        return { DeployTarget{ "localhost", "Tactility simulator on this machine" } };
    }

    void Deploy(const std::string& projectPath, const std::string& deployTargetId,
                BuildOutputCallback out, BuildProgressCallback progress) override
    {
        const std::string host = deployTargetId.empty() ? std::string("localhost") : deployTargetId;
        RunOnBuildThread([this, projectPath, host, out, progress]() { DoDeploy(projectPath, host, out, progress); });
    }

    // ---- build files -------------------------------------------------------

    std::string GetBuildDirectory(const std::string& projectPath) const override
    {
        // Per platform: two Tactility platforms in one project must not share
        // a build tree (a device ELF and a simulator .so, or two chips).
        const std::string id = m_PlatformConfig.id.empty() ? std::string("tactility") : m_PlatformConfig.id;
        return (ProjectPaths::Build(projectPath) / id).string();
    }

    // The app's assets directory, which tactility.py packs into the .app: the
    // boot payload (dproject.bin, boot.scene) lands at its root, which the
    // app reads as F:/.
    std::string GetBootPayloadDirectory(const std::string& projectPath) const override
    {
        return GetBuildDirectory(projectPath) + "/assets";
    }

    bool GenerateBuildFiles(const std::string& projectPath, const PlatformConfig& config,
                            const std::vector<std::string>& packageDefines) override
    {
        const fs::path root = fs::path(GetBuildDirectory(projectPath));
        std::error_code ec;
        fs::create_directories(root / "main", ec);

        const std::string appId = config.Option("appId");
        const std::string appName = AppNameFor(projectPath, config);
        if (!IsValidAppName(appName))
        {
            // Tactility would refuse the app at install, after a full build.
            DEKI_LOG_ERROR("Tactility backend: the app name '%s' (the project's name; set appName on the "
                           "platform to choose another) is not a Tactility app name: 2 to 31 letters, "
                           "digits, spaces and '-'",
                           appName.c_str());
            return false;
        }
        const std::string platform = SdkPlatformOf(config);

        // tactility.py reads this to build and package the app. One app, built
        // at the project root (a v3 manifest with a single app.0 block).
        {
            std::ofstream f(root / "manifest.properties");
            f << "manifest.version=0.3\n"
              << "target.sdk=" << config.Option("targetSdk", TactilityPin::kVersion) << "\n"
              << "target.platforms=" << platform << "\n"
              << "id=" << appId << "\n"
              << "version.name=" << config.Option("appVersion", "0.1.0") << "\n"
              << "version.code=" << config.Option("appVersionCode", "1") << "\n"
              << "app.0.id=" << appId << "\n"
              << "app.0.name=" << appName << "\n"
              << "app.0.binary=" << appId << "\n";
        }

        // tactility.py's own template, byte for byte: it rewrites any root
        // CMakeLists whose first line is not its current version marker.
        {
            std::ofstream f(root / "CMakeLists.txt");
            f << "# tactility-cmakelists-version: 1\n"
              << "cmake_minimum_required(VERSION 3.20)\n"
              << "\n"
              << "if (NOT DEFINED ENV{TACTILITY_SDK_PATH})\n"
              << "    message(FATAL_ERROR \"TACTILITY_SDK_PATH environment variable is not set\")\n"
              << "endif()\n"
              << "\n"
              << "get_filename_component(TACTILITY_SDK_PATH \"$ENV{TACTILITY_SDK_PATH}\" ABSOLUTE)\n"
              << "include(\"${TACTILITY_SDK_PATH}/TactilitySDK.cmake\")\n"
              << "\n"
              << "tactility_project_pre(" << appId << ")\n"
              << "project(" << appId << ")\n"
              << "tactility_project_post(" << appId << ")\n";
        }

        if (TactilitySdk::IsPosix(platform))
            return GenerateSimulatorComponent(projectPath, root, config, packageDefines);

        // A device app: the ESP-IDF component that would compile the game is
        // not written yet, so this compiles nothing of it (see Build()).
        {
            std::ofstream f(root / "main" / "CMakeLists.txt");
            f << "# Generated by the Deki Tactility builder. Do not edit.\n"
              << "file(GLOB_RECURSE SOURCE_FILES Source/*.c*)\n"
              << "tactility_component_register(SRCS ${SOURCE_FILES} INCLUDE_DIRS \"Include\")\n";
        }
        return true;
    }

    // ---- build -------------------------------------------------------------

    void Build(const std::string& projectPath, BuildOutputCallback out, BuildProgressCallback progress) override
    {
        RunOnBuildThread([this, projectPath, out, progress]() { DoBuild(projectPath, out, progress); });
    }

    void Clean(const std::string& projectPath, BuildOutputCallback, BuildProgressCallback) override
    {
        std::error_code ec;
        fs::remove_all(GetBuildDirectory(projectPath), ec);
    }

    std::string GetPlatformKey() const override { return "tactility"; }

    // ---- toolchain ---------------------------------------------------------
    //
    // A device app: ESP-IDF 6.1 (shared with the ESP32 package) and the
    // TactilitySDK for its chip. The simulator: only its TactilitySDK, which
    // is the simulator's own build packaged; it needs no ESP-IDF.

    bool IsToolchainInstalled() const override
    {
        const std::string platform = Platform();
        if (platform.empty())
            return false;
        if (TactilitySdk::IsPosix(platform))
            return TactilitySdk::IsBuilt(platform);
        return IdfReady() && TactilitySdk::IsBuilt(platform);
    }

    std::string GetToolchainStatus() const override
    {
        const std::string platform = Platform();
        if (platform.empty())
            return "The platform names no Tactility target (idfTarget or tactilityPlatform)";
        const bool posix = TactilitySdk::IsPosix(platform);
        if (!posix && !IdfReady())
            return "ESP-IDF 6.1 is not installed; install the ESP-IDF SDK component";
        if (!TactilitySdk::IsBuilt(platform))
            return "The TactilitySDK " + std::string(TactilityPin::kVersion) + " for " + platform +
                   " has not been built; install the TactilitySDK component";
        return (posix ? std::string() : std::string("ESP-IDF 6.1 and ")) + "TactilitySDK " +
               TactilityPin::kVersion + " for " + platform + " " + (posix ? "is" : "are") + " installed";
    }

    std::vector<ToolchainComponent> GetToolchainComponents() const override
    {
        const std::string platform = Platform();
        std::vector<ToolchainComponent> components;
        if (!TactilitySdk::IsPosix(platform))
            components = m_ToolchainMgr.GetComponents();

        ToolchainComponent sdk;
        sdk.id = "tactility-sdk";
        sdk.displayName = "TactilitySDK (" + platform + ")";
        sdk.canInstall = true;
        sdk.canSetup = false;
        sdk.latestVersion = std::string(TactilityPin::kVersion) + " @ " +
                            std::string(TactilityPin::kCommit).substr(0, 12);
        if (m_SdkBusy)
            sdk.status = ToolchainComponentStatus::Installing;
        else if (TactilitySdk::IsBuilt(platform))
        {
            sdk.status = ToolchainComponentStatus::Installed;
            sdk.installedVersion = sdk.latestVersion;
        }
        else
            sdk.status = ToolchainComponentStatus::NotInstalled;
        sdk.tooltip = TactilitySdk::IsPosix(platform)
                          ? "The Tactility simulator, built from Tactility's sources at the pinned commit and "
                            "packaged as an SDK, plus TactilityTool's app build tool. Takes a while."
                          : "Built from Tactility's sources at the pinned commit (0.8 has no published SDK "
                            "yet), plus TactilityTool's app build tool and configuration. Needs the ESP-IDF "
                            "SDK first; takes a while.";
        components.push_back(sdk);
        return components;
    }

    void InstallToolchainComponent(const std::string& componentId, BuildProgressCallback cb) override
    {
        if (componentId != "tactility-sdk")
        {
            m_ToolchainMgr.InstallComponent(componentId, cb);
            return;
        }

        auto fail = [&cb](const std::string& error)
        {
            if (cb)
            {
                BuildProgress p;
                p.state = BuildState::Failed;
                p.error = error;
                cb(p);
            }
        };
        const std::string platform = Platform();
        if (platform.empty())
            return fail("The platform names no Tactility target (idfTarget or tactilityPlatform)");
        if (m_SdkBusy)
            return fail("The TactilitySDK is already being built");
        if (!TactilitySdk::IsPosix(platform) && !IdfReady())
            return fail("Install the ESP-IDF SDK component first: the TactilitySDK is built with it");

        if (m_SdkThread.joinable())
            m_SdkThread.join();
        m_SdkBusy = true;
        m_SdkCancel = false;
        const std::string idfPath = IdfPath();
        m_SdkThread = std::thread(
            [this, platform, idfPath, cb]()
            {
                auto line = [&cb](const std::string& text)
                {
                    if (cb)
                    {
                        BuildProgress p;
                        p.state = BuildState::Building;
                        p.statusText = text;
                        cb(p);
                    }
                };
                const std::string error = TactilitySdk::Build(platform, idfPath, line, &m_SdkCancel);
                if (cb)
                {
                    BuildProgress p;
                    p.state = error.empty() ? BuildState::Completed : BuildState::Failed;
                    p.statusText = error.empty() ? "TactilitySDK built" : "";
                    p.error = error;
                    p.progress = 1.0f;
                    cb(p);
                }
                m_SdkBusy = false;
            });
    }

    void SetupToolchainComponent(const std::string& componentId, BuildProgressCallback cb) override
    {
        m_ToolchainMgr.SetupComponent(componentId, cb);
    }

    bool IsToolchainBusy() const override { return m_ToolchainMgr.IsBusy() || m_SdkBusy; }

   private:
    /// The SDK platform of the bound platform. Before any platform is bound
    /// (a toolchain listing with none selected) it describes the default
    /// device chip.
    std::string Platform() const
    {
        if (!m_HasPlatformConfig)
            return "esp32s3";
        return SdkPlatformOf(m_PlatformConfig);
    }

    bool IsSimulator() const { return m_HasPlatformConfig && TactilitySdk::IsPosix(Platform()); }

    std::string IdfPath() const { return EditorPaths::GetToolchainsDir() + "/espressif/esp-idf"; }

    // Installed AND at the pinned version: the manager reports a different
    // version as UpdateAvailable, which is not good enough to build with.
    bool IdfReady() const
    {
        for (const auto& c : m_ToolchainMgr.GetComponents())
            if (c.id == "esp-idf")
                return c.status == ToolchainComponentStatus::Installed;
        return false;
    }

    // The simulator app: everything the game is made of - engine, packages,
    // game sources, reflection - compiled into the shared object tactility.py
    // builds. tactility_project_post() adds this directory; the SDK's
    // tactility_component_register() makes the .so and links the SDK's
    // headers-only target, since the simulator resolves Tactility's own
    // symbols when it loads the app.
    bool GenerateSimulatorComponent(const std::string& projectPath, const fs::path& root, const PlatformConfig& config,
                                    const std::vector<std::string>& packageDefines)
    {
        const std::string enginePath = ToCMakePath(EditorSettings::GetEnginePath());
        if (enginePath.empty())
        {
            DEKI_LOG_ERROR("Tactility backend: the editor cannot find its engine sources");
            return false;
        }

        const std::string colorFormat = config.colorFormat.empty() ? std::string("RGB565") : config.colorFormat;
        for (char c : colorFormat)
        {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok)
            {
                DEKI_LOG_ERROR("Tactility backend: colorFormat '%s' is not an identifier", colorFormat.c_str());
                return false;
            }
        }

        const auto allPackages = CMakeGen::ScanPackageManifests(projectPath);
        const auto activeIds = CMakeGen::ResolveActivePackages(allPackages, packageDefines, config.Capabilities());
        std::string transformWhy;
        const CMakeGen::TransformWidth transformWidth = CMakeGen::ResolveProjectTransformWidth(
            allPackages, CMakeGen::ReadProjectTags(projectPath), &transformWhy);
        const std::vector<std::string> transformDefines = CMakeGen::TransformDefines(transformWidth);

        const StripPlan strip = ComputeStripPlan(projectPath, config.id);
        std::string stripSourceRegex;
        for (const auto& rx : strip.SourceExcludeRegexes())
            stripSourceRegex += (stripSourceRegex.empty() ? "" : "|") + rx;

        // Everything the game's code is compiled with, and generated under.
        // The engine is told its target the way every backend tells it: the
        // screen, the colour format, no IRAM placement to ask for
        // (DEKI_FAST_ATTR empty, below).
        std::vector<std::string> defines = { "DEKI_TACTILITY_TARGET" };
        for (const auto& d : transformDefines)
            defines.push_back(d);
        for (const auto& d : config.defines)
            defines.push_back(d);
        for (const auto& d : packageDefines)
            defines.push_back(d);
        if (m_BuildOptions.enableLogging)
            defines.push_back("DEKI_LOG_ENABLED");
        if (m_BuildOptions.enableInternalLogging)
            defines.push_back("DEKI_LOG_INTERNAL_ENABLED");

        std::ostringstream f;
        f << "# Generated by the Deki Tactility builder. Do not edit.\n"
          << "# " << config.displayName << ": the game as a shared object the Tactility simulator loads.\n\n";

        f << "set(CMAKE_CXX_STANDARD 23)\n"
          << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
          // Everything ends up inside a shared object, the engine's static
          // library included.
          << "set(CMAKE_POSITION_INDEPENDENT_CODE ON)\n"
          // Every launch must start from nothing, as it does on a device,
          // where the loader relocates a fresh copy each time. GCC marks
          // template statics and inline variables STB_GNU_UNIQUE, and glibc
          // never unloads a library that has one: the simulator's dlclose()
          // then left the whole engine resident, and the next launch ran on
          // the last one's state (memory counters, singletons) and crashed.
          // For everything compiled into the app, so the engine as well.
          << "add_compile_options(-fno-gnu-unique)\n\n";

        // Absolute, and rewritten on every build: tactility.py runs the
        // configure itself, so nothing can be passed on its command line.
        f << "set(DEKI_PROJECT_ROOT \"" << ToCMakePath(projectPath) << "\")\n"
          << "set(DEKI_ENGINE_PATH \"" << enginePath << "\")\n\n";

        f << "set(DEKI_TRANSFORM_2D " << (transformWidth != CMakeGen::TransformWidth::None ? "ON" : "OFF")
          << " CACHE BOOL \"\" FORCE)\n"
          << "set(DEKI_TRANSFORM_3D " << (transformWidth == CMakeGen::TransformWidth::ThreeD ? "ON" : "OFF")
          << " CACHE BOOL \"\" FORCE)\n"
          << "message(STATUS \"Deki transform: " << CMakeGen::EscapeCMakeString(transformWhy) << "\")\n"
          << "add_subdirectory(\"${DEKI_ENGINE_PATH}\" \"${CMAKE_BINARY_DIR}/deki-engine\")\n\n";

        f << "target_compile_definitions(deki-engine-core PUBLIC\n"
          << "    DEKI_TACTILITY_TARGET\n"
          << "    \"DEKI_SCREEN_WIDTH=" << config.screenWidth << "\"\n"
          << "    \"DEKI_SCREEN_HEIGHT=" << config.screenHeight << "\"\n"
          << "    \"DEKI_DEFAULT_COLOR_FORMAT=Deki::ColorFormat::" << colorFormat << "\"\n"
          << "    \"DEKI_ENABLE_TRANSPARENCY=true\"\n"
          << "    \"DEKI_FAST_ATTR=\"";
        if (m_BuildOptions.enableLogging)
            f << "\n    DEKI_LOG_ENABLED";
        f << ")\n\n";

        // The game's own sources. PluginExports.cpp is the editor DLL's glue.
        CMakeGen::EmitProjectSourceCollection(f, "${DEKI_PROJECT_ROOT}/src");
        f << "list(FILTER PROJECT_SOURCES EXCLUDE REGEX \"PluginExports\\\\.cpp$\")\n\n";

        // Packages: the same collection as the desktop simulator's. Each
        // package's entry is its editor-plugin glue and stays out; the app's
        // entry point is an ordinary source of this package (TactilityApp.cpp).
        // editor/, tests/ and the checked-in generated/ stay out too; the
        // reflection for this configuration is generated below.
        f << "set(_ALL_PACKAGE_SOURCES \"\")\n"
          << "set(PACKAGE_INCLUDE_DIRS \"\")\n"
          << "set(PACKAGE_DEFINES \"\")\n"
          << "set(_ALL_SYSTEM_LIBS \"\")\n"
          << "set(_RC_PKG_DIRS \"\")\n"
          << "set(_RC_PKG_TAGS \"\")\n"
          << "set(_RC_PKG_PREFIXES \"\")\n"
          << "set(_RC_PKG_OUTDIRS \"\")\n"
          << "message(STATUS \"Deki stripping: " << CMakeGen::EscapeCMakeString(strip.summary) << "\")\n"
          << "set(_DEKI_STRIP_SRC_REGEX \"" << CMakeGen::EscapeCMakeString(stripSourceRegex) << "\")\n";

        f << "set(_DEKI_ACTIVE_PACKAGES";
        for (const auto& id : activeIds)
            f << " \"" << CMakeGen::EscapeCMakeString(id) << "\"";
        f << ")\n";

        f << "file(GLOB PACKAGE_CMAKE_FILES \"${DEKI_PROJECT_ROOT}/packages/*/package.cmake\")\n"
          << "foreach(PACKAGE_CMAKE ${PACKAGE_CMAKE_FILES})\n"
          << "    get_filename_component(PACKAGE_DIR \"${PACKAGE_CMAKE}\" DIRECTORY)\n"
          << "    get_filename_component(_MOD_NAME \"${PACKAGE_DIR}\" NAME)\n"
          // A package this platform cannot have (the desktop's SDL3, say)
          // is not compiled at all.
          << "    if(NOT _MOD_NAME IN_LIST _DEKI_ACTIVE_PACKAGES)\n"
          << "        continue()\n"
          << "    endif()\n"
          << "    unset(PACKAGE_ENTRY)\n"
          << "    unset(PACKAGE_UPPER)\n"
          << "    unset(PACKAGE_PREFIX)\n"
          << "    unset(PACKAGE_SYSTEM_LIBS)\n"
          << "    include(\"${PACKAGE_CMAKE}\")\n"
          << "    if(PACKAGE_UPPER)\n"
          << "        list(APPEND PACKAGE_DEFINES \"DEKI_${PACKAGE_UPPER}_EXPORTS\")\n"
          << "    endif()\n"
          << "    if(PACKAGE_SYSTEM_LIBS)\n"
          << "        list(APPEND _ALL_SYSTEM_LIBS ${PACKAGE_SYSTEM_LIBS})\n"
          << "    endif()\n"
          << "    set(_ENTRY_PATH \"\")\n"
          << "    if(PACKAGE_ENTRY)\n"
          << "        set(_ENTRY_PATH \"${PACKAGE_DIR}/${PACKAGE_ENTRY}\")\n"
          << "    endif()\n"
          << "    file(GLOB_RECURSE _MOD_SRCS \"${PACKAGE_DIR}/*.cpp\" \"${PACKAGE_DIR}/*.c\")\n"
          << "    foreach(SRC ${_MOD_SRCS})\n"
          << "        string(FIND \"${SRC}\" \"/editor/\" _IS_EDITOR)\n"
          << "        string(FIND \"${SRC}\" \"/tests/\" _IS_TESTS)\n"
          << "        string(FIND \"${SRC}\" \"/generated/\" _IS_GEN)\n"
          << "        set(_IS_STRIPPED FALSE)\n"
          << "        if(_DEKI_STRIP_SRC_REGEX AND \"${SRC}\" MATCHES \"${_DEKI_STRIP_SRC_REGEX}\")\n"
          << "            set(_IS_STRIPPED TRUE)\n"
          << "        endif()\n"
          << "        if(_IS_EDITOR EQUAL -1 AND _IS_TESTS EQUAL -1 AND _IS_GEN EQUAL -1 AND NOT _IS_STRIPPED AND NOT "
             "\"${SRC}\" STREQUAL \"${_ENTRY_PATH}\")\n"
          << "            list(APPEND _ALL_PACKAGE_SOURCES \"${SRC}\")\n"
          << "        endif()\n"
          << "    endforeach()\n"
          << "    list(APPEND PACKAGE_INCLUDE_DIRS \"${PACKAGE_DIR}\")\n"
          << "    if(PACKAGE_PREFIX)\n"
          << "        list(APPEND _RC_PKG_DIRS \"${PACKAGE_DIR}\")\n"
          << "        list(APPEND _RC_PKG_TAGS \"${_MOD_NAME}\")\n"
          << "        list(APPEND _RC_PKG_PREFIXES \"${PACKAGE_PREFIX}\")\n"
          << "        list(APPEND _RC_PKG_OUTDIRS \"${CMAKE_BINARY_DIR}/refl/${_MOD_NAME}/generated\")\n"
          << "        list(APPEND PACKAGE_INCLUDE_DIRS \"${CMAKE_BINARY_DIR}/refl/${_MOD_NAME}\")\n"
          << "    endif()\n"
          << "endforeach()\n\n";

        // Reflection for this configuration: it depends on the defines it is
        // generated under, so nothing the editor produced can be reused.
        f << "find_program(DEKI_GXX16 NAMES g++-16 g++)\n"
          << "if(NOT DEKI_GXX16)\n"
          << "    message(FATAL_ERROR \"Deki reflection codegen needs GCC 16.1+\")\n"
          << "endif()\n"
          << "if(_RC_PKG_DIRS)\n"
          << "    include(\"${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake\")\n"
          << "    get_target_property(_TT_SDK_INCS TactilitySDK INTERFACE_INCLUDE_DIRECTORIES)\n"
          << "    deki_reflection_codegen(\n"
          << "        UNIT_PREFIX \"tactility\"\n"
          << "        PACKAGE_DIRS ${_RC_PKG_DIRS}\n"
          << "        PACKAGE_TAGS ${_RC_PKG_TAGS}\n"
          << "        PACKAGE_PREFIXES ${_RC_PKG_PREFIXES}\n"
          << "        PACKAGE_OUTDIRS ${_RC_PKG_OUTDIRS}\n"
          << "        GENERATOR_SRC \"${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp\" GXX \"${DEKI_GXX16}\"\n"
          << "        INCLUDE_DIRS ${PACKAGE_INCLUDE_DIRS} \"${DEKI_ENGINE_PATH}/include\" "
             "\"${DEKI_ENGINE_PATH}/third_party\" \"${DEKI_PROJECT_ROOT}/packages\" \"${DEKI_PROJECT_ROOT}/src\" "
             "${_TT_SDK_INCS}\n";
        {
            const auto codegenExcludes = strip.CodegenExcludeRegexes();
            if (!codegenExcludes.empty())
            {
                f << "        EXCLUDE_REGEX";
                for (const auto& rx : codegenExcludes)
                    f << " \"" << CMakeGen::EscapeCMakeString(rx) << "\"";
                f << "\n";
            }
        }
        f << "        DEFINES";
        for (const auto& d : defines)
            f << " \"" << CMakeGen::EscapeCMakeString(d) << "\"";
        f << " \"DEKI_FAST_ATTR=\" \"DEKI_SCREEN_WIDTH=" << config.screenWidth << "\" \"DEKI_SCREEN_HEIGHT="
          << config.screenHeight << "\")\n"
          << "    foreach(_OUTDIR ${_RC_PKG_OUTDIRS})\n"
          << "        file(GLOB _RC_GEN_SRCS \"${_OUTDIR}/*.gen.cpp\")\n"
          << "        list(APPEND _ALL_PACKAGE_SOURCES ${_RC_GEN_SRCS})\n"
          << "    endforeach()\n"
          << "endif()\n\n";

        // deki_register_project_packages(): the static package registration a
        // firmware build uses. The engine links its own empty system-init
        // stub here, as in the desktop simulator, so the init calls go inline.
        const std::string initName = fs::path(CMakeGen::GeneratePackageInitFile(
                                                  root / "main", allPackages, activeIds,
                                                  fs::path(GetSourceDirectory(projectPath)),
                                                  /*engineDefinesSystemInit=*/true))
                                         .filename()
                                         .string();

        f << "tactility_component_register(SRCS\n"
          << "    \"${DEKI_ENGINE_PATH}/entry/Main.cpp\"\n"
          << "    \"${CMAKE_CURRENT_LIST_DIR}/" << initName << "\"\n"
          << "    ${PROJECT_SOURCES}\n"
          << "    ${_ALL_PACKAGE_SOURCES})\n\n";

        f << "foreach(_RC_TAG ${_RC_PKG_TAGS})\n"
          << "    string(MAKE_C_IDENTIFIER \"${_RC_TAG}\" _RC_TAGID)\n"
          << "    if(DEKI_RC_TARGETS_${_RC_TAGID})\n"
          << "        add_dependencies(${PROJECT_NAME} ${DEKI_RC_TARGETS_${_RC_TAGID}})\n"
          << "    endif()\n"
          << "endforeach()\n\n";

        // Package directories before the game's src/, as in every other
        // backend: a package's generated reflection includes its headers
        // unqualified, and a game header of the same name must not win.
        f << "target_include_directories(${PROJECT_NAME} PRIVATE\n"
          << "    \"${DEKI_ENGINE_PATH}/include\"\n"
          << "    \"${DEKI_ENGINE_PATH}/third_party\"\n"
          << "    \"${DEKI_PROJECT_ROOT}/packages\"\n"
          << "    ${PACKAGE_INCLUDE_DIRS}\n"
          << "    \"${DEKI_PROJECT_ROOT}/src\")\n\n";

        // The app id, which only the entry point reads (TactilityApp.cpp), so
        // it stays out of the reflection's defines above.
        f << "target_compile_definitions(${PROJECT_NAME} PRIVATE\n"
          << "    \"" << CMakeGen::EscapeCMakeString("DEKI_TACTILITY_APP_ID=\"" + config.Option("appId") + "\"")
          << "\"\n";
        for (const auto& d : defines)
            f << "    \"" << CMakeGen::EscapeCMakeString(d) << "\"\n";
        f << "    ${PACKAGE_DEFINES})\n\n";

        if (!config.cxxFlags.empty())
        {
            f << "target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:";
            for (const auto& flag : config.cxxFlags)
                if (flag.rfind("-std=", 0) != 0)
                    f << " " << flag;
            f << ">)\n";
        }

        // The app is loaded into the simulator's own process, which exports
        // every one of its symbols so apps can reach Tactility. Some of the
        // engine's third-party code (a JSON parser, an image decoder) may
        // exist there too, under the same names; -Bsymbolic keeps the app's
        // calls on its own copies rather than whatever the host has.
        f << "target_link_options(${PROJECT_NAME} PRIVATE \"-Wl,-Bsymbolic\")\n"
          << "target_link_libraries(${PROJECT_NAME} PRIVATE deki-engine-core ${_ALL_SYSTEM_LIBS})\n";

        return CMakeGen::WriteIfChanged(root / "main" / "CMakeLists.txt", f.str());
    }

    void Report(const BuildOutputCallback& out, const std::string& line, bool isError)
    {
        if (out)
            out(line, isError);
    }

    void Fail(const BuildOutputCallback& out, const BuildProgressCallback& progress, const std::string& error)
    {
        Report(out, error, true);
        SetError(error);
        if (progress)
            progress(GetProgress());
    }

    // tactility.py, the pinned copy, run in the app directory with the SDK
    // it was built against. The simulator's compiler is the one its SDK was
    // built with (TactilitySdk::Build finds it the same way).
    std::string ToolCommand(const fs::path& root, const std::string& args) const
    {
        const fs::path script = root / "tactility-tool.sh";
        std::ofstream f(script, std::ios::binary | std::ios::trunc);
        f << "set -e\n"
          << "unset ESP_IDF_VERSION IDF_PATH\n"
          << "export CXX=$(command -v g++-16 || command -v g++)\n"
          << "export CC=$(command -v gcc-16 || command -v gcc)\n"
          << "export TACTILITY_SDK_PATH=\"" << TactilitySdk::SdkBase() << "\"\n"
          << "cd \"" << root.string() << "\"\n"
          << "exec python3 \"" << TactilitySdk::ToolDir() << "/tactility.py\" " << args << "\n";
        return "sh \"" + script.string() + "\"";
    }

    void DoBuild(const std::string& projectPath, BuildOutputCallback out, BuildProgressCallback progress)
    {
        SetProgress(BuildState::Building, "Building the Tactility app", 0.0f);
        if (progress)
            progress(GetProgress());

        const std::string platform = SdkPlatformOf(m_PlatformConfig);
        if (!TactilitySdk::IsPosix(platform))
        {
            // A failure, not a success with a caveat: the editor turns a
            // completed build into "Firmware build succeeded".
            return Fail(out, progress,
                        "Tactility device apps cannot be built yet: compiling the game as an ESP-IDF "
                        "component is not written. The simulator platform builds.");
        }
#ifdef _WIN32
        return Fail(out, progress, "The Tactility simulator app builds on Linux only");
#else
        const fs::path root = GetBuildDirectory(projectPath);
        const fs::path assets = root / "assets";
        std::error_code ec;

        // The boot payload was exported into assets/ before the build files
        // were generated. The asset export is the project's own, from
        // --export; it ships beside it, where the app reads F:/storage/.
        if (!fs::is_regular_file(assets / "dproject.bin", ec))
            return Fail(out, progress, "No boot payload in " + assets.string() + "; build through the editor");
        const fs::path storage = fs::path(projectPath) / "storage";
        if (!fs::is_regular_file(storage / "asset_table.bin", ec))
            return Fail(out, progress,
                        "The project has no asset export (" + storage.string() + "); run --export first");
        fs::remove_all(assets / "storage", ec);
        fs::copy(storage, assets / "storage", fs::copy_options::recursive, ec);
        if (ec)
            return Fail(out, progress, "Could not copy the asset export into the app: " + ec.message());
        Report(out, "Asset export copied into " + (assets / "storage").string(), false);

        const int code = RunShellCommand(ToolCommand(root, "build -a " + platform + " --local-sdk"), root.string(),
                                         [&out](const std::string& line) { if (out) out(line, false); },
                                         &m_CancelRequested);
        if (m_CancelRequested)
            return Fail(out, progress, "Cancelled");
        if (code != 0)
            return Fail(out, progress, "tactility.py build failed (exit code " + std::to_string(code) + ")");

        const fs::path app = root / "build" / (m_PlatformConfig.Option("appId") + ".app");
        if (!fs::is_regular_file(app, ec))
            return Fail(out, progress, "tactility.py finished but " + app.string() + " is missing");

        Report(out, "Built " + app.string(), false);
        SetProgress(BuildState::Completed, "Built " + app.filename().string(), 1.0f);
        if (progress)
            progress(GetProgress());
#endif
    }

    void DoDeploy(const std::string& projectPath, const std::string& host, BuildOutputCallback out,
                  BuildProgressCallback progress)
    {
        SetProgress(BuildState::Deploying, "Installing on " + host, 0.0f);
        if (progress)
            progress(GetProgress());

        const fs::path root = GetBuildDirectory(projectPath);
        std::error_code ec;
        if (!fs::is_regular_file(root / "build" / (m_PlatformConfig.Option("appId") + ".app"), ec))
            return Fail(out, progress, "Nothing to install: build the app first");

        // The simulator must be running, with its development service on.
        //
        // tactility.py's exit code does not say whether this worked: a refused
        // install prints its failure mark and still exits 0, and the device
        // then answers "run" with 200 for an app it does not have. The mark
        // (print_status_error's "❌") is the tool's only failure signal here.
        for (const char* action : { "install", "run" })
        {
            bool toolFailed = false;
            const int code = RunShellCommand(ToolCommand(root, std::string(action) + " --host " + host + " --local-sdk"),
                                             root.string(),
                                             [&out, &toolFailed](const std::string& line)
                                             {
                                                 if (line.find("\xE2\x9D\x8C") != std::string::npos)
                                                     toolFailed = true;
                                                 if (out)
                                                     out(line, false);
                                             },
                                             &m_CancelRequested);
            if (code != 0 || toolFailed)
                return Fail(out, progress,
                            std::string("tactility.py ") + action + " on " + host +
                                " failed; its output above, and the device's log, say why. A device that "
                                "cannot be reached is not running, or its development service is off.");
        }

        SetProgress(BuildState::Completed, "Running on " + host, 1.0f);
        if (progress)
            progress(GetProgress());
    }

    std::thread m_SdkThread;
    std::atomic<bool> m_SdkBusy{ false };
    std::atomic<bool> m_SdkCancel{ false };
};

}  // namespace
}  // namespace DekiEditor

extern "C" {

DEKI_BUILDER_API const DekiBuilderAbi* DekiBuilder_GetAbi(void)
{
    static const DekiBuilderAbi abi = DekiBuilder_ThisAbi((uint32_t)sizeof(DekiEditor::PlatformConfig),
                                                           (uint32_t)sizeof(DekiEditor::CMakeGen::PackageEntry));
    return &abi;
}

DEKI_BUILDER_API const char* DekiBuilder_GetName(void) { return "Deki Tactility Builder"; }
DEKI_BUILDER_API const char* DekiBuilder_GetVersion(void) { return "0.2.0"; }
DEKI_BUILDER_API int DekiBuilder_GetBuilderCount(void) { return 1; }

DEKI_BUILDER_API DekiEditor::ITargetBuilder* DekiBuilder_CreateBuilder(int index)
{
    return index == 0 ? new DekiEditor::TactilityBuilder() : nullptr;
}

DEKI_BUILDER_API void DekiBuilder_DestroyBuilder(DekiEditor::ITargetBuilder* builder)
{
    delete builder;  // in THIS module: its vtable and operator delete live here
}

}  // extern "C"
