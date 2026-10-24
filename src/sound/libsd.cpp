// Sony's IOP sound library on the host - see sound/libsd.h. Each function follows LIBSD.IRX's code (module offsets
// in the comments; docs/game-internals.md 9 has how it was read).
#include "sound/libsd.h"

#include "ps2x/iop/iop_spu2.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sound
{
    namespace
    {
        // SPU2 register (offset from 0x1F900000) of each entry's param, by entry >> 8 (LIBSD.IRX data 0x4CA0)
        constexpr uint16_t kParamRegister[0x29] = {
            0x000, 0x002, 0x004, 0x006, 0x008, 0x00A, 0x00C, 0x00E, // voice VOLL VOLR PITCH ADSR1 ADSR2 ENVX VOLXL VOLXR
            0x198, 0x760, 0x762, 0x764, 0x766, 0x768, 0x76A, 0x76C, // MMIX MVOLL MVOLR EVOLL EVOLR AVOLL AVOLR BVOLL
            0x76E, 0x770, 0x772, 0x180, 0x184, 0x1A0, 0x1A4, 0x340, // BVOLR MVOLXL MVOLXR PMON NON KON KOFF ENDX
            0x188, 0x18C, 0x190, 0x194, 0x2E0, 0x33C, 0x1A8, 0x19C, // VMIXL VMIXEL VMIXR VMIXER ESA EEA TSA IRQA
            0x1C0, 0x1C4, 0x1C8, 0x19A, 0x1A8, 0x1AC, 0x1AE, 0x1B0, // SSA LSAX NAX ATTR TSA DATA - ADMAS
            0x344,                                                  // STATX
        };
        constexpr uint32_t kAttr = 0x19A;
        constexpr uint32_t kCore = 0x400; // core 1's registers
        // ATTR bit of sceSdSetCoreAttr's switches by (entry & 0xE): 2 effect enable, 4 IRQ enable, 6 mute (data 0x55E2)
        constexpr int kAttrBit[8] = {0, 7, 6, 14, 8, 0, 0, 0};
        constexpr int32_t kErrorInvalid = -100;   // 0xFFFFFF9C
        constexpr int32_t kErrorBusy = -210;      // 0xFFFFFF2E: a DMA is still running
        constexpr int32_t kErrorUnfinished = -201; // 0xFFFFFF37: the previous transfer was not reported done
    }

    LibSd::LibSd(ps2x::iop::detail::IopSpu2 &spu)
        : m_spu(spu)
    {
    }

    void LibSd::write(uint32_t offset, uint16_t value) { m_spu.write16(offset, value); }
    uint16_t LibSd::read(uint32_t offset) { return m_spu.read16(offset); }

    uint32_t LibSd::paramRegister(uint32_t entry) const
    {
        const uint32_t index = (entry >> 8) & 0xFF;
        return index < sizeof(kParamRegister) / sizeof(kParamRegister[0]) ? kParamRegister[index] : 0x7FEu;
    }

    // 0x39A0: base + core * (per-core volume ? 0x28 : 0x400) + voice * 0x10
    void LibSd::setParam(uint32_t entry, uint16_t value)
    {
        write(paramRegister(entry) + (entry & 1) * ((entry & 0x80) ? 0x28u : kCore) + (entry & 0x3E) * 8, value);
    }

    uint16_t LibSd::getParam(uint32_t entry) // 0x39FC
    {
        return read(paramRegister(entry) + (entry & 1) * ((entry & 0x80) ? 0x28u : kCore) + (entry & 0x3E) * 8);
    }

    void LibSd::setSwitch(uint32_t entry, uint32_t value) // 0x3A5C: 24 voice bits over two registers
    {
        const uint32_t r = paramRegister(entry) + (entry & 1) * kCore;
        write(r, static_cast<uint16_t>(value));
        write(r + 2, static_cast<uint16_t>((value >> 16) & 0xFF));
    }

    uint32_t LibSd::getSwitch(uint32_t entry) // 0x3A94
    {
        const uint32_t r = paramRegister(entry) + (entry & 1) * kCore;
        return read(r) | (static_cast<uint32_t>(read(r + 2)) << 16);
    }

    void LibSd::setAddr(uint32_t entry, uint32_t address) // 0x3ACC: voice * 0xC; EEA has only its high half
    {
        const uint32_t r = paramRegister(entry) + (entry & 1) * kCore + (entry & 0x3E) * 6;
        write(r, static_cast<uint16_t>(address >> 17));
        if ((entry & 0xFF00) != 0x1D00)
            write(r + 2, static_cast<uint16_t>((address >> 1) & 0xFFF8));
    }

    uint32_t LibSd::getAddr(uint32_t entry) // 0x3B30
    {
        const uint32_t r = paramRegister(entry) + (entry & 1) * kCore + (entry & 0x3E) * 6;
        const uint32_t high = static_cast<uint32_t>(read(r)) << 17;
        return high | ((entry & 0xFF00) == 0x1D00 ? 0x1FFFFu : static_cast<uint32_t>(read(r + 2)) << 1);
    }

    void LibSd::setCoreAttr(uint32_t entry, uint32_t value) // 0x3FEC
    {
        const uint32_t core = entry & 1;
        if ((entry & 0xFFFE) == 10) // SPDIF output mode (0x3E20)
        {
            const uint32_t mode = value & 0xFFFF;
            uint16_t out = read(0x7C0) & 0xFE57, media = read(0x7C6) & 0xFFF9;
            switch (mode & 0xF)
            {
            case 0: out |= 0x20; break;
            case 1: out |= 0x100; media |= 2; break;
            case 2: break;
            case 3: out |= 0x100; break;
            default: return;
            }
            media = (mode & 0x80) ? (media | 0x8000) : (media & 0x7FFF);
            const uint32_t rate = mode & 0xF00; // the stores in the IRX's order: 0x7C8, 0x7C0, 0x7C6
            write(0x7C8, rate == 0x400 ? 0 : 0x200);
            write(0x7C0, out);
            write(0x7C6, (media & 0xC0FF) | (rate == 0x800 ? 0x1900 : rate == 0x400 ? 0x100 : 0x900));
            m_spdif = mode;
            return;
        }
        const uint32_t attr = kAttr + core * kCore;
        if ((entry & 0xFFFE) == 8) // noise clock
        {
            write(attr, static_cast<uint16_t>((read(attr) & 0xC0FF) | ((value & 0x3F) << 8)));
            return;
        }
        const int bit = kAttrBit[(entry & 0xE) >> 1]; // 0x3F50
        write(attr, static_cast<uint16_t>((read(attr) & ~(1u << bit)) | ((value & 1u) << bit)));
    }

    uint32_t LibSd::getCoreAttr(uint32_t entry) // 0x40A0
    {
        const uint16_t attr = read(kAttr + (entry & 1) * kCore);
        switch (entry & 0xE)
        {
        case 2: return (attr >> 7) & 1;
        case 4: return (attr >> 6) & 1;
        case 6: return (attr >> 14) & 1;
        case 8: return (attr >> 8) & 0x3F;
        case 10: return m_spdif & 0xFFFF;
        default: return 0;
        }
    }

    // ---- transfers ---------------------------------------------------------------------------------------------
    int32_t LibSd::voiceTrans(uint32_t channel, uint32_t mode, const uint8_t *data, uint32_t available, uint32_t spuAddress,
                              uint32_t bytes)
    {
        channel &= 1;
        if (bytes == 0)
            return kErrorInvalid;
        const uint32_t base = channel * kCore;
        if ((read(kAttr + base) & 0x30) != 0 || read(0x1B0 + base) != 0)
            return kErrorBusy;
        if (m_transDone[channel] != 1)
            return kErrorUnfinished;
        if ((mode & 3) != 0 || (mode & 8) != 0)
        {
            static int reported = 0;
            if (reported++ < 8)
                std::fprintf(stderr, "[libsd] VoiceTrans mode 0x%x is not implemented natively\n", mode);
            return kErrorInvalid;
        }
        // TSA, then a DMA write (0x1890 -> 0x2358 cases 0 and 4)
        write(0x1AA + base, static_cast<uint16_t>((spuAddress >> 1) & 0xFFF8));
        write(0x1A8 + base, static_cast<uint16_t>(spuAddress >> 17));
        m_transDone[channel] = 0;
        const uint32_t rounded = (bytes + 63u) & ~63u;
        write(kAttr + base, static_cast<uint16_t>((read(kAttr + base) & 0xFFCF) | 0x20));
        m_padded.assign(data, data + std::min(std::max(available, bytes), rounded));
        m_padded.resize(rounded, 0);
        // the data is in SPU2 RAM now; the DMA counts as running for as long as the core says it takes
        m_dmaDue[channel] = m_now + m_spu.dmaTransfer(channel, m_padded.data(), rounded, 0, rounded, true);
        m_dmaBusy[channel] = true;
        return static_cast<int32_t>(rounded);
    }

    void LibSd::completeTransfer(uint32_t channel) // the completion interrupt (0x29EC)
    {
        if (!m_dmaBusy[channel])
            return;
        m_dmaBusy[channel] = false;
        write(kAttr + channel * kCore, static_cast<uint16_t>(read(kAttr + channel * kCore) & 0xFFCF)); // DMA mode off
        if (m_transCallback[channel])
        {
            m_transDone[channel] = 1;
            m_completed.push_back(channel);
        }
        else
            m_transPending[channel] = 1; // reported by the next status call
    }

    void LibSd::advance(uint64_t iopCycle)
    {
        m_now = iopCycle;
        for (uint32_t channel = 0; channel < 2; ++channel)
            if (m_dmaBusy[channel] && m_dmaDue[channel] <= iopCycle)
                completeTransfer(channel);
    }

    uint64_t LibSd::nextEvent() const
    {
        uint64_t next = ~0ull;
        for (uint32_t channel = 0; channel < 2; ++channel)
            if (m_dmaBusy[channel])
                next = std::min(next, m_dmaDue[channel]);
        return next;
    }

    int32_t LibSd::voiceTransStatus(uint32_t channel, uint32_t flag) // 0x1B10
    {
        channel &= 1;
        if (flag == 1) // waits for the interrupt: here the transfer is simply finished now
            completeTransfer(channel);
        if (m_transDone[channel] == 1)
            return 1;
        if (flag == 1 || (flag == 0 && m_transPending[channel]))
        {
            m_transPending[channel] = 0;
            m_transDone[channel] = 1;
        }
        return m_transDone[channel];
    }

    void LibSd::setTransCallback(uint32_t channel, Callback callback) { m_transCallback[channel & 1] = std::move(callback); }
    void LibSd::setIrqCallback(Callback callback) { m_irqCallback = std::move(callback); }

    bool LibSd::flushCallbacks()
    {
        bool ran = false;
        while (!m_completed.empty())
        {
            const uint32_t channel = m_completed.front();
            m_completed.pop_front();
            if (m_transCallback[channel])
                m_transCallback[channel](channel);
            ran = true;
        }
        return ran;
    }

    void LibSd::serviceIrq() // 0x4220
    {
        if (!m_irqCallback)
            return;
        for (uint32_t cores; (cores = (read(0x7C2) & 0xC) >> 2) != 0;)
        {
            if (cores & 1)
                write(kAttr, read(kAttr) & 0xFFBF);
            if (cores & 2)
                write(kAttr + kCore, read(kAttr + kCore) & 0xFFBF);
            m_irqCallback(0);
        }
    }

    // ---- sceSdInit (0x48E4) --------------------------------------------------------------------------------------
    void LibSd::resetHardware() // 0x206C, flag 0 only
    {
        write(0x7C0, 0);
        write(0x7C0, 0x8000);
        for (uint32_t r : {0x760u, 0x762u, 0x788u, 0x78Au})
            write(r, 0);
        write(0x1B0, 0);
        write(0x1B0 + kCore, 0);
        write(kAttr, 0);
        write(kAttr + kCore, 0);
        write(kAttr, 0x8000);
        write(kAttr + kCore, 0x8000);
        for (uint32_t r : {0x760u, 0x762u, 0x788u, 0x78Au})
            write(r, 0);
        for (uint32_t base : {0u, kCore})
        {
            write(0x1A4 + base, 0xFFFF); // all voices off
            write(0x1A6 + base, 0xFF);
        }
        for (uint32_t r : {0x180u, 0x182u, 0x580u, 0x582u, 0x184u, 0x186u, 0x584u, 0x586u}) // no pitch modulation, no noise
            write(r, 0);
    }

    void LibSd::resetVoices() // 0x4344
    {
        // a silent looping block at 0x5000 for idle voices: written through the data port (TSA 0x2800 halfwords)
        write(kAttr, read(kAttr) & 0xFFCF);
        write(0x1A8, 0);
        write(0x1AA, 0x2800);
        static constexpr uint16_t kSilentLoop[8] = {0x0700, 0, 0, 0, 0, 0, 0, 0};
        for (uint16_t v : kSilentLoop)
            write(0x1AC, v);
        write(kAttr, static_cast<uint16_t>((read(kAttr) & 0xFFCF) | 0x10)); // manual transfer
        write(kAttr, static_cast<uint16_t>(read(kAttr) & 0xFFCF));
        for (uint32_t voice = 0; voice < 24; ++voice) // each register core 1 then core 0, as the IRX writes them
        {
            static constexpr uint16_t kVoice[5] = {0, 0, 0x3FFF, 0, 0}; // VOLL VOLR PITCH ADSR1 ADSR2
            for (uint32_t i = 0; i < 5; ++i)
            {
                write(kCore + voice * 0x10 + i * 2, kVoice[i]);
                write(voice * 0x10 + i * 2, kVoice[i]);
            }
            for (uint32_t i = 0; i < 2; ++i) // SSA = 0x5000, the silent loop
            {
                write(kCore + 0x1C0 + voice * 0xC + i * 2, i ? 0x2800 : 0);
                write(0x1C0 + voice * 0xC + i * 2, i ? 0x2800 : 0);
            }
        }
        for (uint32_t base : {kCore, 0u})
            write(0x1A0 + base, 0xFFFF);
        for (uint32_t base : {kCore, 0u})
            write(0x1A2 + base, 0xFF);
        for (uint32_t base : {kCore, 0u})
            write(0x1A4 + base, 0xFFFF);
        for (uint32_t base : {kCore, 0u})
            write(0x1A6 + base, 0xFF);
        for (uint32_t r : {0x342u, 0x340u, 0x742u, 0x740u}) // ENDX
            write(r, 0);
    }

    void LibSd::defaultSettings(uint32_t flag) // 0x1638
    {
        write(0x7C0, 0xC032);
        write(kAttr, flag == 0 ? 0xC000 : 0xC080);
        write(kAttr + kCore, flag == 0 ? 0xC001 : 0xC081);
        for (uint32_t base : {0u, kCore}) // every voice to the dry and wet mixes
            for (uint32_t r : {0x188u, 0x190u, 0x18Cu, 0x194u})
            {
                write(r + base, 0xFFFF);
                write(r + base + 2, 0xFF);
            }
        write(0x198, 0xFF0);
        write(0x198 + kCore, 0xFFC);
        if (flag == 0)
        {
            for (uint32_t r : {0x760u, 0x762u, 0x788u, 0x78Au, 0x764u, 0x766u, 0x78Cu, 0x78Eu})
                write(r, 0);
            write(0x33C, 0xE);
            write(0x33C + kCore, 0xF);
        }
        write(0x768, 0); // core 0's input from core 1 / external: off; core 1 takes core 0 at full volume
        write(0x76A, 0);
        write(0x790, 0x7FFF);
        write(0x792, 0x7FFF);
        write(0x76C, 0);
        write(0x76E, 0);
        write(0x794, 0);
        write(0x796, 0);
    }

    int32_t LibSd::init(uint32_t flag)
    {
        // 0x15E8: SPDIF defaults
        write(0x7C6, 0x900);
        write(0x7C8, 0x200);
        write(0x7CA, 8);
        if ((flag & 0xF) == 0)
            resetHardware();
        // 0x4644: library state; with flag 0 the effect work areas end at the top of each core's half
        m_transDone[0] = m_transDone[1] = 1;
        m_transPending[0] = m_transPending[1] = 0;
        m_dmaBusy[0] = m_dmaBusy[1] = false;
        m_transCallback[0] = m_transCallback[1] = nullptr;
        m_irqCallback = nullptr;
        m_completed.clear();
        if ((flag & 0xF) == 0)
        {
            m_spdif = 0;
            write(0x2E0, 0xE);
            write(0x2E2, 0xFFF8);
            write(0x2E0 + kCore, 0xF);
            write(0x2E2 + kCore, 0xFFF8);
        }
        resetVoices();
        defaultSettings(flag & 0xF);
        return 0;
    }
}
