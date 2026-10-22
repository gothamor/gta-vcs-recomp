// [gta-recomp] SPU2 emulation, see iop_spu2.h. Register map, ADPCM, ADSR and AutoDMA layout follow the public
// PS1 SPU / PS2 SPU2 documentation (psx-spx, PCSX2's SPU2 notes).
#include "iop_spu2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr int32_t kFilterPos[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kFilterNeg[5] = {0, 0, -52, -55, -60};
        constexpr uint32_t kAddrMask = IopSpu2::kRamHalfwords - 1u;

        int16_t clamp16(int32_t v)
        {
            return static_cast<int16_t>(std::clamp(v, -32768, 32767));
        }

        uint32_t setHigh(uint32_t addr, uint16_t value) { return ((static_cast<uint32_t>(value) & 0xFu) << 16) | (addr & 0xFFFFu); }
        uint32_t setLow(uint32_t addr, uint16_t value) { return (addr & 0xF0000u) | value; }
    }

    IopSpu2::IopSpu2()
    {
        reset();
    }

    void IopSpu2::reset()
    {
        m_ram.assign(kRamHalfwords, 0u);
        m_cores = {};
        for (Core &c : m_cores)
            c.statx = 0x80;
        m_irqInfo = 0;
        std::memset(m_spdif, 0, sizeof(m_spdif));
        m_irqPending = false;
        m_cycle = 0;
        m_nextSampleCycle = kCyclesPerSample;
        m_outBuffer.clear();
    }

    int16_t IopSpu2::fixedVolume(uint16_t reg, int16_t &current)
    {
        if ((reg & 0x8000u) == 0u)
        {
            current = static_cast<int16_t>(reg << 1);
            return current;
        }
        // Sweep mode (approximation): ramp toward full scale or zero.
        const bool decrease = (reg & 0x2000u) != 0u;
        const int32_t next = decrease ? current - 128 : current + 128;
        current = static_cast<int16_t>(std::clamp(next, 0, 0x7FFF));
        return current;
    }

    void IopSpu2::checkIrq(uint32_t lo, uint32_t hi)
    {
        for (uint32_t i = 0; i < 2; ++i)
        {
            const Core &c = m_cores[i];
            if ((c.attr & 0x40u) != 0u && c.irqa >= lo && c.irqa < hi)
            {
                m_irqInfo |= static_cast<uint16_t>(4u << i);
                m_irqPending = true;
                ++m_stats.irqs;
            }
        }
    }

    // ---- registers ------------------------------------------------------------------------------------------------
    uint16_t IopSpu2::read16(uint32_t offset)
    {
        offset &= kRegSize - 1u;
        static const bool trace = std::getenv("PS2X_TRACE_SPU") != nullptr;
        static int traced = 0;
        if (trace && traced++ < 40000)
            std::fprintf(stderr, "[spu2] r 0x%03x\n", offset);
        if (offset >= 0x760u && offset < 0x7B0u)
        {
            const uint32_t core = (offset - 0x760u) / 0x28u;
            const uint32_t reg = (offset - 0x760u) % 0x28u;
            const Core &c = m_cores[core];
            switch (reg)
            {
            case 0x00: return c.mvolL;
            case 0x02: return c.mvolR;
            case 0x04: return c.evolL;
            case 0x06: return c.evolR;
            case 0x08: return c.avolL;
            case 0x0A: return c.avolR;
            case 0x0C: return c.bvolL;
            case 0x0E: return c.bvolR;
            case 0x10: return static_cast<uint16_t>(c.curMvolL);
            case 0x12: return static_cast<uint16_t>(c.curMvolR);
            default: return c.raw[(0x160u + reg) / 2u];
            }
        }
        if (offset >= 0x7C0u)
        {
            if (offset == 0x7C2u)
                return m_irqInfo;
            return m_spdif[((offset - 0x7C0u) / 2u) & 7u];
        }
        return readCore(offset >> 10, offset & 0x3FFu);
    }

    void IopSpu2::write16(uint32_t offset, uint16_t value)
    {
        offset &= kRegSize - 1u;
        static const bool trace = std::getenv("PS2X_TRACE_SPU") != nullptr;
        static int traced = 0;
        if (trace && traced++ < 40000)
            std::fprintf(stderr, "[spu2] w 0x%03x = 0x%04x\n", offset, value);
        if (offset >= 0x760u && offset < 0x7B0u)
        {
            const uint32_t core = (offset - 0x760u) / 0x28u;
            const uint32_t reg = (offset - 0x760u) % 0x28u;
            Core &c = m_cores[core];
            switch (reg)
            {
            case 0x00: c.mvolL = value; break;
            case 0x02: c.mvolR = value; break;
            case 0x04: c.evolL = value; break;
            case 0x06: c.evolR = value; break;
            case 0x08: c.avolL = value; break;
            case 0x0A: c.avolR = value; break;
            case 0x0C: c.bvolL = value; break;
            case 0x0E: c.bvolR = value; break;
            default: c.raw[(0x160u + reg) / 2u] = value; break;
            }
            return;
        }
        if (offset >= 0x7C0u)
        {
            if (offset == 0x7C2u)
                m_irqInfo = value;
            else
                m_spdif[((offset - 0x7C0u) / 2u) & 7u] = value;
            return;
        }
        writeCore(offset >> 10, offset & 0x3FFu, value);
    }

    uint16_t IopSpu2::readCore(uint32_t core, uint32_t reg)
    {
        Core &c = m_cores[core];
        if (reg < 0x180u)
        {
            const Voice &v = c.voices[reg >> 4];
            switch ((reg & 0xFu) >> 1)
            {
            case 0: return v.volL;
            case 1: return v.volR;
            case 2: return v.pitch;
            case 3: return v.adsr1;
            case 4: return v.adsr2;
            case 5: return static_cast<uint16_t>(v.level);
            case 6: return static_cast<uint16_t>(v.curVolL);
            default: return static_cast<uint16_t>(v.curVolR);
            }
        }
        if (reg >= 0x1C0u && reg < 0x1C0u + 24u * 0xCu)
        {
            const Voice &v = c.voices[(reg - 0x1C0u) / 0xCu];
            switch (((reg - 0x1C0u) % 0xCu) >> 1)
            {
            case 0: return static_cast<uint16_t>(v.ssa >> 16);
            case 1: return static_cast<uint16_t>(v.ssa);
            case 2: return static_cast<uint16_t>(v.lsa >> 16);
            case 3: return static_cast<uint16_t>(v.lsa);
            case 4: return static_cast<uint16_t>(v.nax >> 16);
            default: return static_cast<uint16_t>(v.nax);
            }
        }
        switch (reg)
        {
        case 0x180: return static_cast<uint16_t>(c.pmon);
        case 0x182: return static_cast<uint16_t>(c.pmon >> 16);
        case 0x184: return static_cast<uint16_t>(c.non);
        case 0x186: return static_cast<uint16_t>(c.non >> 16);
        case 0x188: return static_cast<uint16_t>(c.vmixL);
        case 0x18A: return static_cast<uint16_t>(c.vmixL >> 16);
        case 0x18C: return static_cast<uint16_t>(c.vmixEL);
        case 0x18E: return static_cast<uint16_t>(c.vmixEL >> 16);
        case 0x190: return static_cast<uint16_t>(c.vmixR);
        case 0x192: return static_cast<uint16_t>(c.vmixR >> 16);
        case 0x194: return static_cast<uint16_t>(c.vmixER);
        case 0x196: return static_cast<uint16_t>(c.vmixER >> 16);
        case 0x198: return c.mmix;
        case 0x19A: return c.attr;
        case 0x19C: return static_cast<uint16_t>(c.irqa >> 16);
        case 0x19E: return static_cast<uint16_t>(c.irqa);
        case 0x1A0: return static_cast<uint16_t>(c.konLatch);
        case 0x1A2: return static_cast<uint16_t>(c.konLatch >> 16);
        case 0x1A4: return static_cast<uint16_t>(c.kofLatch);
        case 0x1A6: return static_cast<uint16_t>(c.kofLatch >> 16);
        case 0x1A8: return static_cast<uint16_t>(c.tsa >> 16);
        case 0x1AA: return static_cast<uint16_t>(c.tsa);
        case 0x1AC:
        {
            const uint16_t v = m_ram[c.tsa];
            c.tsa = (c.tsa + 1u) & kAddrMask;
            return v;
        }
        case 0x1B0: return c.admas;
        case 0x340: return static_cast<uint16_t>(c.endx);
        case 0x342: return static_cast<uint16_t>(c.endx >> 16);
        case 0x344:
            // bit 7: transfer ready, only while ATTR selects a transfer mode (bits 4-5); otherwise idle (0).
            // sceSdInit waits for (STATX & 0x7FF) == 0 after resetting a core; transfers wait for bit 7.
            return static_cast<uint16_t>((c.attr & 0x30u) != 0u ? 0x80u : 0u);
        default: return c.raw[(reg >> 1) & 0x1FFu];
        }
    }

    void IopSpu2::writeCore(uint32_t core, uint32_t reg, uint16_t value)
    {
        Core &c = m_cores[core];
        c.raw[(reg >> 1) & 0x1FFu] = value;
        if (reg < 0x180u)
        {
            Voice &v = c.voices[reg >> 4];
            switch ((reg & 0xFu) >> 1)
            {
            case 0: v.volL = value; break;
            case 1: v.volR = value; break;
            case 2: v.pitch = value; break;
            case 3: v.adsr1 = value; break;
            case 4: v.adsr2 = value; break;
            case 5: v.level = static_cast<int16_t>(value); break;
            default: break;
            }
            return;
        }
        if (reg >= 0x1C0u && reg < 0x1C0u + 24u * 0xCu)
        {
            Voice &v = c.voices[(reg - 0x1C0u) / 0xCu];
            switch (((reg - 0x1C0u) % 0xCu) >> 1)
            {
            case 0: v.ssa = setHigh(v.ssa, value); break;
            case 1: v.ssa = setLow(v.ssa, value); break;
            case 2: v.lsa = setHigh(v.lsa, value); v.lsaSetBySoftware = true; break;
            case 3: v.lsa = setLow(v.lsa, value); v.lsaSetBySoftware = true; break;
            case 4: v.nax = setHigh(v.nax, value); break;
            default: v.nax = setLow(v.nax, value); break;
            }
            return;
        }
        switch (reg)
        {
        case 0x180: c.pmon = (c.pmon & 0xFF0000u) | value; break;
        case 0x182: c.pmon = (c.pmon & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x184: c.non = (c.non & 0xFF0000u) | value; break;
        case 0x186: c.non = (c.non & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x188: c.vmixL = (c.vmixL & 0xFF0000u) | value; break;
        case 0x18A: c.vmixL = (c.vmixL & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x18C: c.vmixEL = (c.vmixEL & 0xFF0000u) | value; break;
        case 0x18E: c.vmixEL = (c.vmixEL & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x190: c.vmixR = (c.vmixR & 0xFF0000u) | value; break;
        case 0x192: c.vmixR = (c.vmixR & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x194: c.vmixER = (c.vmixER & 0xFF0000u) | value; break;
        case 0x196: c.vmixER = (c.vmixER & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x198: c.mmix = value; break;
        case 0x19A:
            c.attr = value;
            if ((value & 0x40u) == 0u)
                m_irqInfo &= static_cast<uint16_t>(~(4u << core));
            break;
        case 0x19C: c.irqa = setHigh(c.irqa, value); break;
        case 0x19E: c.irqa = setLow(c.irqa, value); break;
        case 0x1A0: c.konLatch = value; keyOn(c, core, value); break;
        case 0x1A2: c.konLatch = static_cast<uint32_t>(value) << 16; keyOn(c, core, (static_cast<uint32_t>(value) & 0xFFu) << 16); break;
        case 0x1A4: c.kofLatch = value; keyOff(c, value); break;
        case 0x1A6: c.kofLatch = static_cast<uint32_t>(value) << 16; keyOff(c, (static_cast<uint32_t>(value) & 0xFFu) << 16); break;
        case 0x1A8: c.tsa = setHigh(c.tsa, value); break;
        case 0x1AA: c.tsa = setLow(c.tsa, value); break;
        case 0x1AC:
            m_ram[c.tsa] = value;
            checkIrq(c.tsa, c.tsa + 1u);
            c.tsa = (c.tsa + 1u) & kAddrMask;
            break;
        case 0x1B0:
            c.admas = value;
            if (value == 0u)
                c.adma.clear();
            break;
        case 0x340:
        case 0x342: c.endx = 0; break; // writing ENDX clears it
        case 0x344: break;             // STATX is read-only here (always ready)
        default: break;
        }
    }

    void IopSpu2::keyOn(Core &c, uint32_t coreIndex, uint32_t mask)
    {
        for (uint32_t i = 0; i < 24; ++i)
        {
            if ((mask & (1u << i)) == 0u)
                continue;
            Voice &v = c.voices[i];
            ++m_stats.keyOns;
            v.nax = v.ssa & kAddrMask;
            v.counter = 0;
            v.hist1 = v.hist2 = 0;
            v.prevSample = 0;
            v.blockValid = false;
            v.lsaSetBySoftware = false;
            v.phase = Voice::Phase::Attack;
            v.level = 0;
            v.envCounter = 0;
            c.endx &= ~(1u << i);
            decodeBlock(c, coreIndex, v);
        }
    }

    void IopSpu2::keyOff(Core &c, uint32_t mask)
    {
        for (uint32_t i = 0; i < 24; ++i)
            if ((mask & (1u << i)) != 0u && c.voices[i].phase != Voice::Phase::Off)
            {
                c.voices[i].phase = Voice::Phase::Release;
                c.voices[i].envCounter = 0;
            }
    }

    // ---- voices ---------------------------------------------------------------------------------------------------
    void IopSpu2::decodeBlock(Core &c, uint32_t coreIndex, Voice &v)
    {
        (void)coreIndex;
        const uint32_t addr = v.nax & kAddrMask;
        checkIrq(addr, addr + 8u);
        const uint16_t header = m_ram[addr];
        uint32_t shift = header & 0xFu;
        if (shift > 12u)
            shift = 9u;
        const uint32_t filter = std::min<uint32_t>((header >> 4) & 0x7u, 4u);
        v.blockFlags = static_cast<uint8_t>(header >> 8);
        v.blockAddr = addr;
        if ((v.blockFlags & 0x4u) != 0u && !v.lsaSetBySoftware)
            v.lsa = addr;
        for (uint32_t i = 0; i < 28; ++i)
        {
            const uint16_t word = m_ram[(addr + 1u + i / 4u) & kAddrMask];
            const int32_t nibble = static_cast<int16_t>(static_cast<uint16_t>(((word >> ((i & 3u) * 4u)) & 0xFu) << 12)) >> shift;
            int32_t s = nibble + ((v.hist1 * kFilterPos[filter] + v.hist2 * kFilterNeg[filter] + 32) >> 6);
            s = clamp16(s);
            v.hist2 = v.hist1;
            v.hist1 = s;
            v.samples[i] = static_cast<int16_t>(s);
        }
        v.blockValid = true;
        (void)c;
    }

    void IopSpu2::stepEnvelope(Voice &v)
    {
        int32_t rate = 0;
        bool exponential = false, decrease = false;
        switch (v.phase)
        {
        case Voice::Phase::Off:
            v.level = 0;
            return;
        case Voice::Phase::Attack:
            rate = (v.adsr1 >> 8) & 0x7F;
            exponential = (v.adsr1 & 0x8000u) != 0u;
            break;
        case Voice::Phase::Decay:
            rate = ((v.adsr1 >> 4) & 0xF) << 2;
            exponential = true;
            decrease = true;
            break;
        case Voice::Phase::Sustain:
            rate = (v.adsr2 >> 6) & 0x7F;
            exponential = (v.adsr2 & 0x8000u) != 0u;
            decrease = (v.adsr2 & 0x4000u) != 0u;
            break;
        case Voice::Phase::Release:
            rate = (v.adsr2 & 0x1F) << 2;
            exponential = (v.adsr2 & 0x20u) != 0u;
            decrease = true;
            break;
        }
        const int32_t shift = rate >> 2;
        int32_t step = decrease ? (-8 + (rate & 3)) : (7 - (rate & 3));
        uint32_t cycles = 1u << std::max(0, shift - 11);
        step <<= std::max(0, 11 - shift);
        if (exponential && !decrease && v.level > 0x6000)
            cycles *= 4u;
        if (exponential && decrease)
            step = (step * v.level) >> 15;
        if (v.envCounter > 0u)
            --v.envCounter;
        if (v.envCounter == 0u)
        {
            v.envCounter = cycles;
            v.level = std::clamp(v.level + step, 0, 0x7FFF);
        }
        switch (v.phase)
        {
        case Voice::Phase::Attack:
            if (v.level >= 0x7FFF)
            {
                v.phase = Voice::Phase::Decay;
                v.envCounter = 0;
            }
            break;
        case Voice::Phase::Decay:
        {
            const int32_t sustain = std::min<int32_t>(((v.adsr1 & 0xF) + 1) * 0x800, 0x7FFF);
            if (v.level <= sustain)
            {
                v.phase = Voice::Phase::Sustain;
                v.envCounter = 0;
            }
            break;
        }
        case Voice::Phase::Release:
            if (v.level <= 0)
                v.phase = Voice::Phase::Off;
            break;
        default:
            break;
        }
    }

    int32_t IopSpu2::voiceSample(Core &c, uint32_t coreIndex, uint32_t voiceIndex)
    {
        Voice &v = c.voices[voiceIndex];
        if (v.phase == Voice::Phase::Off)
            return 0;
        if (!v.blockValid)
            decodeBlock(c, coreIndex, v);
        const uint32_t index = v.counter >> 12;
        const int32_t frac = static_cast<int32_t>(v.counter & 0xFFFu);
        const int32_t s0 = v.samples[std::min<uint32_t>(index, 27u)];
        const int32_t s1 = index + 1u < 28u ? v.samples[index + 1u] : s0;
        const int32_t sample = s0 + (((s1 - s0) * frac) >> 12);
        stepEnvelope(v);
        const int32_t out = (sample * v.level) >> 15;

        v.counter += std::min<uint32_t>(v.pitch, 0x3FFFu);
        while (v.counter >= (28u << 12))
        {
            v.counter -= 28u << 12;
            const uint8_t flags = v.blockFlags;
            if ((flags & 0x1u) != 0u)
            {
                c.endx |= 1u << voiceIndex;
                if ((flags & 0x2u) != 0u)
                    v.nax = v.lsa & kAddrMask;
                else
                {
                    v.phase = Voice::Phase::Off;
                    v.level = 0;
                    v.nax = v.lsa & kAddrMask;
                }
            }
            else
            {
                v.nax = (v.blockAddr + 8u) & kAddrMask;
            }
            decodeBlock(c, coreIndex, v);
        }
        return out;
    }

    void IopSpu2::mixOne(int16_t &outL, int16_t &outR)
    {
        int32_t core0L = 0, core0R = 0;
        for (uint32_t ci = 0; ci < 2; ++ci)
        {
            Core &c = m_cores[ci];
            int32_t dryL = 0, dryR = 0;
            for (uint32_t vi = 0; vi < 24; ++vi)
            {
                Voice &v = c.voices[vi];
                if (v.phase == Voice::Phase::Off)
                    continue;
                const int32_t s = voiceSample(c, ci, vi);
                const int32_t l = (s * fixedVolume(v.volL, v.curVolL)) >> 15;
                const int32_t r = (s * fixedVolume(v.volR, v.curVolR)) >> 15;
                if (c.vmixL & (1u << vi))
                    dryL += l;
                if (c.vmixR & (1u << vi))
                    dryR += r;
            }
            int32_t mixL = (c.mmix & 0x800u) ? dryL : 0;
            int32_t mixR = (c.mmix & 0x400u) ? dryR : 0;
            // AutoDMA input
            if (!c.adma.empty())
            {
                const auto [inL, inR] = c.adma.front();
                c.adma.pop_front();
                const uint32_t base = 0x2000u + ci * 0x400u;
                checkIrq(base + c.admaReadPos, base + c.admaReadPos + 1u);
                checkIrq(base + 0x200u + c.admaReadPos, base + 0x200u + c.admaReadPos + 1u);
                c.admaReadPos = (c.admaReadPos + 1u) & 0x1FFu;
                int16_t dummy = 0;
                if (c.mmix & 0x80u)
                    mixL += (inL * fixedVolume(c.avolL, dummy)) >> 15;
                if (c.mmix & 0x40u)
                    mixR += (inR * fixedVolume(c.avolR, dummy)) >> 15;
            }
            // core 0 output into core 1
            if (ci == 1u)
            {
                int16_t dummy = 0;
                if (c.mmix & 0x08u)
                    mixL += (core0L * fixedVolume(c.bvolL, dummy)) >> 15;
                if (c.mmix & 0x04u)
                    mixR += (core0R * fixedVolume(c.bvolR, dummy)) >> 15;
            }
            const int32_t l = clamp16((mixL * fixedVolume(c.mvolL, c.curMvolL)) >> 15);
            const int32_t r = clamp16((mixR * fixedVolume(c.mvolR, c.curMvolR)) >> 15);
            if (ci == 0u)
            {
                core0L = l;
                core0R = r;
            }
            else
            {
                outL = static_cast<int16_t>(l);
                outR = static_cast<int16_t>(r);
            }
        }
    }

    void IopSpu2::reportStats()
    {
        static const bool trace = std::getenv("PS2X_TRACE_SPU2") != nullptr;
        uint32_t active = 0;
        for (const Core &c : m_cores)
            for (const Voice &v : c.voices)
                active += v.phase != Voice::Phase::Off ? 1u : 0u;
        if (trace)
            std::fprintf(stderr,
                         "[spu2] 5 s: key-ons %llu, active now %u, dma %llu KB, adma %llu samples, irqs %llu, peak %d | core0 attr %04x mmix %04x "
                         "mvol %04x/%04x admas %x | core1 attr %04x mmix %04x mvol %04x/%04x admas %x\n",
                         (unsigned long long)m_stats.keyOns, active, (unsigned long long)(m_stats.dmaBytes >> 10),
                         (unsigned long long)m_stats.admaSamples, (unsigned long long)m_stats.irqs, m_stats.peak, m_cores[0].attr,
                         m_cores[0].mmix, m_cores[0].mvolL, m_cores[0].mvolR, m_cores[0].admas, m_cores[1].attr, m_cores[1].mmix,
                         m_cores[1].mvolL, m_cores[1].mvolR, m_cores[1].admas);
        m_stats = {};
    }

    void IopSpu2::advanceTo(uint64_t cycle)
    {
        m_cycle = cycle;
        while (m_nextSampleCycle <= cycle)
        {
            int16_t l = 0, r = 0;
            mixOne(l, r);
            m_stats.peak = std::max<int32_t>(m_stats.peak, std::max(std::abs(l), std::abs(r)));
            if ((++m_stats.frames % (48000u * 5u)) == 0u)
                reportStats();
            m_outBuffer.push_back(l);
            m_outBuffer.push_back(r);
            m_nextSampleCycle += kCyclesPerSample;
            if (m_outBuffer.size() >= 512u)
            {
                if (m_sink)
                    m_sink(m_outBuffer.data(), static_cast<uint32_t>(m_outBuffer.size() / 2u));
                m_outBuffer.clear();
            }
        }
    }

    // ---- transfers ------------------------------------------------------------------------------------------------
    uint64_t IopSpu2::dmaTransfer(uint32_t core, uint8_t *iopRam, uint32_t iopRamSize, uint32_t madr, uint32_t bytes, bool toSpu)
    {
        core &= 1u;
        Core &c = m_cores[core];
        madr &= 0x1FFFFFu;
        if (madr >= iopRamSize)
            return 64u;
        bytes = std::min(bytes, iopRamSize - madr);
        const uint32_t halfwords = bytes / 2u;
        m_stats.dmaBytes += bytes;
        const uint16_t *src = reinterpret_cast<const uint16_t *>(iopRam + madr);
        if (toSpu && (c.admas & (1u << core)) != 0u)
        {
            // AutoDMA: 512-byte blocks per channel (256 L samples, then 256 R samples)
            for (uint32_t block = 0; block + 512u <= halfwords; block += 512u)
                for (uint32_t i = 0; i < 256u; ++i)
                    c.adma.emplace_back(static_cast<int16_t>(src[block + i]), static_cast<int16_t>(src[block + 256u + i]));
            m_stats.admaSamples += halfwords / 2u;
            // Done when the SPU has played down to one block: the next one is then queued in time.
            const uint64_t queued = c.adma.size();
            return queued > 256u ? (queued - 256u) * kCyclesPerSample : 64u;
        }
        if (toSpu)
        {
            for (uint32_t i = 0; i < halfwords; ++i)
                m_ram[(c.tsa + i) & kAddrMask] = src[i];
            checkIrq(c.tsa, c.tsa + halfwords);
            c.tsa = (c.tsa + halfwords) & kAddrMask;
        }
        else
        {
            uint16_t *dst = reinterpret_cast<uint16_t *>(iopRam + madr);
            for (uint32_t i = 0; i < halfwords; ++i)
                dst[i] = m_ram[(c.tsa + i) & kAddrMask];
            c.tsa = (c.tsa + halfwords) & kAddrMask;
        }
        return std::max<uint64_t>(64u, halfwords);
    }
}
