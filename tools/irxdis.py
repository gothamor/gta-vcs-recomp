#!/usr/bin/env python3
"""Disassemble an IOP module (IRX) with import stubs named.

Writes a listing for reverse-engineering the game's IOP modules. The listing
is derived from game code: keep it under build/analysis/ (not committed).

    python -I tools/irxdis.py <module.irx> [base_hex] > build/analysis/<module>.txt
    python -I tools/irxdis.py --map <module.irx>           # functions: size, callees, imports, strings

Standard library only: the decoder is tools/mipsdis.py (MIPS I, the IOP's instruction set).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mipsdis  # noqa: E402
import toolchain as tc  # noqa: E402

# Common IOP library export names by (library, ordinal). Partial; extend as needed.
NAMES = {
    "sysmem": {4: "AllocSysMemory", 5: "FreeSysMemory", 14: "Kprintf"},
    "loadcore": {6: "RegisterLibraryEntries", 12: "QueryBootMode"},
    "intrman": {4: "RegisterIntrHandler", 9: "EnableIntr", 17: "CpuSuspendIntr", 18: "CpuResumeIntr", 23: "QueryIntrContext"},
    "thbase": {4: "CreateThread", 5: "DeleteThread", 6: "StartThread", 8: "ExitThread", 10: "TerminateThread",
               14: "ChangeThreadPriority", 18: "ReferThreadStatus", 20: "GetThreadId", 24: "SleepThread", 25: "WakeupThread",
               26: "iWakeupThread", 33: "DelayThread", 34: "GetSystemTime", 35: "SetAlarm", 37: "CancelAlarm",
               39: "USec2SysClock", 40: "SysClock2USec"},
    "thsemap": {4: "CreateSema", 5: "DeleteSema", 6: "SignalSema", 7: "iSignalSema", 8: "WaitSema", 9: "PollSema",
                11: "ReferSemaStatus"},
    "thevent": {4: "CreateEventFlag", 5: "DeleteEventFlag", 6: "SetEventFlag", 7: "iSetEventFlag", 8: "ClearEventFlag",
                9: "iClearEventFlag", 10: "WaitEventFlag", 11: "PollEventFlag"},
    "sifman": {7: "sceSifSetDma", 8: "sceSifDmaStat", 29: "sceSifCheckInit"},
    "sifcmd": {14: "sceSifInitRpc", 15: "sceSifBindRpc", 16: "sceSifCallRpc", 17: "sceSifRegisterRpc",
               18: "sceSifCheckStatRpc", 19: "sceSifSetRpcQueue", 20: "sceSifGetNextRequest", 21: "sceSifExecRequest",
               22: "sceSifRpcLoop", 23: "sceSifGetOtherData"},
    "cdvdman": {4: "sceCdInit", 5: "sceCdStandby", 6: "sceCdRead", 7: "sceCdSeek", 8: "sceCdGetError", 9: "sceCdGetToc",
                10: "sceCdSearchFile", 11: "sceCdSync", 12: "sceCdGetDiskType", 13: "sceCdDiskReady", 14: "sceCdTrayReq",
                15: "sceCdStop", 16: "sceCdPosToInt", 17: "sceCdIntToPos", 21: "sceCdCheckCmd", 24: "sceCdReadClock",
                28: "sceCdStatus", 37: "sceCdCallback", 38: "sceCdPause", 39: "sceCdBreak", 44: "sceCdGetReadPos",
                50: "sceCdSC", 75: "sceCdMmode", 112: "sceCdLayerSearchFile"},
    "stdio": {4: "printf"},
    "sysclib": {12: "memcpy", 13: "memmove", 14: "memset", 23: "strcmp", 27: "strcpy", 30: "strlen", 36: "strncpy",
                19: "sprintf"},
    # libsd, in LIBSD.IRX's export order (4 = sceSdInit): told apart by CDSTREAM's calls - 36 x SetParam(entry, value),
    # GetParam(core | voice << 1 | 0x500 = ENVX), Set{Trans,IRQ}Callback (21 / 22) as docs/game-internals.md 9 has them
    "libsd": {4: "sceSdInit", 5: "sceSdSetParam", 6: "sceSdGetParam", 7: "sceSdSetSwitch", 8: "sceSdGetSwitch",
              9: "sceSdSetAddr", 10: "sceSdGetAddr", 11: "sceSdSetCoreAttr", 12: "sceSdGetCoreAttr", 13: "sceSdNote2Pitch",
              14: "sceSdPitch2Note", 15: "sceSdProcBatch", 16: "sceSdProcBatchEx", 17: "sceSdVoiceTrans",
              18: "sceSdBlockTrans", 19: "sceSdVoiceTransStatus", 20: "sceSdBlockTransStatus", 21: "sceSdSetTransCallback",
              22: "sceSdSetIRQCallback", 23: "sceSdSetEffectAttr", 24: "sceSdGetEffectAttr", 25: "sceSdClearEffectWorkArea",
              26: "sceSdSetTransIntrHandler", 27: "sceSdSetSpu2IntrHandler"},
}


def load(path):
    """(image, import stubs {address: "lib:name"}). The image is the module's sections at their load addresses."""
    d = open(path, "rb").read()
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    secs = [struct.unpack_from("<10I", d, shoff + i * 40) for i in range(shnum)]
    strtab = secs[shstrndx][4]
    name = lambda o: d[strtab + o:d.index(b"\0", strtab + o)].decode()
    image = bytearray()
    for s in secs:
        if name(s[0]) in (".text", ".rodata", ".data") and s[3] + s[5] <= 0x100000:
            image.extend(b"\0" * max(0, s[3] + s[5] - len(image)))
            image[s[3]:s[3] + s[5]] = d[s[4]:s[4] + s[5]]
    text = next(s for s in secs if name(s[0]) == ".text")
    code = d[text[4]:text[4] + text[5]]
    return bytes(image), len(code), stubs_of(code)


def exports_of(image, size):
    """The module's own export table: 0x41C00000, 0, version, name[8], then function addresses up to a 0 word.
    Entries 0-3 are the module's start / stop / ... hooks; 4 on are its library functions."""
    exports = {}
    for i in range(0, len(image) - 20, 4):
        if struct.unpack_from("<II", image, i) == (0x41C00000, 0):
            lib = image[i + 12:i + 20].split(bytes(1))[0].decode("latin1")
            j, ordinal = i + 20, 0
            while j + 4 <= len(image):
                a = struct.unpack_from("<I", image, j)[0]
                if a == 0 or a >= size:
                    break
                exports[a] = "%s:%s" % (lib, NAMES.get(lib, {}).get(ordinal, str(ordinal)))
                j, ordinal = j + 4, ordinal + 1
    return exports


def stubs_of(code):
    """Import stubs: 0x41E00000, 0, version, name[8], then (jr ra; addiu zero,zero,N)* , 0, 0"""
    stubs = {}
    i = 0
    while i + 20 <= len(code):
        if struct.unpack_from("<I", code, i)[0] == 0x41E00000 and struct.unpack_from("<I", code, i + 4)[0] == 0:
            lib = code[i + 12:i + 20].split(b"\0")[0].decode("latin1")
            j = i + 20
            while j + 8 <= len(code):
                a, b = struct.unpack_from("<II", code, j)
                if a != 0x03E00008:
                    break
                ordinal = b & 0xFFFF
                stubs[j] = "%s:%s" % (lib, NAMES.get(lib, {}).get(ordinal, str(ordinal)))
                j += 8
            i = j
        else:
            i += 4
    return stubs


def main(path, base=0):
    image, size, stubs = load(path)
    for addr in range(0, size - 3, 4):
        word = struct.unpack_from("<I", image, addr)[0]
        mnemonic, operands, target = mipsdis.decode(word, addr)
        note = ""
        if mnemonic in ("jal", "j") and target in stubs:
            note = "  ; " + stubs[target]
        if addr in stubs:
            note = "  ; <stub %s>" % stubs[addr]
        print("%05x %05x: %-8s %-30s%s" % (addr, addr + base, mnemonic, operands, note))


def string_at(image, addr):
    if not 0 <= addr < len(image):
        return None
    end = image.find(b"\0", addr)
    s = image[addr:end]
    if len(s) >= 4 and all(32 <= c < 127 or c in (9, 10) for c in s):
        return s.decode("latin1")
    return None


def analyse(image, size, stubs):
    """(function starts, code addresses formed as constants, {address: constant formed there}, decoded instructions,
    the first import stub's address). Starts are jal targets and constants pointing just past a jr ra + delay slot."""
    first_stub = min(stubs) - 20 if stubs else size
    insns = [mipsdis.decode(struct.unpack_from("<I", image, a)[0], a) for a in range(0, size, 4)]
    words = [struct.unpack_from("<I", image, a)[0] for a in range(0, size, 4)]
    exports = exports_of(image, size)
    starts, pointers = {0} | set(exports), set()
    hi = {}
    refs = {}  # address -> constants formed there
    for k, (m, ops, t) in enumerate(insns):
        a, w = k * 4, words[k]
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        if m == "jal" and t is not None:
            starts.add(t)
        if m == "lui":
            hi[rt] = (w & 0xFFFF) << 16
        elif w >> 26 in (0x09, 0x0D) or (w >> 26) in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B):
            if rs in hi:
                lo = w & 0xFFFF
                v = (hi[rs] | lo) if w >> 26 == 0x0D else (hi[rs] + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
                refs[a] = v
                if w >> 26 in (0x09, 0x0D) and v < first_stub and v % 4 == 0 and w >> 26 == 0x09:
                    pointers.add(v)
            if w >> 26 in (0x09, 0x0D) and rt in hi and rt != rs:
                del hi[rt]
        if m in ("jr", "j", "b"):
            hi.clear()
    # a code address formed as a constant counts as a function when the code before it cannot fall through (jr / j +
    # delay slot) and it begins with a stack frame (addiu sp, sp, -n): handlers, thread entries, callbacks
    for v in pointers:
        if v >= 8 and (words[v // 4 - 2] >> 26 == 2 or words[v // 4 - 2] & 0xFC00003F == 0x08) and                 words[v // 4] & 0xFFFF8000 == 0x27BD8000:
            starts.add(v)
        elif v >= 8 and words[v // 4 - 2] == 0x03E00008:
            starts.add(v)
    return sorted(s for s in starts if s < first_stub), pointers, refs, insns, first_stub


def function_starts(image, size, stubs):
    return analyse(image, size, stubs)[0]


def function_map(path):
    """Functions (jal targets and code addresses loaded as constants), each with its size, callees, imports, the
    data addresses it forms (lui + addiu / ori / load / store) and the strings among them."""
    image, size, stubs = load(path)
    starts, pointers, refs, insns, first_stub = analyse(image, size, stubs)
    for i, s in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else first_stub
        calls, imports, data, texts = [], [], set(), []
        for a in range(s, end, 4):
            m, ops, t = insns[a // 4]
            if m == "jal" and t is not None:
                (imports if t in stubs else calls).append(stubs.get(t, "%05x" % t))
            if a in refs:
                v = refs[a]
                st = string_at(image, v)
                if st is not None:
                    texts.append(st[:60])
                elif v not in starts:
                    data.add(v)
        tag = " <pointer>" if s in pointers else ""
        exported = exports_of(image, size).get(s)
        if exported:
            tag += " <export %s>" % exported
        print("%05x  %4d insns%s" % (s, (end - s) // 4, tag))
        if calls:
            print("    calls   " + " ".join(sorted(set(calls))))
        if imports:
            counts = {}
            for x in imports:
                counts[x] = counts.get(x, 0) + 1
            print("    imports " + ", ".join("%s%s" % (k, " x%d" % c if c > 1 else "") for k, c in sorted(counts.items())))
        if data:
            print("    data    " + " ".join("%x" % v for v in sorted(data)))
        for t in texts:
            print("    string  %r" % t)


def decompile(path, out):
    """Every function's C through headless Ghidra (GHIDRA_INSTALL_DIR, else scoop's ghidra), as a raw MIPS image at
    address 0 with the import stubs named imp_<lib>_<name> and the function starts of --map."""
    import subprocess
    import tempfile
    image, size, stubs = load(path)
    ghidra = tc.ghidra_home("GHIDRA_INSTALL_DIR")
    headless = os.path.join(ghidra, "support", "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless")
    work = tempfile.mkdtemp(prefix="irxdis_")
    bin_path = os.path.join(work, os.path.basename(path) + ".bin")
    with open(bin_path, "wb") as f:
        f.write(image)
    with open(os.path.join(work, "symbols.txt"), "w") as f:
        for a, n in sorted(stubs.items()):
            f.write("%x imp_%s\n" % (a, n.replace(":", "_")))
        exports = exports_of(image, size)
        for a in function_starts(image, size, stubs):
            f.write("%x %s\n" % (a, "exp_" + exports[a].split(":")[1] if a in exports else "fn_%05x" % a))
    subprocess.run([headless, work, "irx", "-import", bin_path, "-processor", "MIPS:LE:32:default",
                    "-loader", "BinaryLoader", "-scriptPath", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ghidra"),
                    "-postScript", "DecompileAll.java", os.path.join(work, "symbols.txt"), os.path.abspath(out),
                    "-deleteProject"], check=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--map":
        function_map(sys.argv[2])
    elif sys.argv[1] == "--decompile":
        decompile(sys.argv[2], sys.argv[3])
    else:
        main(sys.argv[1], int(sys.argv[2], 16) if len(sys.argv) > 2 else 0)
