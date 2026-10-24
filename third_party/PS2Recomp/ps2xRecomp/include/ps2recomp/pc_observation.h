#ifndef PS2RECOMP_PC_OBSERVATION_H
#define PS2RECOMP_PC_OBSERVATION_H

#include "ps2recomp/types.h"

namespace ps2recomp
{
    // [gta-recomp] Which emitted instructions need ctx->pc (and the delay-slot bookkeeping) stored before them.
    //
    // Nothing reads ctx->pc while straight-line generated code runs except the runtime calls it makes: syscalls,
    // MMIO, guest calls, exceptions, the scheduler's checkpoints. So the per-instruction stores are dropped where
    // nothing can read them ("lean pc", on unless PS2X_RECOMP_FULL_PC=1 is set when the recompiler runs):
    // - pure ALU / FPU / MMI instructions (cannotObservePc);
    // - loads / stores into guest RAM or the scratchpad (isPlainMemoryAccess): the address goes through READn /
    //   WRITEn, whose checked path (I/O, debug watches, exceptions) stores the pc itself - the emitter puts it in a
    //   constant `ps2AccessPc` next to the access (ps2_runtime_macros.h);
    // - around branches with such a delay slot: the branch's own pc, the delay slot's pc / in_delay_slot /
    //   branch_pc, forward gotos and the not-taken fallthrough. Back-edges keep theirs (the checkpoint resumes there).
    // What still sees a stale pc: the sampling profiler and crash reports (to the last call / loop head / I/O access).
    bool cannotObservePc(const Instruction &inst);
    bool isPlainMemoryOpcode(const Instruction &inst);
    bool leanPcEnabled();
    bool restrictCtxEnabled(); // [gta-recomp] generated functions take ctx as __restrict (function_emitter.cpp)
}

#endif
