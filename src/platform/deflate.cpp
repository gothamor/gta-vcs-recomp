// DEFLATE (RFC 1951) in a zlib stream (RFC 1950), written from the RFCs: deflate.h.
#include "platform/deflate.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace platform
{
    namespace
    {
        constexpr int kWindow = 32768;
        constexpr int kWindowMask = kWindow - 1;
        constexpr int kHashBits = 15;
        constexpr int kMinMatch = 3;
        constexpr int kMaxMatch = 258;
        // how hard the matcher looks (zlib's level 6 has the same numbers)
        constexpr int kMaxChain = 128;
        constexpr int kGoodLength = 8;  // a match this long already: look a quarter as far
        constexpr int kNiceLength = 128; // stop looking at a match this long
        constexpr int kLazyLength = 16;  // don't look for a better match after one this long
        constexpr size_t kBlockTokens = 32768;

        constexpr uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                              67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                              4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                            769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                            9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        constexpr uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

        struct Token
        {
            uint16_t lengthOrLiteral; // a literal byte (distance 0) or a match length
            uint16_t distance;
        };

        struct CodeTables
        {
            uint8_t length[kMaxMatch + 1];   // match length -> length code 0..28 (symbol 257 + code)
            uint8_t nearDist[256];           // distance - 1 < 256 -> distance code
            uint8_t farDist[256];            // (distance - 1) >> 7 -> distance code
            CodeTables()
            {
                for (int len = 0; len <= kMaxMatch; ++len)
                {
                    int code = 28;
                    while (code > 0 && kLengthBase[code] > len)
                        --code;
                    length[len] = static_cast<uint8_t>(code);
                }
                auto slow = [](int distance) {
                    int code = 29;
                    while (kDistBase[code] > distance)
                        --code;
                    return static_cast<uint8_t>(code);
                };
                for (int d = 0; d < 256; ++d)
                {
                    nearDist[d] = slow(d + 1);
                    farDist[d] = slow((d << 7) + 1); // codes from 257 on cover whole multiples of 128
                }
            }
        };
        const CodeTables kCodes;

        int lengthCode(int length)
        {
            return kCodes.length[length];
        }

        int distCode(int distance)
        {
            const int d = distance - 1;
            return d < 256 ? kCodes.nearDist[d] : kCodes.farDist[d >> 7];
        }

        // how many bytes from a and b match, up to limit (8 at a time)
        int matchLength(const uint8_t *a, const uint8_t *b, int limit)
        {
            int n = 0;
            while (n + 8 <= limit)
            {
                uint64_t x, y;
                std::memcpy(&x, a + n, 8);
                std::memcpy(&y, b + n, 8);
                if (x != y)
                    return n + (__builtin_ctzll(x ^ y) >> 3); // little-endian: the first differing byte
                n += 8;
            }
            while (n < limit && a[n] == b[n])
                ++n;
            return n;
        }

        class BitWriter
        {
        public:
            explicit BitWriter(std::vector<uint8_t> &out) : m_out(out) {}

            void bits(uint32_t value, int count)
            {
                m_acc |= static_cast<uint64_t>(value) << m_count;
                m_count += count;
                while (m_count >= 8)
                {
                    m_out.push_back(static_cast<uint8_t>(m_acc));
                    m_acc >>= 8;
                    m_count -= 8;
                }
            }

            void align()
            {
                if (m_count > 0)
                    bits(0, 8 - m_count);
            }

        private:
            std::vector<uint8_t> &m_out;
            uint64_t m_acc = 0;
            int m_count = 0;
        };

        // Huffman code lengths of at most `limit` bits: a Huffman tree; when it is too deep, the counts are halved
        // (keeping every used symbol) and the tree built again
        void codeLengths(const uint32_t *counts, int symbols, int limit, uint8_t *lengths)
        {
            std::fill(lengths, lengths + symbols, uint8_t(0));
            std::vector<uint32_t> freq(counts, counts + symbols);
            std::vector<int> used;
            for (int s = 0; s < symbols; ++s)
                if (freq[s])
                    used.push_back(s);
            if (used.empty())
                return;
            if (used.size() == 1)
            {
                lengths[used[0]] = 1;
                return;
            }
            for (;;)
            {
                // two queues: the leaves sorted by count, the internal nodes in the order they are made
                std::vector<int> leaves = used;
                std::stable_sort(leaves.begin(), leaves.end(), [&](int a, int b) { return freq[a] < freq[b]; });
                const size_t n = leaves.size();
                std::vector<uint64_t> weight(2 * n);
                std::vector<int> parent(2 * n, -1);
                for (size_t i = 0; i < n; ++i)
                    weight[i] = freq[leaves[i]];
                size_t leaf = 0, inner = n, made = n;
                auto take = [&]() -> size_t {
                    if (leaf < n && (inner >= made || weight[leaf] <= weight[inner]))
                        return leaf++;
                    return inner++;
                };
                while (made < 2 * n - 1)
                {
                    const size_t a = take(), b = take();
                    weight[made] = weight[a] + weight[b];
                    parent[a] = parent[b] = static_cast<int>(made);
                    ++made;
                }
                // depths, root last: a node's depth is its parent's + 1
                std::vector<int> depth(2 * n, 0);
                int deepest = 0;
                for (size_t i = 2 * n - 2; i-- > 0;)
                {
                    depth[i] = depth[parent[i]] + 1;
                    if (i < n)
                        deepest = std::max(deepest, depth[i]);
                }
                if (deepest <= limit)
                {
                    for (size_t i = 0; i < n; ++i)
                        lengths[leaves[i]] = static_cast<uint8_t>(depth[i]);
                    return;
                }
                for (int s : used)
                    freq[s] = std::max<uint32_t>(1, freq[s] >> 1);
            }
        }

        // canonical codes (RFC 1951 3.2.2), bit-reversed for the LSB-first stream
        void canonicalCodes(const uint8_t *lengths, int symbols, uint16_t *codes)
        {
            int count[16] = {};
            for (int s = 0; s < symbols; ++s)
                ++count[lengths[s]];
            count[0] = 0;
            int next[16] = {};
            int code = 0;
            for (int bits = 1; bits < 16; ++bits)
            {
                code = (code + count[bits - 1]) << 1;
                next[bits] = code;
            }
            for (int s = 0; s < symbols; ++s)
            {
                const int len = lengths[s];
                if (!len)
                {
                    codes[s] = 0;
                    continue;
                }
                int c = next[len]++, reversed = 0;
                for (int i = 0; i < len; ++i)
                {
                    reversed = (reversed << 1) | (c & 1);
                    c >>= 1;
                }
                codes[s] = static_cast<uint16_t>(reversed);
            }
        }

        struct Code
        {
            std::array<uint8_t, 288> litLengths{};
            std::array<uint16_t, 288> litCodes{};
            std::array<uint8_t, 30> distLengths{};
            std::array<uint16_t, 30> distCodes{};
        };

        // the code lengths of a dynamic block as code-length symbols (16 / 17 / 18 runs)
        struct LengthSymbol
        {
            uint8_t symbol, extra;
        };

        class Deflater
        {
        public:
            Deflater(const uint8_t *data, size_t size, std::vector<uint8_t> &out)
                : m_data(data), m_size(size), m_bits(out), m_head(size_t(1) << kHashBits, -1), m_prev(kWindow, -1)
            {
                m_tokens.reserve(kBlockTokens + 2);
            }

            void run()
            {
                bool pending = false; // the byte before i is not coded yet
                int pendingLength = 0, pendingDistance = 0;
                size_t i = 0;
                while (i < m_size)
                {
                    int length = 0, distance = 0;
                    if (i + kMinMatch <= m_size)
                    {
                        const int32_t candidate = insert(i);
                        if (!(pending && pendingLength >= kLazyLength))
                            longestMatch(i, candidate, pending ? std::max(pendingLength, kMinMatch - 1) : kMinMatch - 1,
                                         length, distance);
                    }
                    if (pending && pendingLength >= kMinMatch && length <= pendingLength)
                    {
                        // the match at i - 1 is as good: take it
                        match(pendingLength, pendingDistance);
                        const size_t end = i - 1 + static_cast<size_t>(pendingLength);
                        for (size_t k = i + 1; k < end; ++k)
                            if (k + kMinMatch <= m_size)
                                insert(k);
                        i = end;
                        pending = false;
                        continue;
                    }
                    if (pending)
                        literal(m_data[i - 1]);
                    pending = true;
                    pendingLength = length;
                    pendingDistance = distance;
                    ++i;
                }
                if (pending)
                    literal(m_data[m_size - 1]);
                flush(true);
            }

        private:
            const uint8_t *m_data;
            size_t m_size;
            BitWriter m_bits;
            std::vector<int32_t> m_head;
            std::vector<int32_t> m_prev;
            std::vector<Token> m_tokens;
            size_t m_blockStart = 0; // input the block's tokens code: [m_blockStart, m_covered)
            size_t m_covered = 0;

            uint32_t hash(size_t i) const
            {
                const uint32_t v = (uint32_t(m_data[i]) << 16) | (uint32_t(m_data[i + 1]) << 8) | m_data[i + 2];
                return (v * 2654435761u) >> (32 - kHashBits);
            }

            // i into its hash chain; the chain's previous head (the newest earlier position with the same hash)
            int32_t insert(size_t i)
            {
                const uint32_t h = hash(i);
                const int32_t previous = m_head[h];
                m_prev[i & kWindowMask] = previous;
                m_head[h] = static_cast<int32_t>(i);
                return previous;
            }

            void longestMatch(size_t i, int32_t candidate, int beat, int &bestLength, int &bestDistance)
            {
                bestLength = 0;
                bestDistance = 0;
                int chain = beat >= kGoodLength ? kMaxChain / 4 : kMaxChain;
                const int maxLength = static_cast<int>(std::min<size_t>(kMaxMatch, m_size - i));
                if (maxLength < kMinMatch)
                    return;
                int best = beat;
                if (best >= maxLength) // nothing longer fits before the end (and here[best] would be past it)
                    return;
                const uint8_t *here = m_data + i;
                while (candidate >= 0 && chain-- > 0)
                {
                    const size_t distance = i - static_cast<size_t>(candidate);
                    if (distance > static_cast<size_t>(kWindow) || distance == 0)
                        break;
                    const uint8_t *there = m_data + candidate;
                    if (there[best] == here[best] && there[0] == here[0] && there[1] == here[1])
                    {
                        const int length = 2 + matchLength(there + 2, here + 2, maxLength - 2);
                        if (length > best)
                        {
                            best = length;
                            bestLength = length;
                            bestDistance = static_cast<int>(distance);
                            if (length >= kNiceLength || length >= maxLength)
                                break;
                        }
                    }
                    const int32_t next = m_prev[static_cast<size_t>(candidate) & kWindowMask];
                    if (next >= candidate) // a slot the window has reused
                        break;
                    candidate = next;
                }
            }

            void literal(uint8_t byte)
            {
                m_tokens.push_back({byte, 0});
                ++m_covered;
                if (m_tokens.size() >= kBlockTokens)
                    flush(false);
            }

            void match(int length, int distance)
            {
                m_tokens.push_back({static_cast<uint16_t>(length), static_cast<uint16_t>(distance)});
                m_covered += static_cast<size_t>(length);
                if (m_tokens.size() >= kBlockTokens)
                    flush(false);
            }

            static uint64_t tokenBits(const Token &t, const uint8_t *litLengths, const uint8_t *distLengths)
            {
                if (!t.distance)
                    return litLengths[t.lengthOrLiteral];
                const int lc = lengthCode(t.lengthOrLiteral), dc = distCode(t.distance);
                return uint64_t(litLengths[257 + lc]) + kLengthExtra[lc] + distLengths[dc] + kDistExtra[dc];
            }

            void writeTokens(const Code &code)
            {
                for (const Token &t : m_tokens)
                {
                    if (!t.distance)
                    {
                        m_bits.bits(code.litCodes[t.lengthOrLiteral], code.litLengths[t.lengthOrLiteral]);
                        continue;
                    }
                    const int lc = lengthCode(t.lengthOrLiteral), dc = distCode(t.distance);
                    m_bits.bits(code.litCodes[257 + lc], code.litLengths[257 + lc]);
                    if (kLengthExtra[lc])
                        m_bits.bits(t.lengthOrLiteral - kLengthBase[lc], kLengthExtra[lc]);
                    m_bits.bits(code.distCodes[dc], code.distLengths[dc]);
                    if (kDistExtra[dc])
                        m_bits.bits(t.distance - kDistBase[dc], kDistExtra[dc]);
                }
                m_bits.bits(code.litCodes[256], code.litLengths[256]);
            }

            static void runLengths(const uint8_t *lengths, int count, std::vector<LengthSymbol> &out)
            {
                for (int i = 0; i < count;)
                {
                    const uint8_t len = lengths[i];
                    int run = 1;
                    while (i + run < count && lengths[i + run] == len)
                        ++run;
                    if (len == 0 && run >= 3)
                    {
                        int left = run;
                        while (left >= 11)
                        {
                            const int n = std::min(left, 138);
                            out.push_back({18, static_cast<uint8_t>(n - 11)});
                            left -= n;
                        }
                        if (left >= 3)
                        {
                            out.push_back({17, static_cast<uint8_t>(left - 3)});
                            left = 0;
                        }
                        while (left-- > 0)
                            out.push_back({0, 0});
                    }
                    else if (len != 0 && run >= 4)
                    {
                        out.push_back({len, 0});
                        int left = run - 1;
                        while (left >= 3)
                        {
                            const int n = std::min(left, 6);
                            out.push_back({16, static_cast<uint8_t>(n - 3)});
                            left -= n;
                        }
                        while (left-- > 0)
                            out.push_back({len, 0});
                    }
                    else
                    {
                        for (int k = 0; k < run; ++k)
                            out.push_back({len, 0});
                    }
                    i += run;
                }
            }

            void flush(bool final)
            {
                // counts
                std::array<uint32_t, 286> litCounts{};
                std::array<uint32_t, 30> distCounts{};
                for (const Token &t : m_tokens)
                {
                    if (!t.distance)
                        ++litCounts[t.lengthOrLiteral];
                    else
                    {
                        ++litCounts[257 + lengthCode(t.lengthOrLiteral)];
                        ++distCounts[distCode(t.distance)];
                    }
                }
                litCounts[256] = 1;

                // dynamic codes
                Code dynamic;
                codeLengths(litCounts.data(), 286, 15, dynamic.litLengths.data());
                codeLengths(distCounts.data(), 30, 15, dynamic.distLengths.data());
                if (std::all_of(dynamic.distLengths.begin(), dynamic.distLengths.end(), [](uint8_t l) { return l == 0; }))
                    dynamic.distLengths[0] = 1; // a block without matches still describes one distance code
                int hlit = 286, hdist = 30;
                while (hlit > 257 && dynamic.litLengths[hlit - 1] == 0)
                    --hlit;
                while (hdist > 1 && dynamic.distLengths[hdist - 1] == 0)
                    --hdist;
                std::vector<uint8_t> all(dynamic.litLengths.begin(), dynamic.litLengths.begin() + hlit);
                all.insert(all.end(), dynamic.distLengths.begin(), dynamic.distLengths.begin() + hdist);
                std::vector<LengthSymbol> lengthSymbols;
                runLengths(all.data(), static_cast<int>(all.size()), lengthSymbols);
                std::array<uint32_t, 19> clCounts{};
                for (const LengthSymbol &ls : lengthSymbols)
                    ++clCounts[ls.symbol];
                std::array<uint8_t, 19> clLengths{};
                std::array<uint16_t, 19> clCodes{};
                codeLengths(clCounts.data(), 19, 7, clLengths.data());
                canonicalCodes(clLengths.data(), 19, clCodes.data());
                int hclen = 19;
                while (hclen > 4 && clLengths[kCodeLengthOrder[hclen - 1]] == 0)
                    --hclen;
                canonicalCodes(dynamic.litLengths.data(), 286, dynamic.litCodes.data());
                canonicalCodes(dynamic.distLengths.data(), 30, dynamic.distCodes.data());

                uint64_t dynamicBits = 3 + 5 + 5 + 4 + 3ull * hclen;
                for (const LengthSymbol &ls : lengthSymbols)
                    dynamicBits += clLengths[ls.symbol] + (ls.symbol == 16 ? 2 : ls.symbol == 17 ? 3 : ls.symbol == 18 ? 7 : 0);
                for (const Token &t : m_tokens)
                    dynamicBits += tokenBits(t, dynamic.litLengths.data(), dynamic.distLengths.data());
                dynamicBits += dynamic.litLengths[256];

                // fixed codes (RFC 1951 3.2.6)
                Code fixed;
                for (int s = 0; s < 288; ++s)
                    fixed.litLengths[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
                for (int s = 0; s < 30; ++s)
                    fixed.distLengths[s] = 5;
                canonicalCodes(fixed.litLengths.data(), 288, fixed.litCodes.data());
                canonicalCodes(fixed.distLengths.data(), 30, fixed.distCodes.data());
                uint64_t fixedBits = 3;
                for (const Token &t : m_tokens)
                    fixedBits += tokenBits(t, fixed.litLengths.data(), fixed.distLengths.data());
                fixedBits += 7;

                // stored: up to 65535 bytes a block, each 3 bits, alignment (up to 7) and 32 bits of lengths
                const size_t raw = m_covered - m_blockStart;
                const size_t storedBlocks = std::max<size_t>(1, (raw + 65534) / 65535);
                const uint64_t storedBits = storedBlocks * (3 + 7 + 32) + raw * 8ull;

                if (storedBits < dynamicBits && storedBits < fixedBits)
                {
                    size_t pos = m_blockStart;
                    for (size_t b = 0; b < storedBlocks; ++b)
                    {
                        const size_t n = std::min<size_t>(65535, m_covered - pos);
                        const bool last = final && b + 1 == storedBlocks;
                        m_bits.bits(last ? 1u : 0u, 1);
                        m_bits.bits(0, 2);
                        m_bits.align();
                        m_bits.bits(static_cast<uint32_t>(n), 16);
                        m_bits.bits(static_cast<uint32_t>(~n & 0xFFFF), 16);
                        for (size_t k = 0; k < n; ++k)
                            m_bits.bits(m_data[pos + k], 8);
                        pos += n;
                    }
                }
                else if (fixedBits <= dynamicBits)
                {
                    m_bits.bits(final ? 1u : 0u, 1);
                    m_bits.bits(1, 2);
                    writeTokens(fixed);
                }
                else
                {
                    m_bits.bits(final ? 1u : 0u, 1);
                    m_bits.bits(2, 2);
                    m_bits.bits(static_cast<uint32_t>(hlit - 257), 5);
                    m_bits.bits(static_cast<uint32_t>(hdist - 1), 5);
                    m_bits.bits(static_cast<uint32_t>(hclen - 4), 4);
                    for (int k = 0; k < hclen; ++k)
                        m_bits.bits(clLengths[kCodeLengthOrder[k]], 3);
                    for (const LengthSymbol &ls : lengthSymbols)
                    {
                        m_bits.bits(clCodes[ls.symbol], clLengths[ls.symbol]);
                        if (ls.symbol == 16)
                            m_bits.bits(ls.extra, 2);
                        else if (ls.symbol == 17)
                            m_bits.bits(ls.extra, 3);
                        else if (ls.symbol == 18)
                            m_bits.bits(ls.extra, 7);
                    }
                    writeTokens(dynamic);
                }
                if (final)
                    m_bits.align();
                m_tokens.clear();
                m_blockStart = m_covered;
            }
        };

        uint32_t adler32(const uint8_t *data, size_t size)
        {
            uint32_t a = 1, b = 0;
            while (size)
            {
                const size_t n = std::min<size_t>(size, 5552); // the most before b can overflow
                for (size_t i = 0; i < n; ++i)
                {
                    a += data[i];
                    b += a;
                }
                a %= 65521u;
                b %= 65521u;
                data += n;
                size -= n;
            }
            return (b << 16) | a;
        }
    }

    std::vector<uint8_t> zlibCompress(const uint8_t *data, size_t size)
    {
        std::vector<uint8_t> out;
        out.reserve(size / 2 + 64);
        out.push_back(0x78); // deflate, 32 KB window
        out.push_back(0x9C); // default level; (0x78 << 8 | 0x9C) % 31 == 0
        Deflater(data, size, out).run();
        const uint32_t check = adler32(data, size);
        out.push_back(static_cast<uint8_t>(check >> 24));
        out.push_back(static_cast<uint8_t>(check >> 16));
        out.push_back(static_cast<uint8_t>(check >> 8));
        out.push_back(static_cast<uint8_t>(check));
        return out;
    }
}
