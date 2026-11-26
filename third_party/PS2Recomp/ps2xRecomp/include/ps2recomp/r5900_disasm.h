#ifndef PS2RECOMP_R5900_DISASM_H
#define PS2RECOMP_R5900_DISASM_H

// [gta-recomp] The recompiler's own R5900 (Emotion Engine) instruction identification and disassembly, in place of
// the rabbitizer library (DECISIONS.md 2026-11-27). It gives what R5900Decoder took from rabbitizer: the
// instruction's facts (branch, jump, trap, load / store, which registers it writes, link) and the text of the
// `// 0xADDR: 0xRAW  ...` comments in the generated C++ - the same text as rabbitizer's with its default settings,
// so the recompiler's output did not change (checked over all 2^32 words before rabbitizer was removed).

#include <cstdint>
#include <string>

namespace ps2recomp::r5900
{
    struct Facts
    {
        bool isBranch = false;
        bool isJump = false;
        bool isTrap = false;
        bool doesLoad = false;
        bool doesStore = false;
        bool modifiesRs = false;
        bool modifiesRt = false;
        bool modifiesRd = false;
        bool doesLink = false;
        bool modifiesFs = false;
        bool modifiesFt = false;
        bool modifiesFd = false;
        bool modifiesHI = false;
        bool modifiesLO = false;
        bool isFunctionCall = false; // an "and link" instruction
        bool isReturn = false;       // jr $ra
        bool hasDelaySlot = false;   // a branch or a jump
        bool isEret = false;
    };

    Facts examine(uint32_t word);

    // the text rabbitizer's RabbitizerInstruction_disassemble gives: `addiu       $sp, $sp, -0x10`; a word that is
    // no valid instruction (or one GNU as would not take back) as `.word 0x...` with the attempt in a comment.
    // address: the instruction's own (jump targets take its upper bits).
    std::string disassemble(uint32_t word, uint32_t address);
}

#endif // PS2RECOMP_R5900_DISASM_H
