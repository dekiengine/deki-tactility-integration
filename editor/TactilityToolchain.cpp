#include "TactilityToolchain.h"

#include <deki-editor/EditorPaths.h>
#include <deki-editor/ShellCommand.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

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

std::string TactilitySdk::SdkDir(const std::string& idfTarget)
{
    return SdkBase() + "/" + TactilityPin::kVersion + "-" + idfTarget + "/TactilitySDK";
}

bool TactilitySdk::IsBuilt(const std::string& idfTarget)
{
    std::error_code ec;
    return !idfTarget.empty() && fs::is_regular_file(fs::path(SdkDir(idfTarget)) / "idf-version.txt", ec) &&
           fs::is_regular_file(fs::path(ToolDir()) / "CDN" / ("sdkconfig.app." + idfTarget), ec) &&
           fs::is_regular_file(fs::path(ToolDir()) / "tactility.py", ec);
}

std::string TactilitySdk::Build(const std::string& idfTarget, const std::string& idfPath,
                                const std::function<void(const std::string&)>& onLine,
                                const std::atomic<bool>* cancel)
{
    const std::string device = DeviceFor(idfTarget);
    if (device.empty())
        return "no Tactility SDK for '" + idfTarget + "'; an app runs only on esp32s3 or esp32p4";

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
    if (!fs::is_regular_file(fs::path(ToolDir()) / "CDN" / ("sdkconfig.app." + idfTarget), ec))
        return "TactilityTool at the pinned commit has no CDN/sdkconfig.app." + idfTarget;

    // 2. Build for the chip's generic board, then package the SDK with
    //    Tactility's own release script - the steps its CI runs to publish one.
    //    A build tree left by the other chip is discarded first: idf.py refuses
    //    to switch targets under an existing one.
    const fs::path targetMarker = src / "build" / ".deki-target";
    if (fs::exists(src / "build", ec) && ReadFirstLine(targetMarker) != idfTarget)
    {
        onLine("Discarding a build tree made for another chip");
        fs::remove_all(src / "build", ec);
        fs::remove(src / "sdkconfig", ec);
    }

    const fs::path sdk = SdkDir(idfTarget);
    fs::remove_all(sdk, ec);
    fs::create_directories(sdk.parent_path(), ec);

#ifdef _WIN32
    const fs::path script = fs::path(Root()) / ("build-sdk-" + idfTarget + ".bat");
    {
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
    }
    const std::string command = "call \"" + Native(script.string()) + "\"";
#else
    const fs::path script = fs::path(Root()) / ("build-sdk-" + idfTarget + ".sh");
    {
        std::ofstream f(script, std::ios::binary | std::ios::trunc);
        f << "set -e\n"
          << ". \"" << idfPath << "/export.sh\"\n"
          << "cd \"" << src.string() << "\"\n"
          << "python device.py " << device << "\n"
          << "idf.py build\n"
          << "export ESP_IDF_VERSION=" << TactilityPin::kIdfMajorMinor << "\n"
          << "python Buildscripts/release-sdk-esp32.py \"" << sdk.string() << "\"\n";
    }
    const std::string command = "sh \"" + script.string() + "\"";
#endif

    if (auto err = run("Building TactilitySDK " + std::string(TactilityPin::kVersion) + " for " + idfTarget,
                       command, Native(src.string()));
        !err.empty())
        return err;

    {
        std::ofstream marker(targetMarker, std::ios::trunc);
        marker << idfTarget << "\n";
    }

    if (!IsBuilt(idfTarget))
        return "the release script finished but " + Native(sdk.string()) + " has no idf-version.txt";
    onLine("TactilitySDK " + std::string(TactilityPin::kVersion) + " for " + idfTarget + " ready at " +
           Native(sdk.string()));
    return {};
}

}  // namespace DekiEditor
