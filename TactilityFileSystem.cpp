#include "TactilityFileSystem.h"

#include <cstdio>
#include <cstring>

#include <deki/LogSystem.h>

#if defined(DEKI_TACTILITY_TARGET)
extern "C"
{
#include <app/paths.h>
}
#endif

namespace DekiTactility
{

namespace
{
/// Tactility documents 255 as the maximum path length.
constexpr size_t kMaxPath = 256;

const char* ModeToStdio(Deki::IFileSystem::OpenMode mode)
{
    switch (mode)
    {
        case Deki::IFileSystem::OpenMode::READ_BINARY:  return "rb";
        case Deki::IFileSystem::OpenMode::WRITE_BINARY: return "wb";
        case Deki::IFileSystem::OpenMode::READ_TEXT:    return "r";
        case Deki::IFileSystem::OpenMode::WRITE_TEXT:   return "w";
    }
    return "rb";
}

int OriginToStdio(Deki::IFileSystem::SeekOrigin origin)
{
    switch (origin)
    {
        case Deki::IFileSystem::SeekOrigin::BEGIN:   return SEEK_SET;
        case Deki::IFileSystem::SeekOrigin::CURRENT: return SEEK_CUR;
        case Deki::IFileSystem::SeekOrigin::END:     return SEEK_END;
    }
    return SEEK_SET;
}
}  // namespace

TactilityFileSystem::TactilityFileSystem(const char* appId) : m_AppId(appId != nullptr ? appId : "")
{
}

TactilityFileSystem::~TactilityFileSystem()
{
    Shutdown();
}

std::string TactilityFileSystem::Resolve(const char* virtualPath) const
{
    if (virtualPath == nullptr)
        return {};

    // Prefixes are matched case-sensitively, as the engine writes them.
    if (std::strncmp(virtualPath, "F:/", 3) == 0)
        return m_AssetsPath + "/" + (virtualPath + 3);
    if (std::strncmp(virtualPath, "S:/", 3) == 0)
        return m_UserDataPath + "/" + (virtualPath + 3);

    // No prefix: the engine's Storage::Default. Treat it as user data, which is
    // the only writable location an app has.
    if (virtualPath[0] == '/')
        return virtualPath;  // already absolute, pass through untouched
    return m_UserDataPath + "/" + virtualPath;
}

bool TactilityFileSystem::ConvertPath(const char* virtualPath, char* outBuffer, size_t bufferSize)
{
    const std::string resolved = Resolve(virtualPath);
    if (resolved.empty() || outBuffer == nullptr || resolved.size() + 1 > bufferSize)
        return false;
    std::memcpy(outBuffer, resolved.c_str(), resolved.size() + 1);
    return true;
}

#if defined(DEKI_TACTILITY_TARGET)

bool TactilityFileSystem::Initialize()
{
    if (m_Initialized)
        return true;

    if (m_AppId.empty())
    {
        DEKI_LOG_ERROR("TactilityFileSystem: no app id; both directories are keyed off it");
        return false;
    }

    char buffer[kMaxPath];

    if (app_paths_get_assets_directory(m_AppId.c_str(), buffer, sizeof(buffer)) != ERROR_NONE)
    {
        DEKI_LOG_ERROR("TactilityFileSystem: could not resolve the assets directory for '%s'",
                       m_AppId.c_str());
        return false;
    }
    m_AssetsPath = buffer;

    if (app_paths_get_user_data_directory(m_AppId.c_str(), buffer, sizeof(buffer)) != ERROR_NONE)
    {
        DEKI_LOG_ERROR("TactilityFileSystem: could not resolve the user data directory for '%s'",
                       m_AppId.c_str());
        return false;
    }
    m_UserDataPath = buffer;

    m_Initialized = true;
    DEKI_LOG_INFO("TactilityFileSystem: F:/ -> %s, S:/ -> %s", m_AssetsPath.c_str(),
                  m_UserDataPath.c_str());
    return true;
}

#else  // !DEKI_TACTILITY_TARGET — editor/host build, no Tactility SDK present

bool TactilityFileSystem::Initialize()
{
    return false;
}

#endif  // DEKI_TACTILITY_TARGET

void TactilityFileSystem::Shutdown()
{
    m_AssetsPath.clear();
    m_UserDataPath.clear();
    m_Initialized = false;
}

Deki::IFileSystem::FileHandle TactilityFileSystem::OpenFile(const char* path, OpenMode mode)
{
    if (!m_Initialized)
        return nullptr;
    const std::string resolved = Resolve(path);
    if (resolved.empty())
        return nullptr;
    return (FileHandle)std::fopen(resolved.c_str(), ModeToStdio(mode));
}

void TactilityFileSystem::CloseFile(FileHandle handle)
{
    if (handle != nullptr)
        std::fclose((FILE*)handle);
}

size_t TactilityFileSystem::ReadFile(FileHandle handle, void* buffer, size_t size)
{
    if (handle == nullptr || buffer == nullptr)
        return 0;
    return std::fread(buffer, 1, size, (FILE*)handle);
}

size_t TactilityFileSystem::WriteFile(FileHandle handle, const void* buffer, size_t size)
{
    if (handle == nullptr || buffer == nullptr)
        return 0;
    return std::fwrite(buffer, 1, size, (FILE*)handle);
}

long TactilityFileSystem::SeekFile(FileHandle handle, long offset, SeekOrigin origin)
{
    if (handle == nullptr)
        return -1;
    return std::fseek((FILE*)handle, offset, OriginToStdio(origin));
}

long TactilityFileSystem::TellFile(FileHandle handle)
{
    if (handle == nullptr)
        return -1;
    return std::ftell((FILE*)handle);
}

long TactilityFileSystem::GetFileSize(FileHandle handle)
{
    if (handle == nullptr)
        return -1;

    FILE* file = (FILE*)handle;
    const long current = std::ftell(file);
    if (current < 0 || std::fseek(file, 0, SEEK_END) != 0)
        return -1;
    const long size = std::ftell(file);
    // Put the cursor back: callers use this mid-read to size a buffer.
    std::fseek(file, current, SEEK_SET);
    return size;
}

bool TactilityFileSystem::FileExists(const char* path)
{
    if (!m_Initialized)
        return false;
    const std::string resolved = Resolve(path);
    if (resolved.empty())
        return false;

    FILE* file = std::fopen(resolved.c_str(), "rb");
    if (file == nullptr)
        return false;
    std::fclose(file);
    return true;
}

}  // namespace DekiTactility
