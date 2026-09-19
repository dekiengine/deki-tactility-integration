#include "TactilityToolchain.h"

#include <deki-editor/EditorPaths.h>
#include <deki-editor/ShellCommand.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{
// Tactility's generic board for each chip an app can run on (see
// ValidatePlatform: only these two can execute code from PSRAM).
std::string DeviceFor(const std::string& idfTarget)
{
    if (idfTarget == "esp32s3" || idfTarget == "esp32p4")
        return "generic-" + idfTarget;
    return {};
}

std::string Native(std::string p)
{
#ifdef _WIN32
    std::replace(p.begin(), p.end(), '/', '\\');
#endif
    return p;
}

// The simulator's one patch (see TactilityToolchain.h). Recorded in the SDK
// it builds, so an SDK from before the patch counts as not built.
constexpr const char* kSimulatorPatchMarker = "deki-simulator-patches.txt";
constexpr const char* kSimulatorPatchId = "display-resolution-from-env 1";

constexpr const char* kSimulatorSizeLine = "static const SdlDisplayConfig sdl_display_config = { 640, 480 };";
constexpr const char* kSimulatorSizePatch =
    "// Patched by Deki (deki-tactility-integration): the display size comes from\n"
    "// TACTILITY_SIMULATOR_RESOLUTION (\"320x240\"), so the simulator can stand in for\n"
    "// a device of any size. Unset, it is Tactility's own 640x480.\n"
    "#include <cstdio>\n"
    "#include <cstdlib>\n"
    "static SdlDisplayConfig deki_sdl_display_config() {\n"
    "    SdlDisplayConfig config = { 640, 480 };\n"
    "    const char* value = std::getenv(\"TACTILITY_SIMULATOR_RESOLUTION\");\n"
    "    if (value == nullptr) {\n"
    "        return config;\n"
    "    }\n"
    "    unsigned width = 0, height = 0;\n"
    "    if (std::sscanf(value, \"%ux%u\", &width, &height) != 2 || width < 64 || height < 64 || width > 4096 || "
    "height > 4096) {\n"
    "        std::fprintf(stderr, \"TACTILITY_SIMULATOR_RESOLUTION '%s' is not <width>x<height>; using 640x480\\n\", "
    "value);\n"
    "        return config;\n"
    "    }\n"
    "    config.horizontal_resolution = static_cast<uint16_t>(width);\n"
    "    config.vertical_resolution = static_cast<uint16_t>(height);\n"
    "    return config;\n"
    "}\n"
    "static const SdlDisplayConfig sdl_display_config = deki_sdl_display_config();";

std::string PatchSimulator(const fs::path& src)
{
    const fs::path file = src / "Devices" / "simulator" / "Source" / "module.cpp";
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return "the simulator source " + file.string() + " is missing";
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (text.find("deki_sdl_display_config") != std::string::npos)
        return {};  // already patched: the checkout keeps local changes
    const size_t at = text.find(kSimulatorSizeLine);
    if (at == std::string::npos)
        return "the simulator's display setup is not what the Deki patch expects in " + file.string() +
               "; the pinned Tactility changed, and the patch needs updating with it";
    text.replace(at, std::string(kSimulatorSizeLine).size(), kSimulatorSizePatch);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    return out ? std::string() : "could not write " + file.string();
}

std::string ReadFirstLine(const fs::path& file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}
}  // namespace

std::string TactilitySdk::Root()
{
    return EditorPaths::GetToolchainsDir() + "/tactility/" + std::string(TactilityPin::kCommit).substr(0, 12);
}

std::string TactilitySdk::SourceDir()
{
    return Root() + "/src";
}

std::string TactilitySdk::ToolDir()
{
    return Root() + "/tool";
}

std::string TactilitySdk::SdkBase()
{
    return Root() + "/sdk";
}

std::string TactilitySdk::SdkDir(const std::string& platform)
{
    return SdkBase() + "/" + TactilityPin::kVersion + "-" + platform + "/TactilitySDK";
}

bool TactilitySdk::IsPosix(const std::string& platform)
{
    return platform.rfind("posix-", 0) == 0;
}

bool TactilitySdk::IsBuilt(const std::string& platform)
{
    if (platform.empty())
        return false;
    std::error_code ec;
    const fs::path sdk = SdkDir(platform);
    const fs::path tool = ToolDir();
    if (!fs::is_regular_file(tool / "tactility.py", ec))
        return false;
    // release-sdk-posix.py writes the top-level CMakeLists.txt last;
    // release-sdk-esp32.py writes idf-version.txt last. A device app also
    // needs the app configuration tactility.py would otherwise download.
    if (IsPosix(platform))
        return fs::is_regular_file(sdk / "CMakeLists.txt", ec) &&
               ReadFirstLine(sdk / kSimulatorPatchMarker) == kSimulatorPatchId;
    return fs::is_regular_file(sdk / "idf-version.txt", ec) &&
           fs::is_regular_file(tool / "CDN" / ("sdkconfig.app." + platform), ec);
}

std::string TactilitySdk::Build(const std::string& platform, const std::string& idfPath,
                                const std::function<void(const std::string&)>& onLine,
                                const std::atomic<bool>* cancel)
{
    const bool posix = IsPosix(platform);
    const std::string device = posix ? std::string() : DeviceFor(platform);
    if (!posix && device.empty())
        return "no Tactility SDK for '" + platform + "'; an app runs only on esp32s3, esp32p4 or the simulator";
#ifdef _WIN32
    if (posix)
        return "the Tactility simulator SDK builds on Linux only";
#endif

    std::error_code ec;
    const fs::path src = SourceDir();
    fs::create_directories(fs::path(Root()), ec);

    auto run = [&](const std::string& what, const std::string& line, const std::string& dir) -> std::string
    {
        onLine("> " + what);
        const int code = RunShellCommand(line, dir, onLine, cancel);
        if (cancel != nullptr && cancel->load())
            return "cancelled";
        if (code != 0)
            return what + " failed (exit code " + std::to_string(code) + ")";
        return {};
    };

    // A repository at a pinned commit. A partial clone: history without file
    // contents, which is all a checkout of one commit needs. Then the checkout
    // is verified - a pin nobody checks is not one.
    auto checkout = [&](const std::string& name, const std::string& url, const std::string& commit,
                        const fs::path& dir, bool submodules) -> std::string
    {
        if (!fs::exists(dir / ".git", ec))
        {
            if (auto err = run("Cloning " + name,
                               "git clone --filter=blob:none --no-checkout \"" + url + "\" \"" +
                                   Native(dir.string()) + "\"",
                               Native(Root()));
                !err.empty())
                return err;
        }
        const std::string git = "git -C \"" + Native(dir.string()) + "\" ";
        if (auto err = run("Checking out " + name + " " + commit.substr(0, 12), git + "checkout --detach " + commit,
                           Native(Root()));
            !err.empty())
            return err;
        if (submodules)
        {
            if (auto err = run("Fetching " + name + " submodules",
                               git + "submodule update --init --recursive --filter=blob:none", Native(Root()));
                !err.empty())
                return err;
        }
        std::string head;
        RunShellCommand(git + "rev-parse HEAD", Native(Root()), [&head](const std::string& l) { head = l; },
                        cancel);
        if (head != commit)
            return "the " + name + " checkout is at '" + head + "', not the pinned " + commit;
        return {};
    };

    // 1. Tactility's sources, with their submodules, and TactilityTool.
    if (auto err = checkout("Tactility", TactilityPin::kRepository, TactilityPin::kCommit, src, true); !err.empty())
        return err;
    if (auto err = checkout("TactilityTool", TactilityPin::kToolRepository, TactilityPin::kToolCommit,
                            fs::path(ToolDir()), false);
        !err.empty())
        return err;
    if (!posix && !fs::is_regular_file(fs::path(ToolDir()) / "CDN" / ("sdkconfig.app." + platform), ec))
        return "TactilityTool at the pinned commit has no CDN/sdkconfig.app." + platform;

    const fs::path sdk = SdkDir(platform);
    fs::remove_all(sdk, ec);
    fs::create_directories(sdk.parent_path(), ec);

    // 2. Build and package, with Tactility's own scripts - the steps its CI
    //    runs to publish an SDK.
    fs::path script;
    std::string command;
    fs::path targetMarker;
    if (posix)
    {
        if (auto err = PatchSimulator(src); !err.empty())
            return err;
        onLine("Simulator patched: display size from TACTILITY_SIMULATOR_RESOLUTION");

        // The simulator: Tactility's root CMakeLists builds it whenever
        // ESP-IDF is not in the environment, into buildsim/, which is where
        // release-sdk-posix.py packages the libraries from. Testing and
        // mbedtls's sample programs are off: they are most of the build time
        // and no part of the SDK. The compiler is found the way the engine's
        // own reflection codegen finds it, so the simulator, its SDK and an
        // app built against it agree.
        script = fs::path(Root()) / ("build-sdk-" + platform + ".sh");
        std::ofstream f(script, std::ios::binary | std::ios::trunc);
        f << "set -e\n"
          << "unset ESP_IDF_VERSION IDF_PATH\n"
          << "CXX=$(command -v g++-16 || command -v g++)\n"
          << "CC=$(command -v gcc-16 || command -v gcc)\n"
          << "cd \"" << src.string() << "\"\n"
          << "cmake -S . -B buildsim -G Ninja -DCMAKE_C_COMPILER=\"$CC\" -DCMAKE_CXX_COMPILER=\"$CXX\" "
             "-DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF\n"
          << "cmake --build buildsim\n"
          << "python3 Buildscripts/release-sdk-posix.py \"" << sdk.string() << "\"\n";
        command = "sh \"" + script.string() + "\"";
    }
    else
    {
        // A generic board of the chip. A build tree left by the other chip is
        // discarded first: idf.py refuses to switch targets under an existing
        // one.
        targetMarker = src / "build" / ".deki-target";
        if (fs::exists(src / "build", ec) && ReadFirstLine(targetMarker) != platform)
        {
            onLine("Discarding a build tree made for another chip");
            fs::remove_all(src / "build", ec);
            fs::remove(src / "sdkconfig", ec);
        }
#ifdef _WIN32
        script = fs::path(Root()) / ("build-sdk-" + platform + ".bat");
        std::ofstream f(script, std::ios::binary | std::ios::trunc);
        // MSYSTEM cleared: ESP-IDF's scripts refuse to run when it is set, and
        // exit 0 while doing so. export.bat by full path: a bare name after a
        // cd fails under NoDefaultCurrentDirectoryInExePath.
        f << "@echo off\r\n"
          << "set \"MSYSTEM=\"\r\n"
          << "cd /d \"" << Native(idfPath) << "\" || exit /b 1\r\n"
          << "call \"" << Native(idfPath) << "\\export.bat\" || exit /b 1\r\n"
          << "cd /d \"" << Native(src.string()) << "\" || exit /b 1\r\n"
          << "python device.py " << device << " || exit /b 1\r\n"
          << "idf.py build || exit /b 1\r\n"
          << "set \"ESP_IDF_VERSION=" << TactilityPin::kIdfMajorMinor << "\"\r\n"
          << "python Buildscripts\\release-sdk-esp32.py \"" << Native(sdk.string()) << "\" || exit /b 1\r\n";
        command = "call \"" + Native(script.string()) + "\"";
#else
        script = fs::path(Root()) / ("build-sdk-" + platform + ".sh");
        std::ofstream f(script, std::ios::binary | std::ios::trunc);
        f << "set -e\n"
          << ". \"" << idfPath << "/export.sh\"\n"
          << "cd \"" << src.string() << "\"\n"
          << "python device.py " << device << "\n"
          << "idf.py build\n"
          << "export ESP_IDF_VERSION=" << TactilityPin::kIdfMajorMinor << "\n"
          << "python Buildscripts/release-sdk-esp32.py \"" << sdk.string() << "\"\n";
        command = "sh \"" + script.string() + "\"";
#endif
    }

    if (auto err = run("Building TactilitySDK " + std::string(TactilityPin::kVersion) + " for " + platform,
                       command, Native(src.string()));
        !err.empty())
        return err;

    if (!targetMarker.empty())
    {
        std::ofstream marker(targetMarker, std::ios::trunc);
        marker << platform << "\n";
    }
    if (posix)
    {
        std::ofstream marker(sdk / kSimulatorPatchMarker, std::ios::trunc);
        marker << kSimulatorPatchId << "\n";
    }

    if (!IsBuilt(platform))
        return "the release script finished but " + Native(sdk.string()) + " is incomplete";
    onLine("TactilitySDK " + std::string(TactilityPin::kVersion) + " for " + platform + " ready at " +
           Native(sdk.string()));
    return {};
}

std::string TactilitySdk::EnsureSimulator(int width, int height,
                                          const std::function<void(const std::string&)>& onLine,
                                          const std::atomic<bool>* cancel)
{
#ifdef _WIN32
    (void)width;
    (void)height;
    (void)onLine;
    (void)cancel;
    return "the Tactility simulator runs on Linux only";
#else
    // tactility.py talks to the development service on port 6666; answering
    // there is what "running" means. python3 is already required by the tool.
    const std::string probe =
        "python3 -c \"import urllib.request; urllib.request.urlopen('http://localhost:6666/info', timeout=1)\"";
    auto quiet = [](const std::string&) {};
    const std::string resolution = std::to_string(width) + "x" + std::to_string(height);

    // The simulator this backend started last, as "<pid> <width>x<height>".
    // One it started at another size is restarted at this one; a simulator
    // started any other way is used as it is, since it is not ours to stop.
    const fs::path started = fs::path(Root()) / "simulator.started";
    if (RunShellCommand(probe + " 2>/dev/null", Root(), quiet, cancel) == 0)
    {
        std::ifstream in(started);
        long pid = 0;
        std::string size;
        in >> pid >> size;
        const bool ours = pid > 0 && RunShellCommand("kill -0 " + std::to_string(pid) + " 2>/dev/null", Root(),
                                                     quiet, cancel) == 0;
        if (!ours || size == resolution)
        {
            onLine(ours ? "Using the Tactility simulator already running at " + resolution
                        : "Using the Tactility simulator already running on this machine");
            return {};
        }
        onLine("Restarting the Tactility simulator: it runs at " + size + ", the platform is " + resolution);
        // It ignores SIGTERM.
        RunShellCommand("kill -9 " + std::to_string(pid), Root(), quiet, cancel);
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            if (RunShellCommand("kill -0 " + std::to_string(pid) + " 2>/dev/null", Root(), quiet, cancel) != 0)
                break;
            RunShellCommand("sleep 0.25", Root(), quiet, cancel);
        }
    }

    const fs::path src = SourceDir();
    const fs::path binary = src / "buildsim" / "Tactility" / "Tactility";
    const fs::path data = src / "Data";
    std::error_code ec;
    if (!fs::is_regular_file(binary, ec))
        return "the simulator is not built (" + binary.string() + "); install the TactilitySDK component";

    // Its development service, the thing an app is installed through, is off
    // until a setting turns it on at boot: the one its Development app writes.
    const fs::path settings = data / "data" / "tactility" / "user" / "app" / "tactility.development";
    fs::create_directories(settings, ec);
    {
        std::ofstream f(settings / "development.properties", std::ios::trunc);
        f << "enableOnBoot=true\n";
    }

    // Detached from the editor (its own session, output to a log beside the
    // SDK) so it outlives this deploy, like any simulator the user starts.
    // $! is the simulator itself: setsid, not being a group leader here,
    // execs rather than forks, and so does nohup.
    const fs::path log = fs::path(Root()) / "simulator.log";
    onLine("Starting the Tactility simulator at " + resolution + " (log: " + log.string() + ")");
    const std::string start = "cd \"" + data.string() + "\" && TACTILITY_SIMULATOR_RESOLUTION=" + resolution +
                              " setsid nohup \"" + binary.string() + "\" > \"" + log.string() +
                              "\" 2>&1 < /dev/null & echo \"$! " + resolution + "\" > \"" + started.string() + "\"";
    if (RunShellCommand(start, data.string(), onLine, cancel) != 0)
        return "could not start the simulator";

    for (int attempt = 0; attempt < 60; ++attempt)
    {
        if (cancel != nullptr && cancel->load())
            return "cancelled";
        if (RunShellCommand(probe + " 2>/dev/null", Root(), quiet, cancel) == 0)
            return {};
        RunShellCommand("sleep 0.5", Root(), quiet, cancel);
    }
    return "the simulator did not start answering on port 6666; see " + log.string();
#endif
}

}  // namespace DekiEditor
