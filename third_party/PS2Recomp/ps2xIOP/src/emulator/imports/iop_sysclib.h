#pragma once

#include <functional>
#include <string>

#include <cstdint>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopMemory;

    class IopSysclib
    {
    public:
        explicit IopSysclib(IopMemory &memory) noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);

    private:
        std::string formatGuest(const std::string &format, const std::function<uint32_t()> &nextArg) const; // [gta-recomp]
        IopMemory &m_memory;
    };
}
