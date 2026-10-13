#include "TactilityToolchain.h"

#include <deki-editor/EditorPaths.h>
#include <deki-editor/ShellCommand.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

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
    {
        return "generic-" + idfTarget;
    }
    return {};
}

std::string Native(std::string p)
{
#ifdef _WIN32
    std::replace(p.begin(), p.end(), '/', '\\');
#endif
    return p;
}

// The simulator's one patch (see TactilityToolchain.h). Recorded in the SDK it
// builds, so an SDK without the record counts as not built.
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
        {
            return "the simulator source " + file.string() + " is missing";
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (text.find("deki_sdl_display_config") != std::string::npos)
    {
        return {};  // already patched: the checkout keeps local changes
    }
    const size_t at = text.find(kSimulatorSizeLine);
    if (at == std::string::npos)
    {
        return "the simulator's display setup is not what the Deki patch expects in " + file.string() +
               "; the pinned Tactility changed, and the patch needs updating with it";
    }
    text.replace(at, std::string(kSimulatorSizeLine).size(), kSimulatorSizePatch);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    return out ? std::string() : "could not write " + file.string();
}

// Reads one Tactility source line by line and collects the symbols its export
// tables are sure to hold on one chip.
//
// Not a preprocessor, and not meant to be: it follows the #if nesting and
// decides each condition three ways. The chip's own target macro
// (CONFIG_IDF_TARGET_<CHIP>) and ESP_PLATFORM are known; a macro the file
// itself #defines to 0 or 1 under known conditions is known; a defined() test
// joined by || or && is worked out from those. Anything else (a board's
// radios, TinyUSB, FreeRTOS options) is unknown, and an entry under an unknown
// condition does not count: an app that leaned on it would load on some
// devices and not others, so the build should say so instead.
class ExportScan
{
public:
    explicit ExportScan(std::string chip) : m_Chip(std::move(chip)) {}

    void Line(std::string line, std::vector<std::string>& found)
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '#')
        {
            Directive(line.substr(first + 1));
            return;
        }
        if (!Active())
        {
            return;
        }
        Collect(line, found);
    }

private:
    enum class Truth
    {
        False,
        True,
        Unknown
    };

    struct Branch
    {
        Truth current;  // whether the branch being read is compiled
        Truth taken;    // whether any branch so far was
    };

    static bool IsIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

    static std::string Trim(const std::string& s)
    {
        const size_t b = s.find_first_not_of(" \t");
        const size_t e = s.find_last_not_of(" \t");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    }

    static Truth Not(Truth t)
    {
        return t == Truth::Unknown ? t : (t == Truth::True ? Truth::False : Truth::True);
    }

    bool Active() const
    {
        return std::all_of(m_Stack.begin(), m_Stack.end(), [](const Branch& b) { return b.current == Truth::True; });
    }

    Truth Defined(const std::string& name) const
    {
        if (name.rfind("CONFIG_IDF_TARGET_", 0) == 0)
        {
            return name == "CONFIG_IDF_TARGET_" + m_Chip ? Truth::True : Truth::False;
        }
        if (name == "ESP_PLATFORM")
        {
            return Truth::True;
        }
        return Truth::Unknown;
    }

    // One term: [!]defined(NAME), [!]defined NAME, or [!]NAME of a macro this
    // file set to 0 or 1.
    Truth Term(std::string term) const
    {
        term = Trim(term);
        bool negate = false;
        if (!term.empty() && term[0] == '!')
        {
            negate = true;
            term = Trim(term.substr(1));
        }
        Truth value = Truth::Unknown;
        if (term.rfind("defined", 0) == 0)
        {
            std::string name;
            for (char c : term.substr(7))
            {
                if (IsIdent(c))
                {
                    name += c;
                }
                else if (!name.empty())
                {
                    break;
                }
            }
            value = Defined(name);
        }
        else if (!term.empty() && std::all_of(term.begin(), term.end(), IsIdent))
        {
            const auto it = m_Defines.find(term);
            if (it != m_Defines.end())
            {
                value = it->second ? Truth::True : Truth::False;
            }
        }
        return negate ? Not(value) : value;
    }

    static std::vector<std::string> Split(const std::string& s, const std::string& op)
    {
        std::vector<std::string> parts;
        size_t from = 0;
        for (size_t at = s.find(op); at != std::string::npos; at = s.find(op, from))
        {
            parts.push_back(s.substr(from, at - from));
            from = at + op.size();
        }
        parts.push_back(s.substr(from));
        return parts;
    }

    Truth Condition(std::string expr) const
    {
        if (const size_t comment = expr.find("//"); comment != std::string::npos)
        {
            expr.resize(comment);
        }
        if (const size_t comment = expr.find("/*"); comment != std::string::npos)
        {
            expr.resize(comment);
        }
        expr = Trim(expr);
        const bool hasOr = expr.find("||") != std::string::npos;
        const bool hasAnd = expr.find("&&") != std::string::npos;
        if (hasOr && hasAnd)
        {
            return Truth::Unknown;
        }
        if (hasOr || hasAnd)
        {
            bool anyTrue = false, anyFalse = false, anyUnknown = false;
            for (const auto& part : Split(expr, hasOr ? "||" : "&&"))
            {
                const Truth t = Term(part);
                anyTrue |= t == Truth::True;
                anyFalse |= t == Truth::False;
                anyUnknown |= t == Truth::Unknown;
            }
            if (hasOr)
            {
                return anyTrue ? Truth::True : (anyUnknown ? Truth::Unknown : Truth::False);
            }
            return anyFalse ? Truth::False : (anyUnknown ? Truth::Unknown : Truth::True);
        }
        return Term(expr);
    }

    void Directive(const std::string& text)
    {
        std::string body = Trim(text);
        std::string word;
        size_t p = 0;
        while (p < body.size() && IsIdent(body[p]))
        {
            word += body[p++];
        }
        const std::string rest = body.substr(p);

        if (word == "ifdef" || word == "ifndef")
        {
            const Truth t = Defined(Trim(rest));
            const Truth v = word == "ifdef" ? t : Not(t);
            m_Stack.push_back({ v, v });
        }
        else if (word == "if")
        {
            const Truth v = Condition(rest);
            m_Stack.push_back({ v, v });
        }
        else if (word == "elif" && !m_Stack.empty())
        {
            Branch& b = m_Stack.back();
            if (b.taken == Truth::True)
            {
                b.current = Truth::False;
            }
            else if (b.taken == Truth::False)
            {
                b.current = Condition(rest);
                b.taken = b.current;
            }
            else
            {
                b.current = Truth::Unknown;
            }
        }
        else if (word == "else" && !m_Stack.empty())
        {
            Branch& b = m_Stack.back();
            b.current = Not(b.taken);
            b.taken = Truth::True;
        }
        else if (word == "endif" && !m_Stack.empty())
        {
            m_Stack.pop_back();
        }
        else if (word == "define" && Active())
        {
            // #define NAME 0|1, the one form a condition later reads.
            const std::string def = Trim(rest);
            const size_t space = def.find_first_of(" \t");
            if (space != std::string::npos)
            {
                const std::string value = Trim(def.substr(space));
                if (value == "0" || value == "1")
                {
                    m_Defines[def.substr(0, space)] = value == "1";
                }
            }
        }
    }

    static void Collect(const std::string& line, std::vector<std::string>& found)
    {
        for (size_t at = line.find("DEFINE_MODULE_SYMBOL"); at != std::string::npos;
             at = line.find("DEFINE_MODULE_SYMBOL", at + 1))
        {
            size_t p = at + std::strlen("DEFINE_MODULE_SYMBOL");
            const bool alias = line.compare(p, 6, "_ALIAS") == 0;
            while (p < line.size() && line[p] != '(')
            {
                ++p;
            }
            if (p >= line.size())
            {
                continue;
            }
            ++p;
            while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p])))
            {
                ++p;
            }
            std::string name;
            if (alias)
            {
                if (p < line.size() && line[p] == '"')
                {
                    const size_t close = line.find('"', p + 1);
                    if (close != std::string::npos)
                    {
                        name = line.substr(p + 1, close - p - 1);
                    }
                }
            }
            else
            {
                while (p < line.size() && IsIdent(line[p]))
                {
                    name += line[p++];
                }
            }
            if (!name.empty() && !std::isdigit(static_cast<unsigned char>(name[0])) && name != "symbol")
            {
                found.push_back(name);
            }
        }

        // { "name", (void*)... }
        for (size_t at = line.find("{ \""); at != std::string::npos; at = line.find("{ \"", at + 1))
        {
            const size_t close = line.find('"', at + 3);
            if (close == std::string::npos)
            {
                break;
            }
            const std::string name = line.substr(at + 3, close - at - 3);
            size_t p = close + 1;
            while (p < line.size() && (line[p] == ' ' || line[p] == ','))
            {
                ++p;
            }
            if (!name.empty() && std::all_of(name.begin(), name.end(), IsIdent) && line.compare(p, 6, "(void*") == 0)
            {
                found.push_back(name);
            }
        }
    }

    std::string m_Chip;
    std::vector<Branch> m_Stack;
    std::map<std::string, bool> m_Defines;
};

std::string ReadFirstLine(const fs::path& file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
    {
        line.pop_back();
    }
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

std::vector<std::string> TactilitySdk::AppExports(const std::string& platform, std::string& error)
{
    std::vector<std::string> names;
    std::error_code ec;
    const fs::path cache = fs::path(SdkDir(platform)).parent_path() / "deki-app-exports-v2.txt";
    if (fs::is_regular_file(cache, ec))
    {
        std::ifstream in(cache);
        for (std::string line; std::getline(in, line);)
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (!line.empty())
            {
                names.push_back(line);
            }
        }
        if (!names.empty())
        {
            return names;
        }
    }

    // A module's symbol table lists its entries with Tactility's macros
    // (tactility/module.h), or as a literal { "name", (void*)... } pair:
    //   DEFINE_MODULE_SYMBOL(name)
    //   DEFINE_MODULE_SYMBOL_SIGNATURE(name, type)
    //   DEFINE_MODULE_SYMBOL_ALIAS("name", symbol)
    // An entry counts only where the preprocessor is sure to keep it for this
    // chip (ExportScan): the tables leave out a chip's missing helpers and a
    // board's absent radios behind #if.
    const fs::path src = SourceDir();
    const std::vector<fs::path> roots = { src / "Modules", src / "TactilityKernel" / "source",
                                          src / "Platforms" / "platform-esp32" };
    if (!fs::is_directory(src / "Modules", ec))
    {
        error = "the Tactility sources are not at " + Native(src.string());
        return {};
    }

    std::string chip = platform;
    std::transform(chip.begin(), chip.end(), chip.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::vector<std::string> found;
    for (const auto& root : roots)
    {
        if (!fs::is_directory(root, ec))
        {
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            const std::string ext = it->path().extension().string();
            if (!it->is_regular_file(ec) || (ext != ".c" && ext != ".cpp"))
            {
                continue;
            }
            std::ifstream in(it->path(), std::ios::binary);
            ExportScan scan(chip);
            for (std::string line; std::getline(in, line);)
            {
                scan.Line(line, found);
            }
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    if (found.empty())
    {
        error = "no exported symbols found in the Tactility sources at " + Native(src.string());
        return {};
    }

    fs::create_directories(cache.parent_path(), ec);
    std::ofstream out(cache, std::ios::binary | std::ios::trunc);
    for (const auto& name : found)
    {
        out << name << "\n";
    }
    return found;
}

std::string TactilitySdk::IdfEnvironmentScript(const std::string& idfPath)
{
#ifdef _WIN32
    // MSYSTEM cleared: ESP-IDF's scripts refuse to run when it is set, and
    // exit 0 while doing so. export.bat by full path: a bare name after a cd
    // fails under NoDefaultCurrentDirectoryInExePath.
    //
    // The editor's own Python goes first when it has one. export.bat picks its
    // virtual environment by the version of the first python on PATH, and the
    // environment ESP-IDF installed was made with the editor's; a newer system
    // Python makes it look for one that does not exist and stop.
    std::string lines = "set \"MSYSTEM=\"\r\n";
    const fs::path python = fs::path(EditorPaths::GetToolchainsDir()) / "espressif" / "python" / "tools";
    std::error_code ec;
    if (fs::is_regular_file(python / "python.exe", ec))
    {
        lines += "set \"PATH=" + Native(python.string()) + ";" + Native((python / "Scripts").string()) +
                 ";%PATH%\"\r\n";
    }
    lines += "cd /d \"" + Native(idfPath) + "\" || exit /b 1\r\n";
    lines += "call \"" + Native(idfPath) + "\\export.bat\" || exit /b 1\r\n";
    return lines;
#else
    return ". \"" + idfPath + "/export.sh\"\n";
#endif
}

bool TactilitySdk::IsBuilt(const std::string& platform)
{
    if (platform.empty())
    {
        return false;
    }
    std::error_code ec;
    const fs::path sdk = SdkDir(platform);
    const fs::path tool = ToolDir();
    if (!fs::is_regular_file(tool / "tactility.py", ec))
    {
        return false;
    }
    // release-sdk-posix.py writes the top-level CMakeLists.txt last;
    // release-sdk-esp32.py writes idf-version.txt last. A device app also
    // needs the app configuration tactility.py would otherwise download.
    if (IsPosix(platform))
    {
        return fs::is_regular_file(sdk / "CMakeLists.txt", ec) &&
               ReadFirstLine(sdk / kSimulatorPatchMarker) == kSimulatorPatchId;
    }
    return fs::is_regular_file(sdk / "idf-version.txt", ec) &&
           fs::is_regular_file(tool / "CDN" / ("sdkconfig.app." + platform), ec);
}

std::string TactilitySdk::Build(const std::string& platform, const std::string& idfPath,
                                const std::function<void(const std::string&)>& onLine, const std::atomic<bool>* cancel)
{
    const bool posix = IsPosix(platform);
    const std::string device = posix ? std::string() : DeviceFor(platform);
    if (!posix && device.empty())
    {
        return "no Tactility SDK for '" + platform + "'; an app runs only on esp32s3, esp32p4 or the simulator";
    }
#ifdef _WIN32
    if (posix)
    {
        return "the Tactility simulator SDK builds on Linux only";
    }
#endif

    std::error_code ec;
    const fs::path src = SourceDir();
    fs::create_directories(fs::path(Root()), ec);

    auto run = [&](const std::string& what, const std::string& line, const std::string& dir) -> std::string
    {
        onLine("> " + what);
        const int code = RunShellCommand(line, dir, onLine, cancel);
        if (cancel != nullptr && cancel->load())
        {
            return "cancelled";
        }
        if (code != 0)
        {
            return what + " failed (exit code " + std::to_string(code) + ")";
        }
        return {};
    };

    // A repository at a pinned commit. A partial clone (history without file
    // contents) is all a checkout of one commit needs. The checkout is then
    // verified, so the pin is enforced.
    auto checkout = [&](const std::string& name, const std::string& url, const std::string& commit, const fs::path& dir,
                        bool submodules) -> std::string
    {
        if (!fs::exists(dir / ".git", ec))
        {
            if (auto err =
                    run("Cloning " + name,
                        "git clone --filter=blob:none --no-checkout \"" + url + "\" \"" + Native(dir.string()) + "\"",
                        Native(Root()));
                !err.empty())
            {
                return err;
            }
        }
        const std::string git = "git -C \"" + Native(dir.string()) + "\" ";
        if (auto err = run("Checking out " + name + " " + commit.substr(0, 12), git + "checkout --detach " + commit,
                           Native(Root()));
            !err.empty())
        {
            return err;
        }
        if (submodules)
        {
            if (auto err = run("Fetching " + name + " submodules",
                               git + "submodule update --init --recursive --filter=blob:none", Native(Root()));
                !err.empty())
            {
                return err;
            }
        }
        std::string head;
        RunShellCommand(git + "rev-parse HEAD", Native(Root()), [&head](const std::string& l) { head = l; }, cancel);
        if (head != commit)
        {
            return "the " + name + " checkout is at '" + head + "', not the pinned " + commit;
        }
        return {};
    };

    // 1. Tactility's sources, with their submodules, and TactilityTool.
    if (auto err = checkout("Tactility", TactilityPin::kRepository, TactilityPin::kCommit, src, true); !err.empty())
    {
        return err;
    }
    if (auto err = checkout("TactilityTool", TactilityPin::kToolRepository, TactilityPin::kToolCommit,
                            fs::path(ToolDir()), false);
        !err.empty())
    {
        return err;
    }
    if (!posix && !fs::is_regular_file(fs::path(ToolDir()) / "CDN" / ("sdkconfig.app." + platform), ec))
    {
        return "TactilityTool at the pinned commit has no CDN/sdkconfig.app." + platform;
    }

    const fs::path sdk = SdkDir(platform);
    fs::remove_all(sdk, ec);
    fs::create_directories(sdk.parent_path(), ec);

    // 2. Build and package with Tactility's own scripts, the steps its CI runs
    //    to publish an SDK.
    fs::path script;
    std::string command;
    fs::path targetMarker;
    if (posix)
    {
        if (auto err = PatchSimulator(src); !err.empty())
        {
            return err;
        }
        onLine("Simulator patched: display size from TACTILITY_SIMULATOR_RESOLUTION");

        // The simulator: Tactility's root CMakeLists builds it into buildsim/
        // when ESP-IDF is not in the environment, and release-sdk-posix.py
        // packages the libraries from there. Tests and mbedtls's sample
        // programs are off: they are most of the build time and not part of
        // the SDK. The compiler is found the way the engine's reflection
        // codegen finds it, so the simulator, its SDK and an app built against
        // it agree.
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
        // removed first: idf.py refuses to switch targets in an existing one.
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
        f << "@echo off\r\n"
          << IdfEnvironmentScript(idfPath) << "cd /d \"" << Native(src.string()) << "\" || exit /b 1\r\n"
          << "python device.py " << device << " || exit /b 1\r\n"
          << "idf.py build || exit /b 1\r\n"
          << "set \"ESP_IDF_VERSION=" << TactilityPin::kIdfMajorMinor << "\"\r\n"
          << "python Buildscripts\\release-sdk-esp32.py \"" << Native(sdk.string()) << "\" || exit /b 1\r\n";
        command = "call \"" + Native(script.string()) + "\"";
#else
        script = fs::path(Root()) / ("build-sdk-" + platform + ".sh");
        std::ofstream f(script, std::ios::binary | std::ios::trunc);
        f << "set -e\n"
          << IdfEnvironmentScript(idfPath) << "cd \"" << src.string() << "\"\n"
          << "python device.py " << device << "\n"
          << "idf.py build\n"
          << "export ESP_IDF_VERSION=" << TactilityPin::kIdfMajorMinor << "\n"
          << "python Buildscripts/release-sdk-esp32.py \"" << sdk.string() << "\"\n";
        command = "sh \"" + script.string() + "\"";
#endif
    }

    if (auto err = run("Building TactilitySDK " + std::string(TactilityPin::kVersion) + " for " + platform, command,
                       Native(src.string()));
        !err.empty())
    {
        return err;
    }

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
    {
        return "the release script finished but " + Native(sdk.string()) + " is incomplete";
    }
    onLine("TactilitySDK " + std::string(TactilityPin::kVersion) + " for " + platform + " ready at " +
           Native(sdk.string()));
    return {};
}

std::string TactilitySdk::EnsureSimulator(int width, int height, const std::function<void(const std::string&)>& onLine,
                                          const std::atomic<bool>* cancel)
{
#ifdef _WIN32
    (void)width;
    (void)height;
    (void)onLine;
    (void)cancel;
    return "the Tactility simulator runs on Linux only";
#else
    // tactility.py talks to the development service on port 6666, so a
    // simulator that answers there is running. The tool already requires
    // python3.
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
        const bool ours =
            pid > 0 && RunShellCommand("kill -0 " + std::to_string(pid) + " 2>/dev/null", Root(), quiet, cancel) == 0;
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
            {
                break;
            }
            RunShellCommand("sleep 0.25", Root(), quiet, cancel);
        }
    }

    const fs::path src = SourceDir();
    const fs::path binary = src / "buildsim" / "Tactility" / "Tactility";
    const fs::path data = src / "Data";
    std::error_code ec;
    if (!fs::is_regular_file(binary, ec))
    {
        return "the simulator is not built (" + binary.string() + "); install the TactilitySDK component";
    }

    // Its development service, through which apps are installed, is off
    // unless a setting turns it on at boot (the one its Development app
    // writes).
    const fs::path settings = data / "data" / "tactility" / "user" / "app" / "tactility.development";
    fs::create_directories(settings, ec);
    {
        std::ofstream f(settings / "development.properties", std::ios::trunc);
        f << "enableOnBoot=true\n";
    }

    // Detached from the editor (its own session, output to a log beside the
    // SDK) so it outlives this deploy, like a simulator the user starts. $! is
    // the simulator itself: setsid, not being a group leader here, execs
    // instead of forking, and so does nohup.
    const fs::path log = fs::path(Root()) / "simulator.log";
    onLine("Starting the Tactility simulator at " + resolution + " (log: " + log.string() + ")");
    const std::string start = "cd \"" + data.string() + "\" && TACTILITY_SIMULATOR_RESOLUTION=" + resolution +
                              " setsid nohup \"" + binary.string() + "\" > \"" + log.string() +
                              "\" 2>&1 < /dev/null & echo \"$! " + resolution + "\" > \"" + started.string() + "\"";
    if (RunShellCommand(start, data.string(), onLine, cancel) != 0)
    {
        return "could not start the simulator";
    }

    for (int attempt = 0; attempt < 60; ++attempt)
    {
        if (cancel != nullptr && cancel->load())
        {
            return "cancelled";
        }
        if (RunShellCommand(probe + " 2>/dev/null", Root(), quiet, cancel) == 0)
        {
            return {};
        }
        RunShellCommand("sleep 0.5", Root(), quiet, cancel);
    }
    return "the simulator did not start answering on port 6666; see " + log.string();
#endif
}

}  // namespace DekiEditor
