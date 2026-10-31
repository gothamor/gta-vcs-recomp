// [gta-recomp] Guest RAM write watch - see runtime/ps2_write_watch.h. The PS5 has no OS write watch: guest RAM is plain
// memory, enabled() is false and every generation 0, so no block is ever told unchanged (they are copied, as before
// the watch). The interface stays for the RSL path (docs/FULLY_NATIVE.md step 4).
#include "runtime/ps2_write_watch.h"

#include <cstring>

namespace ps2writewatch
{
    uint8_t *allocate(size_t bytes)
    {
        uint8_t *ram = new uint8_t[bytes];
        std::memset(ram, 0, bytes);
        return ram;
    }

    void release(uint8_t *ram) { delete[] ram; }

    bool enabled() { return false; }

    uint32_t fold() { return 0u; }

    uint32_t generation(uint32_t) { return 0u; }

    bool unchangedSince(uint32_t, uint32_t) { return false; }
}
