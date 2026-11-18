"""Test-only CHD v5 writer: builds small CHDs the way MAME's chdman lays them out, so tools/chd.py can be checked
without game data. Not a general chdman: one compressed map with fixed 4-bit codes, codecs chosen per hunk.

Formats followed: MAME src/lib/util/chd.cpp (v5 header, compressed map), chdcodec.cpp (zlib / lzma / huff / flac
and the CD codecs), huffman.cpp (the map's tree in RLE form, the huff codec's in huffman-coded form, canonical
codes), cdrom.h (2448-byte CD frames: 2352 sector + 96 subcode).
"""
import binascii
import collections
import lzma
import struct
import zlib

CD_FRAME = 2448
CD_SECTOR = 2352
CD_SUB = 96

# map entry types (chd.cpp)
TYPE0, TYPE1, TYPE2, TYPE3, NONE, SELF = 0, 1, 2, 3, 4, 5


class BitWriter:
    def __init__(self):
        self.bits = []

    def write(self, value, count):
        for i in range(count - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def data(self):
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def crc16(data):
    return binascii.crc_hqx(data, 0xFFFF)


def deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def lzma_raw(data):
    return lzma.compress(data, format=lzma.FORMAT_RAW,
                         filters=[{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16, "lc": 3, "lp": 0, "pb": 2}])


def _crc8(data):  # FLAC's frame header CRC (polynomial 0x07)
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def _crc16(data):  # FLAC's frame CRC (polynomial 0x8005)
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _flac_frame(num, n, chan, subframes):
    """One FLAC frame of n samples per channel: the header (fixed block size, its 16-bit size at the end, 44.1 kHz,
    16 bits, channel assignment `chan`) and its CRC-8, the subframes [(samples, bits per sample, _subframe spec)], then
    the CRC-16."""
    bw = BitWriter()
    bw.write(0x3FFE, 14)
    bw.write(0, 2)               # reserved, fixed block size
    bw.write(7, 4)               # block size: 16 bits at the end of the header
    bw.write(9, 4)               # 44.1 kHz
    bw.write(chan, 4)
    bw.write(4, 3)               # 16 bits per sample
    bw.write(0, 1)
    bw.write(num, 8)             # frame number (UTF-8, < 0x80)
    bw.write(n - 1, 16)
    bw.write(_crc8(bw.data()), 8)
    for s, bps, spec in subframes:
        _subframe(bw, s, bps, spec)
    body = bw.data()
    return body + struct.pack(">H", _crc16(body))


def flac_frames(samples, blocksize, compress=False):
    """Interleaved 16-bit stereo `samples` as FLAC frames (no stream header, as in a CHD): VERBATIM subframes, or with
    `compress` CONSTANT ones for a constant channel and FIXED order 1 otherwise - smaller than the samples, as libchdr
    needs (a compressed hunk longer than the hunk is a read error there; chdman stores such a hunk uncompressed)."""
    out = b""
    frames = len(samples) // 2
    for num, start in enumerate(range(0, frames, blocksize)):
        n = min(blocksize, frames - start)
        subframes = []
        for ch in range(2):
            channel = [samples[(start + i) * 2 + ch] for i in range(n)]
            spec = ({"kind": "constant"} if len(set(channel)) == 1 else {"kind": "fixed", "order": 1}) if compress \
                else {"kind": "verbatim"}
            subframes.append((channel, 16, spec))
        out += _flac_frame(num, n, 1, subframes)  # 1: left / right
    return out


def _rice(bw, values, k):
    for r in values:
        u = 2 * r if r >= 0 else -2 * r - 1
        for _ in range(u >> k):
            bw.write(0, 1)
        bw.write(1, 1)
        bw.write(u & ((1 << k) - 1), k)


def _subframe(bw, s, bps, spec):
    """spec: {"kind": "constant" | "verbatim" | "fixed" | "lpc", "order": n (fixed), "coefs": [...], "shift": n,
    "precision": n (lpc), "wasted": n (optional)}."""
    kind = spec["kind"]
    wasted = spec.get("wasted", 0)
    if wasted:
        assert all(v % (1 << wasted) == 0 for v in s)
        s = [v >> wasted for v in s]
        bps -= wasted
    bw.write(0, 1)
    if kind == "constant":
        assert len(set(s)) == 1
        bw.write(0, 6)
    elif kind == "verbatim":
        bw.write(1, 6)
    elif kind == "fixed":
        bw.write(8 + spec["order"], 6)
    else:
        bw.write(31 + len(spec["coefs"]), 6)
    bw.write(1 if wasted else 0, 1)
    if wasted:  # unary: wasted - 1 zeros, then a one
        for _ in range(wasted - 1):
            bw.write(0, 1)
        bw.write(1, 1)
    mask = (1 << bps) - 1
    if kind == "constant":
        bw.write(s[0] & mask, bps)
        return
    if kind == "verbatim":
        for v in s:
            bw.write(v & mask, bps)
        return
    if kind == "fixed":
        coefs, shift = [[], [1], [2, -1], [3, -3, 1], [4, -6, 4, -1]][spec["order"]], 0
    else:
        coefs, shift = spec["coefs"], spec["shift"]
    order = len(coefs)
    res = [s[i] - (sum(c * s[i - 1 - j] for j, c in enumerate(coefs)) >> shift) for i in range(order, len(s))]
    for v in s[:order]:
        bw.write(v & mask, bps)
    if kind == "lpc":
        prec = spec["precision"]
        bw.write(prec - 1, 4)
        bw.write(shift & 0x1F, 5)
        for c in coefs:
            bw.write(c & ((1 << prec) - 1), prec)
    # residual: method 0, partition order 1 (two partitions, each with its own Rice parameter)
    bw.write(0, 2)
    bw.write(1, 4)
    split = (len(s) >> 1) - order
    for part in (res[:split], res[split:]):
        mean = sum(abs(v) for v in part) // max(1, len(part))
        k = min(14, mean.bit_length())
        bw.write(k, 4)
        _rice(bw, part, k)


def flac_encode(samples, blocksize, plans):
    """Interleaved 16-bit stereo `samples` as FLAC frames; frame i uses plans[i % len(plans)] = (channel assignment,
    spec of the first subframe, spec of the second). Assignment 1: left / right; 8: left / side; 9: side / right;
    10: mid / side. Covers what tools/chd.py decodes: CONSTANT, VERBATIM, FIXED 0-4, LPC, wasted bits, Rice
    partitions."""
    out = b""
    frames = len(samples) // 2
    for num, start in enumerate(range(0, frames, blocksize)):
        chan, spec0, spec1 = plans[num % len(plans)]
        n = min(blocksize, frames - start)
        left = [samples[(start + i) * 2] for i in range(n)]
        right = [samples[(start + i) * 2 + 1] for i in range(n)]
        side = [l - r for l, r in zip(left, right)]
        chans = {1: [(left, 16), (right, 16)], 8: [(left, 16), (side, 17)], 9: [(side, 17), (right, 16)],
                 10: [([(l + r) >> 1 for l, r in zip(left, right)], 16), (side, 17)]}[chan]
        out += _flac_frame(num, n, chan, [(s, bps, spec) for (s, bps), spec in zip(chans, (spec0, spec1))])
    return out


def raw_mode1_frames(sectors, seed=0):
    """MODE1_RAW CD frames: sync, header, the 2048 data bytes, then EDC / ECC bytes (made up: nothing checks them
    but the CRC16 of the frames) and 96 bytes of subcode."""
    import random
    rng = random.Random(seed)
    out = []
    for lba, data in enumerate(sectors):
        header = bytes([lba // 4500 % 100, lba // 75 % 60, lba % 75, 1])
        tail = bytes(rng.randrange(256) for _ in range(CD_SECTOR - 16 - 2048))
        out.append(b"\x00" + b"\xff" * 10 + b"\x00" + header + data + tail + bytes(CD_SUB))
    return out


# How a hunk is encoded, beyond its codec:
#   ecc_frames: frames whose sync header (0-11) and EDC / ECC (2064-2351) the compressor removed (build_chd: frame
#     numbers of the whole disc; cd_compress: of its hunk), as
#     chdman does for raw sectors with valid ECC (the reader puts the sync back; the hunk's CRC16 is of the full
#     frames);
#   flac_plans: flac_encode's plans for cdfl (default: VERBATIM);
#   flac_endian: the flac codec's sample byte order, "L" or "B".
Encoding = collections.namedtuple("Encoding", "ecc_frames flac_plans flac_endian", defaults=((), None, "L"))


def cd_compress(codec, hunk, frames, enc=Encoding()):
    base = bytearray(b"".join(hunk[i * CD_FRAME:i * CD_FRAME + CD_SECTOR] for i in range(frames)))
    sub = b"".join(hunk[i * CD_FRAME + CD_SECTOR:(i + 1) * CD_FRAME] for i in range(frames))
    if codec == b"cdfl":
        samples = list(struct.unpack(">%dh" % (len(base) // 2), bytes(base)))
        audio = flac_encode(samples, 1176, enc.flac_plans) if enc.flac_plans else flac_frames(samples, 1176)
        return audio + deflate(sub)
    ecc = bytearray((frames + 7) // 8)
    for i in enc.ecc_frames:
        ecc[i // 8] |= 1 << (i % 8)
        base[i * CD_SECTOR:i * CD_SECTOR + 12] = bytes(12)
        base[i * CD_SECTOR + 2064:(i + 1) * CD_SECTOR] = bytes(CD_SECTOR - 2064)
    comp = CD_COMPRESSORS[codec](bytes(base))
    return bytes(ecc) + struct.pack(">H", len(comp)) + comp + deflate(sub)


def canonical_codes(lens):
    """MAME huffman_assign_canonical_codes: {symbol: (code, length)} for the code lengths (0: unused)."""
    histo = [0] * 33
    for length in lens:
        histo[length] += 1
    start = 0
    for cl in range(32, 0, -1):
        nxt = (start + histo[cl]) >> 1
        histo[cl] = start
        start = nxt
    codes = {}
    for sym, length in enumerate(lens):
        if length:
            codes[sym] = (histo[length], length)
            histo[length] += 1
    return codes


def balanced_lengths(count):
    """Code lengths of a complete tree over `count` (>= 2) equally likely symbols."""
    k = (count - 1).bit_length()
    short = (1 << k) - count  # this many get k - 1 bits
    return [k - 1] * short + [k] * (count - short)


# The huff codec's byte code (8-bit symbols, up to 16 bits): a complete but uneven tree, so the canonical codes are not
# the bytes themselves - 0 in 1 bit, 1 in 2, 2 and 3 in 9, the rest in 10 (1/2 + 1/4 + 2/512 + 252/1024 = 1)
HUFF_LENGTHS = [1, 2, 9, 9] + [10] * 252


def huff_compress(data, lens=HUFF_LENGTHS):
    """chdcodec.cpp chd_huffman_compressor: the tree in huffman_context_base::export_tree_huffman's form, then
    the bytes' codes. The tree: each code length as a symbol of a small tree (length + 1; 0: repeat the last length
    2-8 times, or 9 + an 8-bit count), the small tree's own lengths first (3 bits each: symbol 0, then from `start`
    on, 7 = the rest are 0)."""
    tokens = []  # (small-tree symbol, extra bits, extra count)
    i = 0
    while i < len(lens):
        tokens.append((lens[i] + 1, 0, 0))
        run = 0
        while i + 1 + run < len(lens) and lens[i + 1 + run] == lens[i]:
            run += 1
        i += 1 + run
        while run:
            if run == 1:
                tokens.append((lens[i - 1] + 1, 0, 0))
                run = 0
            elif run <= 8:
                tokens.append((0, 3, run - 2))
                run = 0
            else:
                n = min(run, 9 + 255)
                # 7 in 3 bits, then the 8-bit rest (rlefullbits for 256 codes)
                tokens.append((0, 3 + 8, (7 << 8) | (n - 9)))
                run -= n
    used = sorted({t[0] for t in tokens})
    small = [0] * 24
    for sym, length in zip(used, balanced_lengths(len(used))):
        small[sym] = length
    bw = BitWriter()
    bw.write(small[0], 3)
    nonzero = [s for s in range(1, 24) if small[s]]
    start = min(nonzero[0], 8)
    bw.write(start - 1, 3)
    for s in range(start, nonzero[-1] + 1):
        bw.write(small[s], 3)
    if nonzero[-1] < 23:
        bw.write(7, 3)
    small_codes = canonical_codes(small)
    for sym, nbits, extra in tokens:
        code, length = small_codes[sym]
        bw.write(code, length)
        if nbits:
            bw.write(extra, nbits)
    codes = canonical_codes(lens)
    for byte in data:
        code, length = codes[byte]
        bw.write(code, length)
    return bw.data()


def flac_blocksize(nbytes):  # chdcodec.cpp chd_flac_compressor::blocksize
    n = nbytes // 4
    while n > 2048:
        n //= 2
    return n


def flac_compress(hunk, endian):
    """chd_flac_compressor: 'L' or 'B' (the samples' byte order in the hunk), then FLAC frames of 2 channels."""
    samples = list(struct.unpack(("<" if endian == "L" else ">") + "%dh" % (len(hunk) // 2), hunk))
    return endian.encode() + flac_frames(samples, flac_blocksize(len(hunk)), compress=True)


# the codecs of whole hunks (hunk, encoding -> bytes), and the CD codecs' sector compressors (cdfl: FLAC,
# cd_compress)
COMPRESSORS = {b"zlib": lambda hunk, enc: deflate(hunk), b"lzma": lambda hunk, enc: lzma_raw(hunk),
               b"huff": lambda hunk, enc: huff_compress(hunk),
               b"flac": lambda hunk, enc: flac_compress(hunk, enc.flac_endian)}
CD_COMPRESSORS = {b"cdlz": lzma_raw, b"cdzl": deflate, b"cdfl": None}


def compress(codec, hunk, frames, enc=Encoding()):
    if codec in COMPRESSORS:
        return COMPRESSORS[codec](hunk, enc)
    if codec in CD_COMPRESSORS:
        return cd_compress(codec, hunk, frames, enc)
    return b"\x00" * 7  # an unknown codec's payload: the reader must refuse it by name, not decode it


def build_chd(hunks, hunk_bytes, unit_bytes, compressors, plan, metadata, logical_bytes=None, break_crc=None,
              break_map_crc=False, enc=Encoding()):
    """hunks: the raw hunk data. plan: per hunk, ("c", codec index) / ("none",) / ("self", hunk number).
    metadata: [(tag, bytes)]. enc: Encoding (its ecc_frames: frame numbers of the whole disc).
    Returns the CHD file's bytes."""
    frames = hunk_bytes // CD_FRAME if unit_bytes == CD_FRAME else 0
    header_len = 124
    payloads, entries = [], []
    for n, (hunk, how) in enumerate(zip(hunks, plan)):
        if how[0] == "c":
            mine = [f - n * frames for f in enc.ecc_frames if frames and n * frames <= f < (n + 1) * frames]
            data = compress(compressors[how[1]], hunk, frames, enc._replace(ecc_frames=mine))
            crc = crc16(hunk) ^ (1 if break_crc == n else 0)
            entries.append((how[1], len(data), crc))
            payloads.append(data)
        elif how[0] == "none":
            entries.append((NONE, hunk_bytes, crc16(hunk)))
            payloads.append(hunk)
        else:
            entries.append((SELF, how[1], None))
    max_len = max([e[1] for e in entries if e[0] != SELF] + [1])
    lengthbits = max(1, max_len.bit_length())
    selfbits = max(1, (len(hunks) - 1).bit_length())

    bw = BitWriter()
    for _ in range(16):
        bw.write(4, 4)  # every code 4 bits long: symbol s is the 4-bit value s
    for t, _, _ in entries:
        bw.write(t, 4)
    for t, val, crc in entries:
        if t == SELF:
            bw.write(val, selfbits)
        elif t == NONE:
            bw.write(crc, 16)
        else:
            bw.write(val, lengthbits)
            bw.write(crc, 16)
    mapbits = bw.data()

    meta_blob = b""
    meta_offset = header_len
    for i, (tag, data) in enumerate(metadata):
        here = meta_offset + len(meta_blob)
        nxt = here + 16 + len(data) if i + 1 < len(metadata) else 0
        meta_blob += tag + struct.pack(">I", len(data)) + struct.pack(">Q", nxt) + data
    map_offset = header_len + len(meta_blob)
    first = map_offset + 16 + len(mapbits)

    raw_map = b""
    cur = first
    for t, val, crc in entries:
        if t == SELF:
            raw_map += struct.pack(">B", SELF) + b"\0\0\0" + val.to_bytes(6, "big") + b"\0\0"
        else:
            length = val
            raw_map += struct.pack(">B", t) + length.to_bytes(3, "big") + cur.to_bytes(6, "big") + struct.pack(">H", crc)
            cur += length
    map_crc = crc16(raw_map) ^ (1 if break_map_crc else 0)
    map_header = struct.pack(">I", len(mapbits)) + first.to_bytes(6, "big") + struct.pack(">H", map_crc) + \
        bytes([lengthbits, selfbits, 0, 0])

    header = bytearray(header_len)
    header[0:8] = b"MComprHD"
    struct.pack_into(">II", header, 8, header_len, 5)
    for i, c in enumerate(compressors[:4]):
        header[16 + 4 * i:20 + 4 * i] = c
    logical = logical_bytes if logical_bytes is not None else len(hunks) * hunk_bytes
    struct.pack_into(">QQQ", header, 32, logical, map_offset, meta_offset if metadata else 0)
    struct.pack_into(">II", header, 56, hunk_bytes, unit_bytes)
    return bytes(header) + meta_blob + map_header + mapbits + b"".join(payloads)
