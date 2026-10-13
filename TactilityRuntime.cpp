// Static construction and destruction for a Tactility device app (see
// TactilityRuntime.h).

#if defined(DEKI_TACTILITY_TARGET) && !defined(DEKI_EDITOR)

#include "TactilityRuntime.h"

#if defined(ESP_PLATFORM)

#include <cstdint>
#include <cstdlib>

extern "C"
{
// This module's handle, which the compiler passes to __cxa_atexit for every
// static with a destructor. A start file defines it in each executable or
// shared object; the app links without start files, so it is defined here.
// Hidden, as the app is built: it identifies this app, not the firmware.
void* __dso_handle = &__dso_handle;

// Ends of the constructor tables, from the link script.
typedef void (*DekiCtor)();
extern DekiCtor __deki_ctors_start[];
extern DekiCtor __deki_ctors_end[];
extern DekiCtor __deki_init_array_start[];
extern DekiCtor __deki_init_array_end[];

int __cxa_atexit(void (*destructor)(void*), void* object, void* dso);
}

namespace
{

struct Destructor
{
    void (*fn)(void*);
    void* object;
};

// Plain storage, not a container: entries arrive while the statics are being
// constructed, before any container here could be.
Destructor* g_Destructors = nullptr;
size_t g_DestructorCount = 0;
size_t g_DestructorCapacity = 0;
bool g_Constructed = false;

}  // namespace

extern "C" int __cxa_atexit(void (*destructor)(void*), void* object, void*)
{
    if (g_DestructorCount == g_DestructorCapacity)
    {
        const size_t capacity = g_DestructorCapacity == 0 ? 64 : g_DestructorCapacity * 2;
        auto* grown = static_cast<Destructor*>(std::realloc(g_Destructors, capacity * sizeof(Destructor)));
        if (grown == nullptr)
        {
            return -1;
        }
        g_Destructors = grown;
        g_DestructorCapacity = capacity;
    }
    g_Destructors[g_DestructorCount++] = { destructor, object };
    return 0;
}

namespace DekiTactility
{

void RunStaticConstructors()
{
    if (g_Constructed)
    {
        return;
    }
    g_Constructed = true;

    // .ctors runs last entry first, as GCC's own start files run it.
    for (DekiCtor* ctor = __deki_ctors_end; ctor != __deki_ctors_start;)
    {
        --ctor;
        const auto value = reinterpret_cast<uintptr_t>(*ctor);
        if (value != 0 && value != UINTPTR_MAX)
        {
            (*ctor)();
        }
    }
    for (DekiCtor* ctor = __deki_init_array_start; ctor != __deki_init_array_end; ++ctor)
    {
        (*ctor)();
    }
}

void RunStaticDestructors()
{
    while (g_DestructorCount > 0)
    {
        const Destructor d = g_Destructors[--g_DestructorCount];
        d.fn(d.object);
    }
    std::free(g_Destructors);
    g_Destructors = nullptr;
    g_DestructorCapacity = 0;
}

}  // namespace DekiTactility

#else  // the simulator: dlopen() and dlclose() do both

namespace DekiTactility
{

void RunStaticConstructors() {}
void RunStaticDestructors() {}

}  // namespace DekiTactility

#endif  // ESP_PLATFORM
#endif  // DEKI_TACTILITY_TARGET && !DEKI_EDITOR
