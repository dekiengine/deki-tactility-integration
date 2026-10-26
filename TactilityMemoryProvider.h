#pragma once

#include <deki/providers/IMemoryProvider.h>

#include <cstddef>
#include <cstdlib>

// Tactility's own allocator, which places each block where it is asked to.
//
// Under Tactility the app shares internal RAM with the OS, which needs it for
// DMA buffers: every SD card read, Wi-Fi, the display. Plain malloc puts every
// block under 16 KB in internal RAM first, so a scene's thousands of small
// allocations (objects, components, strings) used to take the last of it, and
// then the SD card could no longer read a file: a scene switch found no scene.
//
// So the engine's ordinary allocations (operator new) go to PSRAM, and only
// what the engine asks to have internal (a strip, a staging band) is internal.
// On the bare-metal targets the game owns the board, and they keep ordinary
// allocations internal.
#if defined(DEKI_TACTILITY_TARGET)
extern "C"
{
#include <tactility/memory.h>
}
#endif

namespace DekiTactility
{

class TactilityMemoryProvider : public Deki::IMemoryProvider
{
public:
    bool Initialize() override { return true; }
    void Shutdown() override {}

    bool Serves(Deki::Memory::Region region) const override
    {
        return region == Deki::Memory::Internal || region == Deki::Memory::External;
    }

#if defined(DEKI_TACTILITY_TARGET)
    void* Allocate(Deki::Memory::Region region, size_t bytes, bool needsDma) override
    {
        MemoryPolicy policy{};
        if (region == Deki::Memory::Internal)
        {
            policy.required = MEMORY_CAPABILITY_INTERNAL;
        }
        else if (region == Deki::Memory::External)
        {
            policy.required = MEMORY_CAPABILITY_EXTERNAL;
        }
        else
        {
            return nullptr;
        }
        if (needsDma)
        {
            policy.required |= MEMORY_CAPABILITY_DMA;
        }
        return memory_alloc_with_policy(bytes, &policy);
    }

    void Free(Deki::Memory::Region, void* ptr) override { memory_free(ptr); }

    // PSRAM preferred, and internal RAM when PSRAM is full or absent: a block
    // operator new hands out must still come back when it can.
    void* AllocateRaw(size_t size) override
    {
        MemoryPolicy policy{};
        policy.desired = MEMORY_CAPABILITY_EXTERNAL;
        return memory_alloc_with_policy(size, &policy);
    }

    void FreeRaw(void* ptr) override { memory_free(ptr); }
#else
    // Off-target (the editor compiling the package) nothing allocates through it.
    void* Allocate(Deki::Memory::Region, size_t, bool) override { return nullptr; }
    void Free(Deki::Memory::Region, void*) override {}
#endif

    // 0 means unknown: Tactility exports no free-size query to apps. The
    // render system then draws in strips, which keeps a framebuffer out of
    // the OS's internal RAM.
    size_t GetAvailable(Deki::Memory::Region) const override { return 0; }
};

}  // namespace DekiTactility
