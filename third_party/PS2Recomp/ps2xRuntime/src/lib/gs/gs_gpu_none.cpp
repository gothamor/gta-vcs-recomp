#include "runtime/gs/gs_gpu.h"

// [gta-recomp] No compute GS renderer in this build (paraLLEl-GS was Windows-only): PS2X_GS=gpu falls back.
std::unique_ptr<GSGpuRenderer> makeGpuRenderer(const GSGpuOptions &, std::string &whyNot)
{
    whyNot = "no compute GS renderer in this build (PS2X_GS=hw is the GPU renderer)";
    return nullptr;
}
