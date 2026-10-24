// [gta-recomp] The calling convention of guest functions: the generated code, its stub wrappers and the host functions
// that replace guest ones (PS2Runtime::RecompiledFunction).
//
// Under clang on x86-64 they use preserve_none: a call saves no registers - the Win64 convention made every guest
// function save and restore 8 general and 6 vector registers, though generated code keeps nothing in registers across
// a guest call (the guest's state lives in R5900Context). Host code calling a guest function pays for the saves at that
// one boundary instead (the scheduler, an override calling the original), which is rare against guest-to-guest calls.
// docs/PERFORMANCE.md 8. PS2X_GUEST_CC_DEFAULT=1 (a define) keeps the platform's convention.
#pragma once

#if defined(__clang__) && (defined(__x86_64__) || defined(_M_X64)) && !defined(PS2X_GUEST_CC_DEFAULT)
#define PS2_GUEST_CC __attribute__((preserve_none))
#else
#define PS2_GUEST_CC
#endif
