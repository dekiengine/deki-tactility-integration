#pragma once

#include <string>

#include <deki/providers/IFileSystem.h>

namespace DekiTactility
{

/// Deki filesystem mapped onto the directories Tactility gives an app. Deki's
/// two virtual prefixes map to:
///
///   F:/  flash    -> the app's assets directory: the game's data shipped in
///                    the .app bundle (project_data.bin, boot.scene, assets),
///                    read and never written. F:/ files the game writes go to
///                    internal/ in the user data directory, and are read from
///                    there when the game's data has no file of that name: as
///                    on a board.
///   S:/  SD card  -> the app's user data directory: anything the game writes,
///                    which Tactility keeps across OS upgrades.
///
/// Tactility gives real POSIX paths, so after the prefix this is plain stdio,
/// like the engine's DesktopFileSystem. That class is not reused because its
/// base paths are fixed in its constructor, and changing that would be an
/// engine change.
class TactilityFileSystem : public Deki::IFileSystem
{
public:
    /// `appId` is the id from manifest.properties; Tactility names both
    /// directories after it.
    explicit TactilityFileSystem(const char* appId);
    ~TactilityFileSystem() override;

    bool Initialize() override;
    void Shutdown() override;

    FileHandle OpenFile(const char* path, OpenMode mode) override;
    void CloseFile(FileHandle handle) override;
    size_t ReadFile(FileHandle handle, void* buffer, size_t size) override;
    size_t WriteFile(FileHandle handle, const void* buffer, size_t size) override;
    long SeekFile(FileHandle handle, long offset, SeekOrigin origin) override;
    long TellFile(FileHandle handle) override;
    long GetFileSize(FileHandle handle) override;
    bool FileExists(const char* path) override;
    bool ConvertPath(const char* virtualPath, char* outBuffer, size_t bufferSize) override;

private:
    /// Resolves a virtual path to a real one. An F:/ path is the game's data
    /// when that has the file, else internal/ in the user data. Empty on
    /// failure.
    std::string Resolve(const char* virtualPath) const;

    /// True when `virtualPath` names a file of the game's data (F:/ in the
    /// assets directory), which is never written.
    bool IsGameData(const char* virtualPath) const;

    std::string m_AppId;
    std::string m_AssetsPath;    ///< real path behind "F:/", no trailing slash
    std::string m_UserDataPath;  ///< real path behind "S:/", no trailing slash
    bool m_Initialized = false;
};

}  // namespace DekiTactility
