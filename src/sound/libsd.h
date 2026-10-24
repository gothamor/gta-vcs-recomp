// Sony's IOP sound library (LIBSD.IRX, "libsd") reproduced on the host, driving a native SPU2 core
// (docs/NATIVE_PORT_ROADMAP.md milestone 6 step 2; docs/game-internals.md 9).
//
// Same entry encoding and register effects as the IRX (read from its code, tools/irxdis.py --decompile): entries are
// core | voice << 1 | param << 8 (+0x80 for per-core volumes), addresses are bytes in SPU2 RAM. Differences, by
// design: a transfer's data is in SPU2 RAM when VoiceTrans returns (its DMA still counts as running for the time the
// SPU2 core gives it, then completes - callback - as the interrupt would; a status call that waits completes it at
// once), and the delay loops of sceSdInit take no time.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace ps2x::iop::detail
{
    class IopSpu2;
}

namespace sound
{
    class LibSd
    {
    public:
        explicit LibSd(ps2x::iop::detail::IopSpu2 &spu);

        // sceSdInit(flag): 0 = full hardware reset and default settings
        int32_t init(uint32_t flag);

        void setParam(uint32_t entry, uint16_t value);
        [[nodiscard]] uint16_t getParam(uint32_t entry);
        void setSwitch(uint32_t entry, uint32_t value);
        [[nodiscard]] uint32_t getSwitch(uint32_t entry);
        void setAddr(uint32_t entry, uint32_t address);
        [[nodiscard]] uint32_t getAddr(uint32_t entry);
        void setCoreAttr(uint32_t entry, uint32_t value);
        [[nodiscard]] uint32_t getCoreAttr(uint32_t entry);

        // sceSdVoiceTrans(channel, mode, data, spuAddress, bytes): only mode 0 (DMA write to SPU2 RAM) is used by the
        // game. Returns the bytes moved (rounded up to 64) or libsd's negative error. The DMA moves whole 64-byte
        // blocks: `available` bytes at `data` may be read (what lies past `bytes` in the caller's buffer, as the IOP
        // DMA would read it); past those it moves zeros.
        int32_t voiceTrans(uint32_t channel, uint32_t mode, const uint8_t *data, uint32_t available, uint32_t spuAddress,
                           uint32_t bytes);
        // sceSdVoiceTransStatus(channel, flag): 1 when the channel's last transfer is done (flag 1 waits - here it
        // never has to).
        int32_t voiceTransStatus(uint32_t channel, uint32_t flag);

        using Callback = std::function<void(uint32_t)>;
        void setTransCallback(uint32_t channel, Callback callback); // sceSdSetTransCallback
        void setIrqCallback(Callback callback);                    // sceSdSetIRQCallback: on an SPU2 IRQ-address hit

        // IOP time (cycles, 36.864 MHz): transfers whose DMA time has passed complete - their callback is queued.
        void advance(uint64_t iopCycle);
        // The IOP cycle at which the next transfer completes (~0 when none runs).
        [[nodiscard]] uint64_t nextEvent() const;
        // Runs transfer-completion callbacks queued since the last call (on hardware they run from the DMA
        // interrupt, after the caller of VoiceTrans got control back). Returns whether any ran.
        bool flushCallbacks();
        // The SPU2 interrupt: clears the IRQ-enable bit of the core(s) that hit their IRQ address and calls the IRQ
        // callback, as libsd's interrupt handler does. Call after advancing the SPU2 when it reports an IRQ.
        void serviceIrq();

    private:
        uint32_t paramRegister(uint32_t entry) const;
        void write(uint32_t offset, uint16_t value);
        uint16_t read(uint32_t offset);
        void resetHardware();
        void resetVoices();
        void defaultSettings(uint32_t flag);
        void completeTransfer(uint32_t channel);

        ps2x::iop::detail::IopSpu2 &m_spu;
        uint32_t m_spdif = 0;            // the last SPDIF mode set (sceSdGetCoreAttr(10))
        int32_t m_transDone[2] = {1, 1}; // the last transfer is done and was reported
        int32_t m_transPending[2] = {0, 0}; // done, not yet reported (no callback set)
        Callback m_transCallback[2];
        Callback m_irqCallback;
        std::deque<uint32_t> m_completed; // channels whose transfer completed since the last flush
        uint64_t m_now = 0;               // IOP time
        bool m_dmaBusy[2] = {false, false};
        uint64_t m_dmaDue[2] = {0, 0};
        std::vector<uint8_t> m_padded;   // a transfer rounded up to 64 bytes
    };
}
