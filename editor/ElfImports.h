#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace DekiEditor
{

/// What a device app's ELF leaves for the loader to resolve, and whether it
/// has thread-local storage, read straight from the file (ELF32, little
/// endian, as every ESP32 chip is). Empty `error` on success.
struct ElfImports
{
    std::vector<std::string> undefined;
    // Symbols the app defines and exports. elf_loader resolves a relocation
    // that names one through the firmware's tables, not the app, and fails.
    std::vector<std::string> exported;
    bool hasTls = false;
    std::string error;
};

/// Reads the dynamic symbol table of the app at `path`. Its own file, apart
/// from TactilityBuilder, so the tests can reach it.
ElfImports ReadElfImports(const std::filesystem::path& path);

}  // namespace DekiEditor
