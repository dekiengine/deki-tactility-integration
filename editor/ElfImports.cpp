#include "ElfImports.h"

#include <cstdint>
#include <fstream>
#include <iterator>

namespace DekiEditor
{

namespace fs = std::filesystem;

ElfImports ReadElfImports(const fs::path& path)
{
    ElfImports result;
    std::ifstream in(path, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u16 = [&data](size_t at) -> uint32_t
    { return at + 2 <= data.size() ? uint8_t(data[at]) | (uint8_t(data[at + 1]) << 8) : 0; };
    auto u32 = [&data](size_t at) -> uint32_t
    {
        return at + 4 <= data.size() ? uint32_t(uint8_t(data[at])) | (uint32_t(uint8_t(data[at + 1])) << 8) |
                                           (uint32_t(uint8_t(data[at + 2])) << 16) |
                                           (uint32_t(uint8_t(data[at + 3])) << 24)
                                     : 0;
    };
    if (data.size() < 52 || data.compare(0, 4, "\x7f" "ELF") != 0 || data[4] != 1 || data[5] != 1)
    {
        result.error = "not a 32-bit little-endian ELF: " + path.string();
        return result;
    }

    constexpr uint32_t kShtDynsym = 11;
    constexpr uint32_t kShfTls = 0x400;
    const uint32_t shoff = u32(32);
    const uint32_t shentsize = u16(46);
    const uint32_t shnum = u16(48);
    auto section = [&](uint32_t i, uint32_t field) { return u32(shoff + i * shentsize + field); };

    for (uint32_t i = 0; i < shnum; ++i)
    {
        if (section(i, 8) & kShfTls)
        {
            result.hasTls = true;
        }
        if (section(i, 4) != kShtDynsym)
        {
            continue;
        }
        const uint32_t symOff = section(i, 16);
        const uint32_t symSize = section(i, 20);
        const uint32_t strIndex = section(i, 24);
        const uint32_t strOff = section(strIndex, 16);
        for (uint32_t at = symOff + 16; at + 16 <= symOff + symSize; at += 16)
        {
            const uint32_t name = u32(at);
            const uint32_t shndx = u16(at + 14);
            if (name == 0 || strOff + name >= data.size())
            {
                continue;
            }
            (shndx == 0 ? result.undefined : result.exported).emplace_back(data.c_str() + strOff + name);
        }
    }
    return result;
}

}  // namespace DekiEditor
