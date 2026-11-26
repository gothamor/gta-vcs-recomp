#ifndef PS2RECOMP_ELF_PARSER_H
#define PS2RECOMP_ELF_PARSER_H

#include <cstdint>
#include "ps2recomp/elf_file.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

namespace ps2recomp
{
        struct Relocation;
        struct Section;
        struct Function;
        struct Symbol;
        class RecompilerReporter;

        class ElfParser
        {
        public:
                explicit ElfParser(const std::string &filePath);
                ~ElfParser();

                bool parse();

                bool loadGhidraFunctionMap(const std::string &mapPath);
                std::vector<Function> extractFunctions() const;
                std::vector<Function> extractExtraFunctions() const;
                std::vector<Symbol> extractSymbols();
                std::vector<Section> getSections();
                std::vector<Relocation> getRelocations();

                // Helper methods
                bool isValidAddress(uint32_t address) const;
                uint32_t readWord(uint32_t address) const;
                uint8_t *getSectionData(const std::string &sectionName) const;
                uint32_t getSectionAddress(const std::string &sectionName) const;
                uint32_t getSectionSize(const std::string &sectionName) const;
                uint32_t getEntryPoint() const;
                void setReporter(RecompilerReporter *reporter);
                void debugAddress(uint32_t address) const;

        private:
                std::string m_filePath;
                std::unique_ptr<elf::ElfFile> m_elf; // [gta-recomp] our own reader, not ELFIO

                std::vector<Section> m_sections;
                std::vector<Symbol> m_symbols;
                std::vector<Relocation> m_relocations;
                std::vector<Function> m_extraFunctions;
                bool m_hasLoadedGhidraMap = false;
                RecompilerReporter *m_reporter = nullptr;
                std::unordered_set<uint32_t> m_ghidraMapStarts;

                void loadSections();
                void loadSymbols();
                void loadRelocations();
                void loadDebugFunctions();
                bool isExecutableSection(const elf::SectionHeader &section) const;
                bool isDataSection(const elf::SectionHeader &section) const;
        };

} // namespace ps2recomp

#endif // PS2RECOMP_ELF_PARSER_H
