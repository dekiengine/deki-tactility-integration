#pragma once

// What a C runtime's start files would do for an executable, done by the app
// itself on a device, where elf_loader starts main() directly.
//
// The loader relocates the app and calls main(); it runs no static
// constructors and keeps no list of static destructors. The app's link script
// (link/tactility-app-xtensa.ld) gathers the constructor tables between known
// symbols, and the app defines its own __cxa_atexit (Tactility exports none).
// main() calls these two around the game. In the simulator the app is a
// shared object dlopen() loads, which does both itself, so they do nothing.

namespace DekiTactility
{

/// Runs the app's static constructors, once.
void RunStaticConstructors();

/// Runs the destructors registered since, last first, and forgets them.
void RunStaticDestructors();

}  // namespace DekiTactility
