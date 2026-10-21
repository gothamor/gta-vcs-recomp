// [gta-recomp] see runtime/ps2_disc_image.h
#include "runtime/ps2_disc_image.h"

#include <atomic>

namespace
{
    std::atomic<IPS2DiscImage *> g_discImage{nullptr};
}

namespace ps2_disc
{
    void setImage(IPS2DiscImage *image)
    {
        g_discImage.store(image, std::memory_order_release);
    }

    IPS2DiscImage *image()
    {
        return g_discImage.load(std::memory_order_acquire);
    }
}
