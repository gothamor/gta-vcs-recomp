#!/usr/bin/env python3
"""Read a CHD (MAME's compressed disc image) as the 2048-byte sectors of a PS2 disc. Standard library only.

PS2 discs come as CHD in two kinds (docs/DISC.md "CHD images"):
  - DVD CHDs (chdman createdvd): 2048-byte units, metadata tag "DVD ";
  - CD-mode CHDs (chdman createcd, also used for PS2 DVDs by many collections): one data track of 2448-byte frames
    (2352 sector + 96 subcode), metadata "CHT2" / "CHTR"; the 2048 data bytes sit at 0 (MODE1) or 16 (MODE1_RAW).

Version 5 only (what chdman has written since 2012). Codecs: zlib, lzma, huff, flac (createdvd's defaults: lzma, zlib,
huff, flac), cdzl, cdlz, cdfl (createcd's) and, with Python 3.14's compression.zstd, zstd / cdzs. Others (avhu for
laserdiscs) raise ChdError naming the codec.
Every hunk is checked against its CRC16 from the map, and the map against its own CRC16 - except CD hunks of raw tracks
whose frames chdman stored without sync / ECC: the data bytes are right, the CRC (of the frames with ECC) can't be
checked without regenerating the ECC (the game's libchdr does; this reader is for checks and the executable, whose
SHA-1 vcs_disc.py compares). A stored pregap is refused rather than guessed (PS2 discs have none).

Formats followed: MAME src/lib/util/chd.cpp (header, decompress_v5_map), huffman.cpp (import_tree_rle,
import_tree_huffman, canonical codes), chdcodec.cpp (chd_zlib/lzma/huffman/flac/cd decompressors), cdrom.h.

    python tools/chd.py info  X.chd
    python tools/chd.py iso   X.chd OUT.iso     the whole disc as an ISO (checks every hunk)
"""
import binascii
import collections
import lzma
import re
import struct
import sys
import zlib

try:
    from compression import zstd as _zstd  # Python 3.14+
except ImportError:  # pragma: no cover - depends on the Python version
    _zstd = None

SECTOR = 2048
CD_FRAME = 2448
CD_SECTOR = 2352
CD_SUB = 96

TYPE0, TYPE1, TYPE2, TYPE3, NONE, SELF, PARENT, RLE_SMALL, RLE_LARGE, SELF_0, SELF_1, PARENT_SELF, PARENT_0, PARENT_1 = \
    range(14)

MAGIC = b"MComprHD"  # a CHD's first 8 bytes
CD_TRACK_TAGS = (b"CHT2", b"CHTR")
CD_TAGS = CD_TRACK_TAGS + (b"CHCD", b"CHGD")  # chdman createcd; CHGD: GD-ROM
DVD_TAG = b"DVD "  # chdman createdvd
# where the 2048 data bytes start in a frame, by track type
CD_DATA_OFFSET = {"MODE1": 0, "MODE1/2048": 0, "MODE1_RAW": 16, "MODE1/2352": 16,
                  "MODE2_FORM1": 0, "MODE2/2048": 0, "MODE2_RAW": 24, "MODE2/2352": 24}


class ChdError(ValueError):
    pass


class _Bits:
    """MSB-first bit reader; past the end reads zeros, as MAME's bitstream_in. Reads only the bytes it needs (a
    whole-buffer integer would make a large map's decode quadratic)."""

    def __init__(self, data):
        self.data = bytes(data)
        self.n = len(self.data) * 8
        self.pos = 0

    def read(self, count):
        if count == 0:
            return 0
        start = self.pos >> 3
        end = (self.pos + count + 7) >> 3
        chunk = self.data[start:end]
        if len(chunk) < end - start:
            chunk += bytes(end - start - len(chunk))
        shift = (end - start) * 8 - (self.pos & 7) - count
        self.pos += count
        return (int.from_bytes(chunk, "big") >> shift) & ((1 << count) - 1)

    def sread(self, count):
        v = self.read(count)
        return v - (1 << count) if count and v >> (count - 1) else v


class _Huffman:
    """MAME's huffman_decoder: canonical codes from the code lengths (huffman_assign_canonical_codes). The tree comes
    in one of two forms: from_rle() (the map's, import_tree_rle) or from_tree() (the huff codec's,
    import_tree_huffman: the lengths coded with a small huffman tree of their own).
    `what` names the user in errors ("map", "huff")."""

    def __init__(self, lens, maxbits, what):
        self.what = what
        histo = [0] * 33
        for length in lens:
            if length > maxbits:
                raise ChdError("%s: huffman code too long" % what)
            histo[length] += 1
        start = 0
        for cl in range(32, 0, -1):
            nxt = (start + histo[cl]) >> 1
            if cl != 1 and nxt * 2 != start + histo[cl]:
                raise ChdError("%s: huffman tree inconsistent" % what)
            histo[cl] = start
            start = nxt
        self.codes = {}
        for sym, length in enumerate(lens):
            if length:
                self.codes[(length, histo[length])] = sym
                histo[length] += 1
        self.maxbits = maxbits

    @classmethod
    def from_rle(cls, numcodes, maxbits, bits, what="map"):
        numbits = 5 if maxbits >= 16 else 4 if maxbits >= 8 else 3
        lens = []
        while len(lens) < numcodes:
            nb = bits.read(numbits)
            if nb != 1:
                lens.append(nb)
                continue
            nb = bits.read(numbits)
            if nb == 1:
                lens.append(nb)
            else:
                lens.extend([nb] * (bits.read(numbits) + 3))
        if len(lens) != numcodes:
            raise ChdError("%s: huffman tree overflows" % what)
        return cls(lens, maxbits, what)

    @classmethod
    def from_tree(cls, numcodes, maxbits, bits, what="huff"):
        """The lengths coded with a small tree of 24 symbols (length + 1, or 0: repeat the last length), whose own
        lengths come first in 3 bits each: symbol 0's, then `start`, then one per symbol until a 7 ends them."""
        small = [bits.read(3)] + [0] * 23
        start = bits.read(3) + 1
        count = 0
        for index in range(1, 24):
            if index >= start and count != 7:
                count = bits.read(3)
                small[index] = 0 if count == 7 else count
        small_tree = cls(small, 6, what)
        rle_bits = (numcodes - 9).bit_length()
        lens = []
        last = 0
        while len(lens) < numcodes:
            value = small_tree.decode(bits)
            if value:
                last = value - 1
                lens.append(last)
            else:
                repeat = bits.read(3) + 2
                if repeat == 7 + 2:
                    repeat += bits.read(rle_bits)
                lens.extend([last] * min(repeat, numcodes - len(lens)))
        return cls(lens, maxbits, what)

    def decode(self, bits):
        code = 0
        for length in range(1, self.maxbits + 1):
            code = (code << 1) | bits.read(1)
            sym = self.codes.get((length, code))
            if sym is not None:
                return sym
        raise ChdError("%s: bad huffman code" % self.what)


def _unhuff(src, size):
    """chd_huffman_decompressor: a huffman-coded tree of the 256 byte values (codes up to 16 bits), then the bytes."""
    bits = _Bits(src)
    tree = _Huffman.from_tree(256, 16, bits)
    out = bytes(tree.decode(bits) for _ in range(size))
    if bits.pos > bits.n:
        raise ChdError("huff: ran off the end")
    return out


def _inflate(data, size):
    return zlib.decompressobj(-15).decompress(data, size)


def _unlzma(data, size):
    # chdman's LZMA: raw LZMA1, lc 3 / lp 0 / pb 2, no end marker; any dictionary at least the hunk size decodes it
    d = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 20,
                                                          "lc": 3, "lp": 0, "pb": 2}])
    return d.decompress(data, size)


def _unzstd(data, size):
    if _zstd is None:
        raise ChdError("codec 'zstd' needs Python 3.14 (compression.zstd); this is Python %d.%d" % sys.version_info[:2])
    return _zstd.decompress(data)[:size]


def _flac_decode(src, nsamples):
    """Raw FLAC frames (no stream header; 2 channels, 16 bits) -> (interleaved samples, bytes used)."""
    b = _Bits(src)

    def unary():
        q = 0
        while b.read(1) == 0:
            q += 1
            if b.pos > b.n:
                raise ChdError("flac: ran off the end")
        return q

    def residual(bs, order):
        method = b.read(2)
        pbits, esc = (4, 15) if method == 0 else (5, 31)
        porder = b.read(4)
        out = []
        for p in range(1 << porder):
            n = (bs >> porder) - (order if p == 0 else 0)
            k = b.read(pbits)
            if k == esc:
                raw = b.read(5)
                out.extend(b.sread(raw) for _ in range(n))
            else:
                for _ in range(n):
                    v = (unary() << k) | b.read(k)
                    out.append((v >> 1) ^ -(v & 1))
        return out

    def subframe(bs, bps):
        b.read(1)
        t = b.read(6)
        wasted = 0
        if b.read(1):
            wasted = unary() + 1
            bps -= wasted
        if t == 0:
            s = [b.sread(bps)] * bs
        elif t == 1:
            s = [b.sread(bps) for _ in range(bs)]
        elif 8 <= t <= 12:
            order = t - 8
            s = [b.sread(bps) for _ in range(order)]
            coefs = [[], [1], [2, -1], [3, -3, 1], [4, -6, 4, -1]][order]
            for r in residual(bs, order):
                s.append(r + sum(c * s[-1 - j] for j, c in enumerate(coefs)))
        elif t >= 32:
            order = t - 31
            s = [b.sread(bps) for _ in range(order)]
            prec = b.read(4) + 1
            shift = b.sread(5)
            coefs = [b.sread(prec) for _ in range(order)]
            for r in residual(bs, order):
                s.append(r + (sum(c * s[-1 - j] for j, c in enumerate(coefs)) >> shift))
        else:
            raise ChdError("flac: subframe type %d" % t)
        return [v << wasted for v in s] if wasted else s

    left, right = [], []
    while len(left) < nsamples:
        if b.read(14) != 0x3FFE:
            raise ChdError("flac: frame sync lost")
        b.read(2)
        bscode, srcode, chan, sscode = b.read(4), b.read(4), b.read(4), b.read(3)
        b.read(1)
        lead = b.read(8)  # the frame number, UTF-8 coded
        ones = 0
        while ones < 8 and (lead << ones) & 0x80:
            ones += 1
        for _ in range(max(0, ones - 1)):
            b.read(8)
        if bscode == 6:
            bs = b.read(8) + 1
        elif bscode == 7:
            bs = b.read(16) + 1
        elif bscode == 1:
            bs = 192
        elif 2 <= bscode <= 5:
            bs = 576 << (bscode - 2)
        elif bscode >= 8:
            bs = 256 << (bscode - 8)
        else:
            raise ChdError("flac: block size code %d" % bscode)
        if srcode == 12:
            b.read(8)
        elif srcode in (13, 14):
            b.read(16)
        bps = {0: 16, 1: 8, 2: 12, 4: 16, 5: 20, 6: 24}.get(sscode)
        if bps is None:
            raise ChdError("flac: sample size code %d" % sscode)
        b.read(8)  # CRC-8
        if chan == 1:
            lch, rch = subframe(bs, bps), subframe(bs, bps)
        elif chan == 8:
            lch, sd = subframe(bs, bps), subframe(bs, bps + 1)
            rch = [x - y for x, y in zip(lch, sd)]
        elif chan == 9:
            sd, rch = subframe(bs, bps + 1), subframe(bs, bps)
            lch = [x + y for x, y in zip(sd, rch)]
        elif chan == 10:
            m, sd = subframe(bs, bps), subframe(bs, bps + 1)
            lch, rch = [], []
            for x, y in zip(m, sd):
                x = (x << 1) | (y & 1)
                lch.append((x + y) >> 1)
                rch.append((x - y) >> 1)
        else:
            raise ChdError("flac: channel assignment %d" % chan)
        if b.pos % 8:
            b.read(8 - b.pos % 8)
        b.read(16)  # CRC-16
        left += lch
        right += rch
    inter = [v for pair in zip(left, right) for v in pair]
    return inter[:nsamples * 2], b.pos // 8


def _cd_assemble(base, sub, frames, hunk_bytes):
    if len(base) != frames * CD_SECTOR or len(sub) != frames * CD_SUB:
        raise ChdError("short CD hunk")
    out = bytearray(hunk_bytes)
    for i in range(frames):
        out[i * CD_FRAME:i * CD_FRAME + CD_SECTOR] = base[i * CD_SECTOR:(i + 1) * CD_SECTOR]
        out[i * CD_FRAME + CD_SECTOR:(i + 1) * CD_FRAME] = sub[i * CD_SUB:(i + 1) * CD_SUB]
    return bytes(out)


CD_SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


def _unflac(src, size):
    """chd_flac_decompressor: 'L' / 'B' (the samples' byte order in the hunk), then FLAC frames of 2 channels."""
    order = {b"L": "<", b"B": ">"}.get(src[:1])
    if order is None:
        raise ChdError("flac: byte order %r" % src[:1])
    samples, _ = _flac_decode(src[1:], size // 4)
    return struct.pack(order + "%dh" % len(samples), *samples)


# the codecs of whole hunks: compressed bytes, the hunk's size -> the hunk
CODECS = {b"zlib": _inflate, b"lzma": _unlzma, b"zstd": _unzstd, b"huff": _unhuff, b"flac": _unflac}
# the CD codecs (chdcodec.cpp chd_cd_decompressor): (the sectors' codec, the subcode's). cdfl's sectors are FLAC
# audio, not a byte codec (_decode_cd)
CD_CODECS = {b"cdlz": (_unlzma, _inflate), b"cdzl": (_inflate, _inflate), b"cdzs": (_unzstd, _unzstd),
             b"cdfl": (None, _inflate)}


def _decode_cd(codec, src, hunk_bytes):
    """-> (hunk, complete). complete is False when frames had their sync header and ECC removed by the compressor
    (raw tracks): the sync is put back, the ECC is not (cdrom.cpp's ecc_generate), so the 2048 data bytes - at 0, 16
    or 24, all before the ECC at 2076 - are right but the hunk's CRC16 (of the whole frames) can't be checked."""
    frames = hunk_bytes // CD_FRAME
    base_codec, sub_codec = CD_CODECS[codec]
    if base_codec is None:  # cdfl
        samples, used = _flac_decode(src, frames * CD_SECTOR // 4)
        base = struct.pack(">%dh" % len(samples), *samples)  # CHDs keep CD audio big-endian
        return _cd_assemble(base, sub_codec(src[used:], frames * CD_SUB), frames, hunk_bytes), True
    clen_bytes = 2 if hunk_bytes < 65536 else 3
    ecc_bytes = (frames + 7) // 8
    hdr = ecc_bytes + clen_bytes
    base_len = int.from_bytes(src[ecc_bytes:hdr], "big")
    base = base_codec(src[hdr:hdr + base_len], frames * CD_SECTOR)
    sub = sub_codec(src[hdr + base_len:], frames * CD_SUB)
    hunk = bytearray(_cd_assemble(base, sub, frames, hunk_bytes))
    complete = True
    for i in range(frames):
        if src[i // 8] & (1 << (i % 8)):
            hunk[i * CD_FRAME:i * CD_FRAME + len(CD_SYNC)] = CD_SYNC
            complete = False
    return bytes(hunk), complete


def _decode(codec, src, hunk_bytes):
    """-> (hunk, complete): complete is False when the hunk's CRC16 can't be checked (_decode_cd)."""
    if codec in CODECS:
        return CODECS[codec](src, hunk_bytes), True
    if codec in CD_CODECS:
        return _decode_cd(codec, src, hunk_bytes)
    raise ChdError("codec %r is not supported" % codec.decode("latin-1").strip("\0"))


def kind(path):
    """"cd" (made by chdman createcd: CD frames, extractcd), "dvd" (createdvd: extractdvd) or None (not a CHD, or
    another kind). Reads the header and the first metadata entry only, for any CHD version from 3 (the advice for a
    CHD that ChdImage refuses)."""
    try:
        with open(path, "rb") as f:
            header = f.read(124)
            if len(header) < 56 or header[:8] != MAGIC:
                return None
            version = struct.unpack(">I", header[12:16])[0]
            if version == 5:
                meta = struct.unpack(">Q", header[48:56])[0]
            elif version in (3, 4):
                meta = struct.unpack(">Q", header[36:44])[0]
            else:
                return None
            if not meta:
                return None
            f.seek(meta)
            tag = f.read(4)
    except OSError:
        return None
    if tag in CD_TAGS:
        return "cd"
    if tag == DVD_TAG:
        return "dvd"
    return None


class ChdImage:
    """The data sectors of a CHD: `sector_count`, `read_sectors(lba, count)` (short at the end, like a file),
    `kind` ("cd" / "dvd")."""

    CACHE_HUNKS = 64

    def __init__(self, path):
        self.f = open(path, "rb")
        try:
            self._open()
        except Exception:
            self.f.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        self.f.close()

    def _open(self):
        h = self.f.read(124)
        if len(h) < 16 or h[:8] != MAGIC:
            raise ChdError("not a CHD file")
        version = struct.unpack(">I", h[12:16])[0]
        if version != 5 or len(h) < 124:
            raise ChdError("CHD version %d: only version 5 is supported (chdman copy converts older ones)" % version)
        self.compressors = [h[16 + 4 * i:20 + 4 * i] for i in range(4)]
        self.logical, map_offset, meta_offset = struct.unpack(">QQQ", h[32:56])
        self.hunk_bytes, self.unit_bytes = struct.unpack(">II", h[56:64])
        if not self.hunk_bytes or not self.unit_bytes:
            raise ChdError("bad hunk / unit size")
        self.meta = self._read_metadata(meta_offset)
        self._read_map(map_offset)
        self._cache = collections.OrderedDict()

        tags = [t for t, _ in self.meta]
        if DVD_TAG in tags:
            self.kind = "dvd"
            if self.unit_bytes != SECTOR:
                raise ChdError("DVD CHD with %d-byte units" % self.unit_bytes)
            self.sector_count = self.logical // SECTOR
            self._frame, self._data_offset = SECTOR, 0
        elif any(t in CD_TAGS for t in tags):
            self.kind = "cd"
            track = next((d for t, d in self.meta if t in CD_TRACK_TAGS), None)
            if track is None:
                raise ChdError("CD CHD without CHT2 / CHTR track metadata")
            text = track.split(b"\0")[0].decode("ascii", "replace")
            fields = dict(re.findall(r"(\w+):(\S+)", text))
            ttype = fields.get("TYPE", "")
            if ttype not in CD_DATA_OFFSET:
                raise ChdError("first CD track is %s, not a data track" % (ttype or "unknown"))
            if fields.get("PGTYPE", "").startswith("V") and int(fields.get("PREGAP", "0")) > 0:
                # PS2 discs have none; where sector 0 then sits is not checked against chdman: refuse, don't guess
                # (src/disc/disc_source.cpp the same)
                raise ChdError("a CD track with %s pregap frames stored: not supported" % fields["PREGAP"])
            self.sector_count = int(fields.get("FRAMES", "0"))
            self._frame, self._data_offset = CD_FRAME, CD_DATA_OFFSET[ttype]
        else:
            raise ChdError("not a disc CHD (metadata %s)" % ", ".join(t.decode("latin-1") for t in tags))
        hunks = len(self.entries)
        self._per_hunk = per_hunk = self.hunk_bytes // self._frame  # sectors (DVD units or CD frames) per hunk
        if (self.sector_count + per_hunk - 1) // per_hunk > hunks:
            raise ChdError("the track is longer than the CHD's data (%d sectors, %d hunks)" % (self.sector_count, hunks))

    def _read_metadata(self, offset):
        out = []
        seen = set()
        while offset and offset not in seen and len(out) < 256:
            seen.add(offset)
            self.f.seek(offset)
            head = self.f.read(16)
            if len(head) < 16:
                raise ChdError("metadata runs past the end of the file")
            tag, flags_len, nxt = struct.unpack(">4sIQ", head)
            out.append((tag, self.f.read(flags_len & 0xFFFFFF)))
            offset = nxt
        return out

    def _read_map(self, offset):
        hunks = (self.logical + self.hunk_bytes - 1) // self.hunk_bytes
        if not self.compressors[0].strip(b"\0"):
            raise ChdError("uncompressed CHD: not supported")
        self.f.seek(offset)
        mh = self.f.read(16)
        if len(mh) < 16:
            raise ChdError("map runs past the end of the file")
        maplen = struct.unpack(">I", mh[0:4])[0]
        first = int.from_bytes(mh[4:10], "big")
        map_crc = struct.unpack(">H", mh[10:12])[0]
        lengthbits, selfbits, parentbits = mh[12], mh[13], mh[14]
        bits = _Bits(self.f.read(maplen))
        huff = _Huffman.from_rle(16, 8, bits)
        types = []
        last = rep = 0
        for _ in range(hunks):
            if rep > 0:
                types.append(last)
                rep -= 1
                continue
            v = huff.decode(bits)
            if v == RLE_SMALL:
                types.append(last)
                rep = 2 + huff.decode(bits)
            elif v == RLE_LARGE:
                types.append(last)
                rep = 2 + 16 + (huff.decode(bits) << 4)
                rep += huff.decode(bits)
            else:
                types.append(v)
                last = v
        entries = []
        raw = bytearray()
        cur = first
        last_self = last_parent = 0
        for n, t in enumerate(types):
            off, length, crc = cur, 0, 0
            if t in (TYPE0, TYPE1, TYPE2, TYPE3):
                length = bits.read(lengthbits)
                cur += length
                crc = bits.read(16)
            elif t == NONE:
                length = self.hunk_bytes
                cur += length
                crc = bits.read(16)
            elif t == SELF:
                off = last_self = bits.read(selfbits)
            elif t == PARENT:
                off = last_parent = bits.read(parentbits)
            elif t in (SELF_0, SELF_1):
                if t == SELF_1:
                    last_self += 1
                t, off = SELF, last_self
            elif t == PARENT_SELF:
                t = PARENT
                off = last_parent = (n * self.hunk_bytes) // self.unit_bytes
            elif t in (PARENT_0, PARENT_1):
                if t == PARENT_1:
                    last_parent += self.hunk_bytes // self.unit_bytes
                t, off = PARENT, last_parent
            else:
                raise ChdError("map: hunk %d has entry type %d" % (n, t))
            entries.append((t, off, length, crc))
            raw += bytes([t]) + length.to_bytes(3, "big") + off.to_bytes(6, "big") + crc.to_bytes(2, "big")
        if binascii.crc_hqx(bytes(raw), 0xFFFF) != map_crc:
            raise ChdError("map CRC mismatch: the file is damaged")
        self.entries = entries

    def hunk(self, n, depth=0):
        data = self._cache.get(n)
        if data is not None:
            self._cache.move_to_end(n)
            return data
        t, off, length, crc = self.entries[n]
        if t == SELF:
            if depth > 8 or off >= n:
                raise ChdError("hunk %d: bad self reference to %d" % (n, off))
            data = self.hunk(off, depth + 1)
        elif t == PARENT:
            raise ChdError("hunk %d: needs a parent CHD (not supported)" % n)
        else:
            self.f.seek(off)
            src = self.f.read(length)
            data, complete = (src, True) if t == NONE else _decode(self.compressors[t], src, self.hunk_bytes)
            if len(data) != self.hunk_bytes or (complete and binascii.crc_hqx(data, 0xFFFF) != crc):
                raise ChdError("hunk %d: CRC mismatch (the file is damaged)" % n)
        self._cache[n] = data
        if len(self._cache) > self.CACHE_HUNKS:
            self._cache.popitem(last=False)
        return data

    def read_sectors(self, lba, count):
        out = []
        end = min(lba + count, self.sector_count)
        per_hunk = self._per_hunk
        for s in range(max(lba, 0), end):
            h = self.hunk(s // per_hunk)
            at = (s % per_hunk) * self._frame + self._data_offset
            out.append(h[at:at + SECTOR])
        return b"".join(out)


def main(argv):
    if len(argv) == 3 and argv[1] == "info":
        with ChdImage(argv[2]) as img:
            used = collections.Counter(
                img.compressors[t].decode("latin-1") if t <= TYPE3 else {NONE: "none", SELF: "self", PARENT: "parent"}[t]
                for t, *_ in img.entries)
            print("kind %s, %d sectors, hunks of %d bytes: %s" % (img.kind, img.sector_count, img.hunk_bytes, dict(used)))
        return 0
    if len(argv) == 4 and argv[1] == "iso":
        with ChdImage(argv[2]) as img, open(argv[3], "wb") as out:
            step = 4096
            for lba in range(0, img.sector_count, step):
                out.write(img.read_sectors(lba, step))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
