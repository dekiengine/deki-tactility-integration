/**
 * @file TactilityApp.cpp
 * @brief The Tactility app's entry point.
 *
 * Tactility runs an external app by loading its binary and calling its
 * main() on the app's own task: a relocated ELF on a device, a dlopen()ed
 * shared object in the simulator. This is that main(). It brings up the
 * providers the engine needs, hooks the OS's close request into the engine's
 * main loop, and runs the game through Deki::Main() like every other target.
 *
 * What an app build ships, and where the engine finds it:
 *   assets/          (F:/) the boot payload: dproject.bin and boot.scene
 *   assets/storage/  (F:/storage/) the asset export: asset_table.bin,
 *                    pack_index.bin and the assets themselves
 * S:/ stays the app's user data directory, the one place it may write.
 */

#if defined(DEKI_TACTILITY_TARGET) && !defined(DEKI_EDITOR)

#include <deki/Engine.h>
#include <deki/LogSystem.h>
#include <deki/Main.h>
#include <deki/Time.h>
#include <deki/assets/AssetLookupTable.h>
#include <deki/assets/AssetManager.h>
#include <deki/assets/AssetPackReader.h>
#include <deki/providers/FileSystem.h>
#include <deki/providers/HostMemoryProvider.h>
#include <deki/providers/IFileSystem.h>
#include <deki/providers/Memory.h>

#include <memory>

#include "TactilityFileSystem.h"
#include "TactilityTimeProvider.h"

extern "C"
{
#include <app/event.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/log.h>
}

#ifndef DEKI_TACTILITY_APP_ID
#error "DEKI_TACTILITY_APP_ID must name the app (the Tactility builder defines it from the platform's appId)"
#endif

namespace DekiTactility
{
namespace
{

constexpr const char* kTag = "Deki";
constexpr const char* kAssetExport = "F:/storage/";

// The asset export's lookup table, loaded before the boot scene so that scene
// and everything after it resolve by key. The desktop host does the same from
// S:/; here the export ships read-only inside the app instead.
bool LoadAssetTable()
{
    const std::string tablePath = std::string(kAssetExport) + "asset_table.bin";
    Deki::IFileSystem* fs = Deki::FileSystem::GetFileSystemForPath(tablePath.c_str());
    if (fs == nullptr)
    {
        DEKI_LOG_ERROR("Tactility app: no filesystem for %s", tablePath.c_str());
        return false;
    }
    auto handle = fs->OpenFile(tablePath.c_str(), Deki::IFileSystem::OpenMode::READ_BINARY);
    if (!handle)
    {
        DEKI_LOG_ERROR("Tactility app: %s is missing; the app was built without an asset export",
                       tablePath.c_str());
        return false;
    }
    const long size = fs->GetFileSize(handle);
    if (size <= 0)
    {
        fs->CloseFile(handle);
        DEKI_LOG_ERROR("Tactility app: %s is empty", tablePath.c_str());
        return false;
    }

    // Lives as long as the app: the lookup table points into it.
    static uint8_t* s_Table = nullptr;
    Deki::Memory::Free(s_Table);
    s_Table = Deki::Memory::AllocateArray<uint8_t>(static_cast<size_t>(size), Deki::Memory::External);
    if (s_Table == nullptr)
    {
        fs->CloseFile(handle);
        DEKI_LOG_ERROR("Tactility app: no memory for the asset table (%ld bytes)", size);
        return false;
    }
    const size_t read = fs->ReadFile(handle, s_Table, static_cast<size_t>(size));
    fs->CloseFile(handle);
    if (read != static_cast<size_t>(size) ||
        !Deki::AssetManager::Get()->LoadAssetLookupTable(s_Table, static_cast<size_t>(size)))
    {
        DEKI_LOG_ERROR("Tactility app: could not load %s", tablePath.c_str());
        return false;
    }

    Deki::AssetManager::Get()->SetCacheDirectory(kAssetExport);
    Deki::AssetPackReader::Instance().LoadPackIndex((std::string(kAssetExport) + "pack_index.bin").c_str());
    DEKI_LOG_INFO("Tactility app: %u assets", Deki::AssetLookupTable::GetEntryCount());
    return true;
}

// The OS asks an app to close through an app event. Without listening for it
// the app cannot be closed at all, so the engine's loop polls every frame and
// stops when one arrives.
struct CloseListener
{
    TaskEventGroup group{};
    AppEventSubscription subscription{};
    bool subscribed = false;

    bool Subscribe()
    {
        task_event_group_construct(&group);
        subscribed = app_event_subscribe(&subscription, &group) == ERROR_NONE;
        return subscribed;
    }

    bool CloseRequested()
    {
        AppEvent event{};
        while (subscribed && app_event_poll(&subscription, &event) == ERROR_NONE)
        {
            if (event.type == APP_EVENT_CLOSE)
                return true;
        }
        return false;
    }

    ~CloseListener()
    {
        if (subscribed)
            app_event_unsubscribe(&subscription);
        task_event_group_destruct(&group);
    }
};

}  // namespace
}  // namespace DekiTactility

using namespace DekiTactility;

extern "C" int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    Deki::LogSystem::SetLogCallback([](Deki::LogLevel level, const std::string& msg, const char*, int) {
        if (level == Deki::LogLevel::Error)
            LOG_E(kTag, "%s", msg.c_str());
        else if (level == Deki::LogLevel::Warning)
            LOG_W(kTag, "%s", msg.c_str());
        else
            LOG_I(kTag, "%s", msg.c_str());
    });

    CloseListener close;
    if (!close.Subscribe())
    {
        LOG_E(kTag, "Could not subscribe to app events; the OS would have no way to close the game");
        return 1;
    }

    // Providers first: Engine::Initialize() initialises them. SetFileSystem
    // brings the filesystem up at once, which the asset table needs.
    Deki::Memory::SetBackend(new Deki::HostMemoryProvider());
    Deki::Time::SetTimeProvider(std::make_unique<TactilityTimeProvider>());
    if (!Deki::FileSystem::SetFileSystem(new TactilityFileSystem(DEKI_TACTILITY_APP_ID)) || !LoadAssetTable())
        return 1;

    Deki::Engine::GetInstance().RegisterUpdate([&close](uint32_t) {
        if (close.CloseRequested())
            Deki::Engine::GetInstance().StopMainLoop();
    });

    return Deki::Main();
}

#endif  // DEKI_TACTILITY_TARGET && !DEKI_EDITOR
