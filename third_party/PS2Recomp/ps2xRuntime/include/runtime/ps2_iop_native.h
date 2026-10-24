// [gta-recomp] Native IOP services (docs/NATIVE_PORT_ROADMAP.md milestone 6). A game can serve SIF RPC servers on
// the host instead of through IRX code on the emulated IOP, keep its own IOP-side time (e.g. a native SPU2), and -
// once nothing needs it - run without the emulated IOP. The emulated IOP stays the reference: everything here is
// opt-in and the default path is unchanged.
#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstdint>
#include <functional>

namespace ps2_iop_native
{
    // A server for `sid`, called on the EE thread for every sceSifCallRpc to it: it reads request.send from EE memory
    // and writes its reply into request.receive (both EE addresses). It is answered like an IOP server that returned
    // at once (the RPC completes, its end callback runs). Registered servers win over the emulated IOP's.
    using Server = std::function<void(uint8_t *rdram, const ps2x::iop::RpcRequest &request)>;
    void registerServer(uint32_t sid, Server server);
    [[nodiscard]] bool hasServer(uint32_t sid);
    // Runs a registered server; false when `sid` has none.
    bool dispatch(uint8_t *rdram, const ps2x::iop::RpcRequest &request, ps2x::iop::RpcResult &result);

    // Called on the EE thread with the EE cycles IOP time advances by - the same batches, wall-clock catch-up included,
    // as the emulated IOP gets (PS2Runtime::advanceIopEeCycles). 294.912 MHz EE cycles; the IOP runs at 1/8 of that.
    using Clock = void (*)(uint64_t eeCycles); // a plain function: it runs thousands of times a second
    void setClock(Clock clock);
    void tick(uint64_t eeCycles);
    [[nodiscard]] uint64_t eeCycles(); // IOP time so far, in EE cycles

    // PS2X_RPC_RECORD=<file>: every RPC call to `sid`s listed in PS2X_RPC_RECORD_SIDS (hex, comma separated; all when
    // unset) is appended to <file> - IOP time (u64 EE cycles), sid, function, send size, receive size (u32 each), the
    // bytes sent, the reply's first word - so a native server can be replayed against the emulated IOP's run (tools/soundcheck.py). The time
    // is the emulated IOP's own when it runs (it runs ahead of EE time while an IOP server waits inside a call).
    [[nodiscard]] bool recording();
    void record(const uint8_t *rdram, const ps2x::iop::RpcRequest &request, uint64_t iopTimeEeCycles);
    void recordReply(const uint8_t *rdram, const ps2x::iop::RpcRequest &request); // then the reply's first word (u32)

    // With the emulated IOP off no IRX module is loaded or run (loads report success) and no IOP time passes on it;
    // every RPC server the game binds must then be native. PS2X_IOP=0 sets it at start; a game may set it before boot.
    void setEmulatedIop(bool enabled);
    [[nodiscard]] bool emulatedIop();

    // The emulated SPU2's output to the host is dropped while a native sound driver owns the audio output.
    void setEmulatedAudioMuted(bool muted);
    [[nodiscard]] bool emulatedAudioMuted();
}
