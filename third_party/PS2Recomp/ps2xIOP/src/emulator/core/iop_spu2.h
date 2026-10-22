// [gta-recomp] SPU2 (PS2 sound processor) as seen from the IOP: registers at 0x1F900000..0x1F9007FF, 2 MB of
// sound RAM, transfers from IOP DMA channels 4 / 7 and the manual data port, 2 cores x 24 ADPCM voices with
// pitch / ADSR / loops, AutoDMA PCM input, IRQ-address interrupts and a 48 kHz stereo mix handed to the host.
// Not yet: reverb, noise, pitch modulation, Gaussian interpolation (linear here).
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace ps2x::iop::detail
{
    class IopSpu2
    {
    public:
        static constexpr uint32_t kRegBase = 0x1F900000u;
        static constexpr uint32_t kRegSize = 0x800u;
        static constexpr uint32_t kRamHalfwords = 0x100000u; // 2 MB
        static constexpr uint32_t kCyclesPerSample = 768u;   // 36.864 MHz / 48 kHz

        using SampleSink = std::function<void(const int16_t *stereo, uint32_t frames)>;

        IopSpu2();
        void reset();

        [[nodiscard]] uint16_t read16(uint32_t offset);
        void write16(uint32_t offset, uint16_t value);

        // An IOP DMA on channel 4 (core 0) / 7 (core 1). Returns the cycles until the transfer counts as done.
        uint64_t dmaTransfer(uint32_t core, uint8_t *iopRam, uint32_t iopRamSize, uint32_t madr, uint32_t bytes, bool toSpu);

        // Advance to IOP cycle `cycle`, mixing one stereo sample per 768 cycles.
        void advanceTo(uint64_t cycle);
        [[nodiscard]] bool takeIrq() noexcept
        {
            const bool irq = m_irqPending;
            m_irqPending = false;
            return irq;
        }
        void setSampleSink(SampleSink sink) { m_sink = std::move(sink); }

    private:
        struct Voice
        {
            uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
            int16_t curVolL = 0, curVolR = 0;
            uint32_t ssa = 0, lsa = 0, nax = 0;
            bool lsaSetBySoftware = false;
            // decoder
            int16_t samples[28]{};
            int32_t hist1 = 0, hist2 = 0;
            uint32_t counter = 0; // 4.12: position within the decoded block
            int16_t prevSample = 0;
            bool blockValid = false;
            uint8_t blockFlags = 0;
            uint32_t blockAddr = 0;
            // envelope
            enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release } phase = Phase::Off;
            int32_t level = 0;
            uint32_t envCounter = 0;
        };

        struct Core
        {
            std::array<Voice, 24> voices{};
            uint16_t attr = 0, mmix = 0, admas = 0, statx = 0x80;
            uint32_t vmixL = 0, vmixR = 0, vmixEL = 0, vmixER = 0, pmon = 0, non = 0, endx = 0;
            uint32_t irqa = 0, tsa = 0;
            uint32_t konLatch = 0, kofLatch = 0;
            uint16_t mvolL = 0, mvolR = 0, evolL = 0, evolR = 0, avolL = 0, avolR = 0, bvolL = 0, bvolR = 0;
            int16_t curMvolL = 0, curMvolR = 0;
            std::array<uint16_t, 0x200> raw{}; // last written value of every core register (fallback reads)
            std::deque<std::pair<int16_t, int16_t>> adma; // AutoDMA input FIFO
            uint32_t admaReadPos = 0; // halfword offset in the input buffer, for IRQ checks
        };

        void writeCore(uint32_t core, uint32_t reg, uint16_t value);
        uint16_t readCore(uint32_t core, uint32_t reg);
        void keyOn(Core &c, uint32_t coreIndex, uint32_t mask);
        void keyOff(Core &c, uint32_t mask);
        void decodeBlock(Core &c, uint32_t coreIndex, Voice &v);
        int32_t voiceSample(Core &c, uint32_t coreIndex, uint32_t voiceIndex);
        void stepEnvelope(Voice &v);
        void mixOne(int16_t &outL, int16_t &outR);
        void checkIrq(uint32_t addressLo, uint32_t addressHi); // halfword range [lo, hi)
        static int16_t fixedVolume(uint16_t reg, int16_t &current);

        std::vector<uint16_t> m_ram;
        std::array<Core, 2> m_cores{};
        uint16_t m_irqInfo = 0;
        uint16_t m_spdif[8]{};
        bool m_irqPending = false;
        uint64_t m_cycle = 0;
        uint64_t m_nextSampleCycle = 0;
        SampleSink m_sink;
        std::vector<int16_t> m_outBuffer;
        struct Stats // PS2X_TRACE_SPU2=1
        {
            uint64_t keyOns = 0, dmaBytes = 0, admaSamples = 0, irqs = 0, frames = 0;
            int32_t peak = 0;
            uint32_t maxActive = 0;
        } m_stats;
        void reportStats();
    };
}
