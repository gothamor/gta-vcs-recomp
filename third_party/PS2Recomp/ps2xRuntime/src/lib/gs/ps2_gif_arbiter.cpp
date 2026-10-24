#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include <algorithm>
#include <cstring>

namespace
{
    // [gta-recomp] packet buffers kept with their capacity: every DIRECT / PATH3 packet was a heap allocation and free
    // (thread-local: the render worker submits and drains; a few buffers per thread, the large ones not kept)
    thread_local std::vector<std::vector<uint8_t>> t_packetPool;
}

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    if (!t_packetPool.empty()) // [gta-recomp]
    {
        pkt.data = std::move(t_packetPool.back());
        t_packetPool.pop_back();
    }
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            gsSetCurrentGifPath(static_cast<uint8_t>(pkt.pathId)); // [gta-recomp]
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            gsSetCurrentGifPath(3u);
        }
    }
    for (auto &pkt : m_queue) // [gta-recomp] back to the pool
        if (t_packetPool.size() < 16u && pkt.data.capacity() <= (1u << 20))
            t_packetPool.push_back(std::move(pkt.data));
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
