// See rhi.h: backend names and the device factory.
#include "rhi/rhi.h"

#include <cctype>
#include <cstring>

namespace rhi
{
    const char *backendName(Backend backend)
    {
        switch (backend)
        {
        case Backend::Vulkan: return "Vulkan";
        case Backend::Null: return "null";
        }
        return "?";
    }

    bool backendFromName(const char *name, Backend &out)
    {
        if (!name)
            return false;
        char lower[16] = {};
        for (size_t i = 0; name[i] && i + 1 < sizeof(lower); ++i)
            lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
        if (std::strcmp(lower, "vulkan") == 0)
            out = Backend::Vulkan;
        else if (std::strcmp(lower, "null") == 0)
            out = Backend::Null;
        else
            return false;
        return true;
    }

    std::unique_ptr<Device> createDevice(const DeviceDesc &desc, std::string &error)
    {
        switch (desc.backend)
        {
        case Backend::Vulkan: return createVulkanDevice(desc, error);
        case Backend::Null: return createNullDevice(desc, error);
        }
        error = std::string("no ") + backendName(desc.backend) + " renderer";
        return nullptr;
    }
}
