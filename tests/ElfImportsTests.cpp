// ReadElfImports, which decides whether a Tactility device app will load
// before it is copied to a device: what the app leaves for elf_loader to
// resolve, what it exports (which elf_loader then fails to find), and whether
// it has thread-local storage (which elf_loader cannot place).
//
// The ELF files are built byte by byte here, as small as the reader allows,
// so each test states exactly the section table it is about.

#include <gtest/gtest.h>

#include "editor/ElfImports.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace DekiEditor;
namespace fs = std::filesystem;

namespace
{

constexpr uint32_t kShtStrtab = 3;
constexpr uint32_t kShtDynsym = 11;
constexpr uint32_t kShtNobits = 8;
constexpr uint32_t kShfTls = 0x400;

void Put16(std::string& b, size_t at, uint32_t v)
{
    b[at] = char(v & 0xFF);
    b[at + 1] = char((v >> 8) & 0xFF);
}

void Put32(std::string& b, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        b[at + i] = char((v >> (8 * i)) & 0xFF);
    }
}

struct Symbol
{
    std::string name;
    bool defined;  // section index 1 rather than 0 (undefined)
};

// An ELF32 little-endian file: a .dynsym with `symbols` (after the null
// entry), its .dynstr, and a .tbss marked TLS when `tls`.
std::string MakeElf(const std::vector<Symbol>& symbols, bool tls = false)
{
    std::string strtab(1, '\0');
    std::vector<uint32_t> nameOffsets;
    for (const auto& s : symbols)
    {
        nameOffsets.push_back(static_cast<uint32_t>(strtab.size()));
        strtab += s.name + '\0';
    }

    const uint32_t symOff = 52;
    const uint32_t symSize = 16 * static_cast<uint32_t>(symbols.size() + 1);
    const uint32_t strOff = symOff + symSize;
    const uint32_t shOff = strOff + static_cast<uint32_t>(strtab.size());
    const uint32_t shNum = tls ? 4 : 3;

    std::string b(shOff + 40 * shNum, '\0');
    b.replace(0, 4, "\x7f" "ELF");
    b[4] = 1;  // 32-bit
    b[5] = 1;  // little endian
    Put32(b, 32, shOff);
    Put16(b, 46, 40);
    Put16(b, 48, shNum);

    for (size_t i = 0; i < symbols.size(); ++i)
    {
        const size_t at = symOff + 16 * (i + 1);
        Put32(b, at, nameOffsets[i]);
        Put16(b, at + 14, symbols[i].defined ? 1 : 0);
    }
    b.replace(strOff, strtab.size(), strtab);

    auto header = [&](uint32_t index, uint32_t type, uint32_t flags, uint32_t offset, uint32_t size, uint32_t link)
    {
        const size_t at = shOff + 40 * index;
        Put32(b, at + 4, type);
        Put32(b, at + 8, flags);
        Put32(b, at + 16, offset);
        Put32(b, at + 20, size);
        Put32(b, at + 24, link);
    };
    header(1, kShtDynsym, 0, symOff, symSize, 2);
    header(2, kShtStrtab, 0, strOff, static_cast<uint32_t>(strtab.size()), 0);
    if (tls)
    {
        header(3, kShtNobits, kShfTls | 0x3, 0, 4, 0);
    }
    return b;
}

class ElfImportsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Path = fs::temp_directory_path() /
                 ("deki-elf-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + ".elf");
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove(m_Path, ec);
    }

    ElfImports Read(const std::string& bytes)
    {
        std::ofstream(m_Path, std::ios::binary) << bytes;
        return ReadElfImports(m_Path);
    }

    fs::path m_Path;
};

}  // namespace

TEST_F(ElfImportsTest, UndefinedAndDefinedSymbolsAreToldApart)
{
    const ElfImports r = Read(MakeElf({ { "printf", false }, { "malloc", false }, { "game_entry", true } }));
    ASSERT_EQ(r.error, "");
    EXPECT_EQ(r.undefined, (std::vector<std::string>{ "printf", "malloc" }));
    EXPECT_EQ(r.exported, (std::vector<std::string>{ "game_entry" }));
    EXPECT_FALSE(r.hasTls);
}

TEST_F(ElfImportsTest, AnAppWithNoSymbolsIsEmpty)
{
    const ElfImports r = Read(MakeElf({}));
    EXPECT_EQ(r.error, "");
    EXPECT_TRUE(r.undefined.empty());
    EXPECT_TRUE(r.exported.empty());
}

TEST_F(ElfImportsTest, ThreadLocalStorageIsFound)
{
    EXPECT_TRUE(Read(MakeElf({ { "printf", false } }, /*tls=*/true)).hasTls);
}

TEST_F(ElfImportsTest, ANameOutsideTheFileIsSkipped)
{
    std::string elf = MakeElf({ { "printf", false }, { "puts", false } });
    Put32(elf, 52 + 16, 0x7FFFFFF0);  // the first symbol's name, far past the end
    const ElfImports r = Read(elf);
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.undefined, (std::vector<std::string>{ "puts" }));
}

TEST_F(ElfImportsTest, NotAnElfIsAnError)
{
    EXPECT_NE(Read(std::string(100, 'x')).error, "");
    EXPECT_NE(Read("").error, "");
}

TEST_F(ElfImportsTest, A64BitOrBigEndianElfIsAnError)
{
    // Every ESP32 chip is 32-bit little endian; anything else is the wrong file.
    std::string elf64 = MakeElf({ { "printf", false } });
    elf64[4] = 2;
    EXPECT_NE(Read(elf64).error, "");

    std::string bigEndian = MakeElf({ { "printf", false } });
    bigEndian[5] = 2;
    EXPECT_NE(Read(bigEndian).error, "");
}

TEST_F(ElfImportsTest, ATruncatedFileDoesNotReadPastItsEnd)
{
    // Cut inside the section headers: the reader treats what is missing as
    // zeros rather than reading past the buffer.
    const std::string elf = MakeElf({ { "printf", false }, { "game_entry", true } });
    const ElfImports r = Read(elf.substr(0, elf.size() - 50));
    EXPECT_EQ(r.error, "");
}

TEST_F(ElfImportsTest, AMissingFileIsAnError)
{
    EXPECT_NE(ReadElfImports(m_Path.string() + ".absent").error, "");
}
