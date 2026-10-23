#!/usr/bin/env python3
"""Disassemble an IOP module (IRX) with import stubs named.

Writes a listing for reverse-engineering the game's IOP modules. The listing
is derived from game code: keep it under build/analysis/ (not committed).

    python -I tools/irxdis.py <module.irx> [base_hex] > build/analysis/<module>.txt

Standard library only: the decoder is tools/mipsdis.py (MIPS I, the IOP's instruction set).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mipsdis  # noqa: E402

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


def main(path, base=0):
    d = open(path, "rb").read()
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    secs = [struct.unpack_from("<10I", d, shoff + i * 40) for i in range(shnum)]
    strtab = secs[shstrndx][4]
    name = lambda o: d[strtab + o:d.index(b"\0", strtab + o)].decode()
    text = next(s for s in secs if name(s[0]) == ".text")
    code = d[text[4]:text[4] + text[5]]

    # Import stubs: 0x41E00000, 0, version, name[8], then (jr ra; addiu zero,zero,N)* , 0, 0
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

    for addr in range(0, len(code) - 3, 4):
        word = struct.unpack_from("<I", code, addr)[0]
        mnemonic, operands, target = mipsdis.decode(word, addr)
        note = ""
        if mnemonic in ("jal", "j") and target in stubs:
            note = "  ; " + stubs[target]
        if addr in stubs:
            note = "  ; <stub %s>" % stubs[addr]
        print("%05x %05x: %-8s %-30s%s" % (addr, addr + base, mnemonic, operands, note))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1], int(sys.argv[2], 16) if len(sys.argv) > 2 else 0)
