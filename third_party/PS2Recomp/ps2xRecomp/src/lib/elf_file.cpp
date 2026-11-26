// [gta-recomp] ps2recomp::elf::ElfFile (elf_file.h), written from the System V ABI's ELF32 layout.
#include "ps2recomp/elf_file.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace ps2recomp::elf
{
    namespace
    {
        uint16_t rd16(const uint8_t *p)
        {
            return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }

        uint32_t rd32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
    }

    bool ElfFile::load(const std::string &path)
    {
        m_error.clear();
        m_sections.clear();
        m_segments.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            m_error = "cannot open " + path;
            return false;
        }
        m_file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

        const uint8_t *f = m_file.data();
        if (m_file.size() < 52 || std::memcmp(f, "\x7f" "ELF", 4) != 0)
        {
            m_error = "not an ELF file";
            return false;
        }
        if (f[4] != 1 || f[5] != 1)
        {
            m_error = "not a 32-bit little-endian ELF";
            return false;
        }
        m_machine = rd16(f + 18);
        m_entry = rd32(f + 24);
        const uint32_t phoff = rd32(f + 28);
        const uint32_t shoff = rd32(f + 32);
        const uint16_t phentsize = rd16(f + 42);
        const uint16_t phnum = rd16(f + 44);
        const uint16_t shentsize = rd16(f + 46);
        const uint16_t shnum = rd16(f + 48);
        const uint16_t shstrndx = rd16(f + 50);

        if (phnum)
        {
            if (phentsize < 32 || !inFile(phoff, static_cast<uint64_t>(phentsize) * phnum))
            {
                m_error = "program headers outside the file";
                return false;
            }
            for (uint16_t i = 0; i < phnum; ++i)
            {
                const uint8_t *p = f + phoff + static_cast<size_t>(i) * phentsize;
                SegmentHeader s;
                s.type = rd32(p);
                s.offset = rd32(p + 4);
                s.virtualAddress = rd32(p + 8);
                s.fileSize = rd32(p + 16);
                s.memorySize = rd32(p + 20);
                s.flags = rd32(p + 24);
                if (s.fileSize && inFile(s.offset, s.fileSize))
                    s.data = f + s.offset;
                m_segments.push_back(s);
            }
        }

        if (shnum)
        {
            if (shentsize < 40 || !inFile(shoff, static_cast<uint64_t>(shentsize) * shnum))
            {
                m_error = "section headers outside the file";
                return false;
            }
            std::vector<uint32_t> nameOffsets;
            for (uint16_t i = 0; i < shnum; ++i)
            {
                const uint8_t *p = f + shoff + static_cast<size_t>(i) * shentsize;
                SectionHeader s;
                nameOffsets.push_back(rd32(p));
                s.type = rd32(p + 4);
                s.flags = rd32(p + 8);
                s.address = rd32(p + 12);
                s.offset = rd32(p + 16);
                s.size = rd32(p + 20);
                s.link = rd32(p + 24);
                s.info = rd32(p + 28);
                s.entrySize = rd32(p + 36);
                if (s.type != SHT_NOBITS && s.size && inFile(s.offset, s.size))
                    s.data = f + s.offset;
                m_sections.push_back(s);
            }
            if (shstrndx < m_sections.size())
                for (size_t i = 0; i < m_sections.size(); ++i)
                    m_sections[i].name = stringAt(m_sections[shstrndx], nameOffsets[i]);
        }
        return true;
    }

    std::string ElfFile::stringAt(const SectionHeader &table, uint32_t offset) const
    {
        if (!table.data || offset >= table.size)
            return {};
        const char *start = reinterpret_cast<const char *>(table.data) + offset;
        const void *end = std::memchr(start, 0, table.size - offset);
        return end ? std::string(start, static_cast<const char *>(end) - start) : std::string(start, table.size - offset);
    }

    std::vector<SymbolEntry> ElfFile::symbols(const SectionHeader &symbolSection) const
    {
        std::vector<SymbolEntry> out;
        if (!symbolSection.data || symbolSection.link >= m_sections.size())
            return out;
        const uint32_t entrySize = symbolSection.entrySize >= 16 ? symbolSection.entrySize : 16;
        const SectionHeader &names = m_sections[symbolSection.link];
        for (uint32_t pos = 0; symbolSection.size - pos >= 16 && pos + entrySize <= symbolSection.size; pos += entrySize)
        {
            const uint8_t *p = symbolSection.data + pos;
            SymbolEntry s;
            s.name = stringAt(names, rd32(p));
            s.value = rd32(p + 4);
            s.size = rd32(p + 8);
            s.bind = static_cast<uint8_t>(p[12] >> 4);
            s.type = static_cast<uint8_t>(p[12] & 0xF);
            s.other = p[13];
            s.sectionIndex = rd16(p + 14);
            out.push_back(std::move(s));
        }
        return out;
    }

    std::vector<RelocationEntry> ElfFile::relocations(const SectionHeader &relocationSection) const
    {
        std::vector<RelocationEntry> out;
        if (!relocationSection.data)
            return out;
        const bool rela = relocationSection.type == SHT_RELA;
        const uint32_t minimum = rela ? 12u : 8u;
        const uint32_t entrySize = relocationSection.entrySize >= minimum ? relocationSection.entrySize : minimum;
        for (uint32_t pos = 0; pos + entrySize <= relocationSection.size; pos += entrySize)
        {
            const uint8_t *p = relocationSection.data + pos;
            RelocationEntry r;
            r.offset = rd32(p);
            const uint32_t info = rd32(p + 4);
            r.symbol = info >> 8;
            r.type = info & 0xFF;
            r.addend = rela ? static_cast<int32_t>(rd32(p + 8)) : 0;
            out.push_back(r);
        }
        return out;
    }
}
