// PC VC RenderWare files converted into VCS's own (docs/MODS.md "PC VC cars in VCS", FORK.md T9): the same
// conversions as tools/dff2mdl.py and tools/txd2xtx.py, byte for byte (tools/rwconv_check.py compares them), so a VC car
// mod's `comet.dff` + `comet.txd` in a mod folder load as VCS's `comet` without a step ahead of time.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vcs::rwconv
{
    // A VC car's .dff onto a VCS vehicle model (an `ldm` resource: the vehicle replaced, from the disc) - its handling,
    // collision and wheels stay - or a weapon's onto a VCS weapon's (a model of one element). `txd`: the .txd beside it
    // (its texture names; may be null). `preserve`: a texture name VCS's dictionary keeps (a weapon's: its HUD icon,
    // named like the model) - materials naming VC's texture of that name use it as `<name>_pc`, as txdToXtx writes it.
    // False (and `report` says why) when either file is not what it should be.
    bool dffToMdl(const std::vector<uint8_t> &dff, const std::vector<uint8_t> &templateMdl, const std::vector<uint8_t> *txd,
                  std::vector<uint8_t> &out, std::string &report, const std::string &preserve = {});

    // A PC (D3D8) .txd into a VCS texture dictionary (a `tex` resource): each texture's top level, halved down to
    // `maxSize` pixels, quantised to 4 or 8 bit. `keep`: the VCS dictionary it replaces (may be null) - its textures
    // the .txd has no name for follow as they are (the parts of the VCS model the conversion keeps use them).
    // `preserve`: the .txd's texture of that name is written as `<name>_pc`, so VCS's own stays (dffToMdl).
    bool txdToXtx(const std::vector<uint8_t> &txd, int maxSize, const std::vector<uint8_t> *keep, std::vector<uint8_t> &out,
                  std::string &report, const std::string &preserve = {});

    // The VCS model a VC car's or weapon's file name stands for: VCS shortened some names to 8 letters (`police` ->
    // `policem`) and has some weapons under its own (`colt45` -> `beretta`); the name itself otherwise (lower case).
    // tools/dff2mdl.py VC_NAMES.
    std::string vcsName(const std::string &vcName);

    // `vcs.exe --rwconv-dff DFF TEMPLATE OUT [TXD]` / `--rwconv-txd TXD OUT [VCS.xtx]`, either with `--preserve NAME`: the
    // conversions on their own (for tools/rwconv_check.py); returns the exit code, or -1 when argv is no such command.
    int commandLine(int argc, char *argv[]);
}
