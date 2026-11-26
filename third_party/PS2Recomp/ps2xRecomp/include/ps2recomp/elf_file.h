#ifndef PS2RECOMP_ELF_FILE_H
#define PS2RECOMP_ELF_FILE_H

// [gta-recomp] A 32-bit little-endian ELF reader for the recompiler, in place of the ELFIO library (DECISIONS.md
// 2026-11-27): the file header, section and program headers, string and symbol tables, REL / RELA entries - what
// ElfParser reads. Every offset is checked against the file; a header that points outside it makes load() fail,
// and section / segment data that does is left out (data() is null).

#include <cstdint>
#include <string>
#include <vector>

namespace ps2recomp::elf
{
    constexpr uint16_t EM_MIPS = 8;

    constexpr uint32_t SHT_NULL = 0;
    constexpr uint32_t SHT_SYMTAB = 2;
    constexpr uint32_t SHT_RELA = 4;
    constexpr uint32_t SHT_NOBITS = 8;
    constexpr uint32_t SHT_REL = 9;
    constexpr uint32_t SHT_DYNSYM = 11;

    constexpr uint32_t SHF_WRITE = 0x1;
    constexpr uint32_t SHF_ALLOC = 0x2;
    constexpr uint32_t SHF_EXECINSTR = 0x4;

    constexpr uint32_t PT_LOAD = 1;
    constexpr uint32_t PF_X = 0x1;
    constexpr uint32_t PF_W = 0x2;
    constexpr uint32_t PF_R = 0x4;

    constexpr uint16_t SHN_UNDEF = 0;
    constexpr uint8_t STB_GLOBAL = 1;
    constexpr uint8_t STT_FUNC = 2;

    struct SectionHeader
    {
        std::string name;
        uint32_t type = 0;
        uint32_t flags = 0;
        uint32_t address = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t link = 0;
        uint32_t info = 0;
        uint32_t entrySize = 0;
        const uint8_t *data = nullptr; // into the file; null for NOBITS, empty or out-of-file sections
    };

    struct SegmentHeader
    {
        uint32_t type = 0;
        uint32_t flags = 0;
        uint32_t offset = 0;
        uint32_t virtualAddress = 0;
        uint32_t fileSize = 0;
        uint32_t memorySize = 0;
        const uint8_t *data = nullptr; // fileSize bytes into the file, or null
    };

    struct SymbolEntry
    {
        std::string name;
        uint32_t value = 0;
        uint32_t size = 0;
        uint8_t bind = 0;
        uint8_t type = 0;
        uint8_t other = 0;
        uint16_t sectionIndex = 0;
    };

    struct RelocationEntry
    {
        uint32_t offset = 0;
        uint32_t symbol = 0;
        uint32_t type = 0;
        int32_t addend = 0; // 0 for REL
    };

    class ElfFile
    {
    public:
        // false (and error() says why) for a file that can't be read or isn't a 32-bit little-endian ELF
        bool load(const std::string &path);
        const std::string &error() const { return m_error; }

        uint16_t machine() const { return m_machine; }
        uint32_t entry() const { return m_entry; }
        const std::vector<SectionHeader> &sections() const { return m_sections; } // index 0 is the null section
        const std::vector<SegmentHeader> &segments() const { return m_segments; }

        // the entries of a SYMTAB / DYNSYM section, names from its linked string table
        std::vector<SymbolEntry> symbols(const SectionHeader &symbolSection) const;
        // the entries of a REL / RELA section
        std::vector<RelocationEntry> relocations(const SectionHeader &relocationSection) const;

    private:
        std::vector<uint8_t> m_file;
        std::string m_error;
        uint16_t m_machine = 0;
        uint32_t m_entry = 0;
        std::vector<SectionHeader> m_sections;
        std::vector<SegmentHeader> m_segments;

        bool inFile(uint64_t offset, uint64_t size) const { return offset <= m_file.size() && size <= m_file.size() - offset; }
        std::string stringAt(const SectionHeader &table, uint32_t offset) const;
    };
}

#endif // PS2RECOMP_ELF_FILE_H
