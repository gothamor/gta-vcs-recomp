// [gta-recomp] SPU2 checks: a voice through the dry path and the reverb (psx-spx "Room" preset), Gaussian
// interpolation passing a constant, pitch modulation and noise producing output. Run by PS2X_IOP_BUILD_TESTS.
#include "emulator/core/iop_spu2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    using ps2x::iop::detail::IopSpu2;

    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        g_failures += ok ? 0 : 1;
    }

    // core c register offset (core 1 at +0x400; the 0x760 block at +0x28)
    void wr(IopSpu2 &spu, uint32_t core, uint32_t reg, uint16_t value) { spu.write16(core * 0x400u + reg, value); }
    void wrHiLo(IopSpu2 &spu, uint32_t core, uint32_t reg, uint32_t addr)
    {
        wr(spu, core, reg, static_cast<uint16_t>(addr >> 16));
        wr(spu, core, reg + 2u, static_cast<uint16_t>(addr));
    }

    // One ADPCM block at halfword address `at`: flags (loop start + end + repeat), shift 0, filter 0, every nibble n.
    void writeBlock(IopSpu2 &spu, uint32_t core, uint32_t at, uint8_t flags, uint8_t nibble)
    {
        wrHiLo(spu, core, 0x1A8, at); // TSA
        const uint16_t data = static_cast<uint16_t>(nibble * 0x1111u);
        wr(spu, core, 0x1AC, static_cast<uint16_t>(flags << 8)); // shift 0 / filter 0
        for (int i = 0; i < 7; ++i)
            wr(spu, core, 0x1AC, data);
    }

    struct Capture
    {
        std::vector<int16_t> samples; // stereo
    };

    std::vector<int16_t> run(IopSpu2 &spu, uint64_t &cycle, uint32_t frames)
    {
        std::vector<int16_t> out;
        spu.setSampleSink([&](const int16_t *s, uint32_t n) { out.insert(out.end(), s, s + n * 2u); });
        cycle += static_cast<uint64_t>(frames) * IopSpu2::kCyclesPerSample;
        spu.advanceTo(cycle);
        return out;
    }

    double rms(const std::vector<int16_t> &s, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to && i < s.size(); ++i)
            sum += double(s[i]) * s[i];
        return std::sqrt(sum / double(std::max<size_t>(1, to - from)));
    }

    // core 1 output, voice 0 playing a looping constant block at pitch 0x1000, envelope attack instant, sustain max
    void setupVoice(IopSpu2 &spu, uint8_t nibble)
    {
        const uint32_t core = 1;
        wr(spu, core, 0x19A, 0xC000); // ATTR: enable, unmute
        writeBlock(spu, core, 0x5000, 0x07, nibble);
        wr(spu, core, 0x000, 0x3FFF); // voice 0 vol L / R
        wr(spu, core, 0x002, 0x3FFF);
        wr(spu, core, 0x004, 0x1000); // pitch 48 kHz
        wr(spu, core, 0x006, 0x000F); // ADSR1: attack rate 0 (fastest), sustain level max
        wr(spu, core, 0x008, 0x0000); // ADSR2: sustain hold
        wrHiLo(spu, core, 0x1C0, 0x5000); // SSA
        wr(spu, core, 0x188, 1); // VMIXL / VMIXR voice 0
        wr(spu, core, 0x190, 1);
        wr(spu, core, 0x198, 0x0C00); // MMIX: voices dry
        spu.write16(0x760u + 0x28u, 0x3FFF); // core 1 MVOL L / R
        spu.write16(0x762u + 0x28u, 0x3FFF);
    }

    void testDryAndGauss()
    {
        IopSpu2 spu;
        uint64_t cycle = 0;
        setupVoice(spu, 0x2);
        wr(spu, 1, 0x1A0, 1); // KON voice 0
        const auto out = run(spu, cycle, 4800);
        // a constant input (nibble 2 << 12 = 0x2000) through Gaussian: 0x2000 * 0x7F7F..0x7F81 / 0x8000, then
        // voice volume 0x7FFE and master 0x7FFE -> about 0x2000 * 0.996^3
        int16_t lo = 32767, hi = -32768;
        for (size_t i = 2000; i < out.size(); i += 2)
        {
            lo = std::min(lo, out[i]);
            hi = std::max(hi, out[i]);
        }
        std::printf("     dry constant: %d..%d\n", lo, hi);
        check(hi - lo <= 2 && lo > 0x1F00 && hi <= 0x2000, "Gaussian interpolation passes a constant (no ripple at block ends)");
    }

    void testReverb()
    {
        IopSpu2 spu;
        uint64_t cycle = 0;
        setupVoice(spu, 0x2);
        const uint32_t core = 1;
        // "Room" preset (psx-spx), addresses x4 for SPU2's halfword units (PSX units are 8 bytes)
        const uint16_t room[32] = {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6,
                                   0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000,
                                   0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000};
        // psx order: dAPF1 dAPF2 vIIR vCOMB1-4 vWALL vAPF1 vAPF2 mLSAME mRSAME mLCOMB1 mRCOMB1 mLCOMB2 mRCOMB2 dLSAME dRSAME
        //            mLDIFF mRDIFF mLCOMB3 mRCOMB3 mLCOMB4 mRCOMB4 dLDIFF dRDIFF mLAPF1 mRAPF1 mLAPF2 mRAPF2 vLIN vRIN
        const int addrIdx[22] = {0, 1, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};
        const int volIdx[10] = {2, 3, 4, 5, 6, 7, 8, 9, 30, 31};
        const uint32_t esa = 0xF0000u, eea = 0xFFFFFu;
        wrHiLo(spu, core, 0x2E0, esa);
        wr(spu, core, 0x33C, static_cast<uint16_t>(eea >> 16));
        for (int i = 0; i < 22; ++i)
            wrHiLo(spu, core, 0x2E4u + 4u * i, static_cast<uint32_t>(room[addrIdx[i]]) * 4u);
        for (int i = 0; i < 10; ++i)
            spu.write16(0x774u + 0x28u + 2u * i, room[volIdx[i]]);
        wr(spu, core, 0x18C, 1); // VMIXEL / VMIXER voice 0
        wr(spu, core, 0x194, 1);
        wr(spu, core, 0x198, 0x0F00);      // MMIX: voices dry + wet
        spu.write16(0x764u + 0x28u, 0x3000); // EVOL L / R
        spu.write16(0x766u + 0x28u, 0x3000);
        wr(spu, core, 0x19A, 0xC080); // ATTR: + effects enable

        wr(spu, core, 0x1A0, 1); // KON
        auto out = run(spu, cycle, 48000 / 4);
        wr(spu, core, 0x1A4, 1); // KOFF: release (fast) - the dry signal stops, the reverb tail goes on
        wr(spu, core, 0x008, 0x0000);
        auto tail = run(spu, cycle, 48000);
        const double during = rms(out, out.size() / 2, out.size());
        const double tail0 = rms(tail, 4800, 9600);       // 50-100 ms after key off
        const double tail1 = rms(tail, 48000, 52800);     // 500-550 ms
        const double tailEnd = rms(tail, 86400, 96000);   // 0.9-1 s
        int32_t peak = 0;
        for (int16_t s : tail)
            peak = std::max<int32_t>(peak, std::abs(s));
        std::printf("     reverb: during %.0f, tail 0.05 s %.0f, 0.5 s %.0f, 1 s %.0f, peak %d\n", during, tail0, tail1, tailEnd, peak);
        check(tail0 > 50.0, "reverb leaves a tail after the voice stops");
        check(tail1 < tail0 && tailEnd < tail1, "the tail decays");
        check(peak < 32767, "reverb does not saturate");

        // the same with PS2X_SPU2_REVERB unset but EVOL 0: no tail
        spu.write16(0x764u + 0x28u, 0);
        spu.write16(0x766u + 0x28u, 0);
        auto silent = run(spu, cycle, 4800);
        check(rms(silent, 2400, silent.size()) < 1.0, "EVOL 0 mutes the reverb");
    }

    void testPmonNoise()
    {
        IopSpu2 spu;
        uint64_t cycle = 0;
        setupVoice(spu, 0x2);
        const uint32_t core = 1;
        // voice 1 = noise
        wr(spu, core, 0x010, 0x3FFF);
        wr(spu, core, 0x012, 0x3FFF);
        wr(spu, core, 0x014, 0x1000);
        wr(spu, core, 0x016, 0x000F);
        wrHiLo(spu, core, 0x1CC, 0x5000);
        wr(spu, core, 0x184, 0x2); // NON voice 1
        wr(spu, core, 0x188, 0x2); // only voice 1 to the output
        wr(spu, core, 0x190, 0x2);
        wr(spu, core, 0x19A, 0xC000 | (0xF << 10)); // fastest noise clock
        wr(spu, core, 0x1A0, 0x3);
        auto out = run(spu, cycle, 4800);
        check(rms(out, 2000, out.size()) > 1000.0, "noise voice produces noise");
        // pitch modulation: voice 1 modulated by voice 0 (constant +0x2000 -> factor 1.25): not silent, no crash
        wr(spu, core, 0x184, 0x0);
        wr(spu, core, 0x180, 0x2);
        wr(spu, core, 0x1A0, 0x3);
        out = run(spu, cycle, 4800);
        check(rms(out, 2000, out.size()) > 1000.0, "pitch-modulated voice plays");
    }

    // A core 0 voice reaches the output through core 1's external input, scaled by core 1's AVOL (0x790/0x792); BVOL
    // (0x794) is the AutoDMA input's. libsd's sceSdInit sets core 1 AVOL 0x7FFF, BVOL 0 - with the two swapped every
    // core 0 voice was silent (VCS: half its gunshots, doors, the bike screech).
    double core0Output(uint16_t core1Avol, uint16_t core1Bvol)
    {
        IopSpu2 spu;
        uint64_t cycle = 0;
        for (uint32_t core : {0u, 1u})
            wr(spu, core, 0x19A, 0xC000);
        writeBlock(spu, 0, 0x5000, 0x07, 0x2);
        wr(spu, 0, 0x000, 0x3FFF);
        wr(spu, 0, 0x002, 0x3FFF);
        wr(spu, 0, 0x004, 0x1000);
        wr(spu, 0, 0x006, 0x000F);
        wr(spu, 0, 0x008, 0x0000);
        wrHiLo(spu, 0, 0x1C0, 0x5000);
        wr(spu, 0, 0x188, 1);
        wr(spu, 0, 0x190, 1);
        wr(spu, 0, 0x198, 0x0FF0); // libsd's MMIX: core 0 voices dry and wet
        wr(spu, 1, 0x198, 0x0FFC); // core 1 also takes the external input (core 0) dry
        spu.write16(0x760u, 0x3FFF); // core 0 MVOL
        spu.write16(0x762u, 0x3FFF);
        spu.write16(0x788u, 0x3FFF); // core 1 MVOL
        spu.write16(0x78Au, 0x3FFF);
        spu.write16(0x790u, core1Avol);
        spu.write16(0x792u, core1Avol);
        spu.write16(0x794u, core1Bvol);
        spu.write16(0x796u, core1Bvol);
        wr(spu, 0, 0x1A0, 1); // KON core 0 voice 0
        const auto out = run(spu, cycle, 4800);
        return rms(out, 2000, out.size());
    }

    void testCore0IntoCore1()
    {
        const double libsdDefaults = core0Output(0x7FFF, 0), noAvol = core0Output(0, 0x7FFF);
        std::printf("     core 0 voice at the output: %.0f with libsd's volumes, %.0f with core 1 AVOL 0\n", libsdDefaults, noAvol);
        check(libsdDefaults > 1000.0, "a core 0 voice is heard with libsd's default volumes (core 1 AVOL 0x7FFF)");
        check(noAvol < 1.0, "core 1 AVOL 0 silences core 0 (BVOL does not carry it)");
    }
}

int main()
{
    testDryAndGauss();
    testCore0IntoCore1();
    testReverb();
    testPmonNoise();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
