#pragma once

#include <string>

#include <deki/providers/IFileSystem.h>

namespace DekiTactility
{

/**
 * @brief Deki filesystem mapped onto the directories Tactility gives an app.
 *
 * Deki addresses storage through two virtual prefixes, and Tactility has a
 * natural home for each:
 *
 *   F:/  flash    -> the app's ASSETS directory. Read-only game data that ships
 *                    inside the .app bundle: dproject.bin and boot.scene.
 *   S:/  SD card  -> the app's USER DATA directory. Anything the game writes,
 *                    which Tactility documents as surviving OS upgrades.
 *
 * Tactility hands out real POSIX paths, so once a prefix is resolved this is
 * ordinary stdio — the same shape as the engine's DesktopFileSystem. That class
 * is not reused because its two base paths are fixed in its constructor with no
 * setter, and adding one would be an engine change this package exists to
 * avoid.
 */
class TactilityFileSystem : public Deki::IFileSystem
{
   public:
    /**
     * @param appId the id from manifest.properties; Tactility keys both
     *              directories off it.
     */
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
    /// Resolve a virtual path to a real one. Empty on failure.
    std::string Resolve(const char* virtualPath) const;

    std::string m_AppId;
    std::string m_AssetsPath;    ///< real path behind "F:/", no trailing slash
    std::string m_UserDataPath;  ///< real path behind "S:/", no trailing slash
    bool m_Initialized = false;
};

}  // namespace DekiTactility
