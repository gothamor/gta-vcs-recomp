#!/usr/bin/env python3
"""Tiny Win32 debugger: run a program, report the crashing thread on a fatal exception.

Catches what in-process handlers cannot (e.g. __fastfail / 0xC0000409) and prints
the fault code, the __fastfail reason, RIP and the return addresses found on the
stack, symbolized with DbgHelp (needs the PDB next to the exe).

    python tools/minidbg.py [--timeout S] <exe> [args...]
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

if os.name != "nt":
    sys.exit("minidbg.py is Windows only; on Linux run the game under gdb (gdb --args build/vcs <iso>), and the crash "
             "handler writes a backtrace to the log (docs/LINUX.md)")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
dbghelp.SymGetModuleBase64.restype = ctypes.c_uint64
dbghelp.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_uint64]

DEBUG_ONLY_THIS_PROCESS = 0x2
CREATE_NEW_CONSOLE = 0x10
EXCEPTION_DEBUG_EVENT = 1
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
CONTEXT_FULL = 0x10000B
THREAD_ALL_ACCESS = 0x1FFFFF

FATAL = {0xC0000005: "access violation", 0xC0000409: "fail-fast / stack buffer overrun", 0xC000001D: "illegal instruction",
         0xC00000FD: "stack overflow", 0xC0000094: "integer divide by zero", 0xC0000374: "heap corruption",
         0x80000003: "breakpoint"}
FASTFAIL = {2: "STACK_COOKIE_CHECK_FAILURE", 3: "CORRUPT_LIST_ENTRY", 5: "INVALID_ARG", 7: "FATAL_APP_EXIT (abort)",
            10: "INVALID_SET_OF_CONTEXT", 13: "GUARD_ICALL_CHECK_FAILURE", 14: "INVALID_BUFFER_ACCESS",
            23: "INVALID_FILE_OPERATION", 50: "UNHANDLED_LSS_EXCEPTON", 22: "UNEXPECTED_HEAP_EXCEPTION"}


class STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE),
                ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


class EXCEPTION_RECORD(ctypes.Structure):
    pass


EXCEPTION_RECORD._fields_ = [("ExceptionCode", wt.DWORD), ("ExceptionFlags", wt.DWORD),
                             ("ExceptionRecord", ctypes.POINTER(EXCEPTION_RECORD)), ("ExceptionAddress", ctypes.c_void_p),
                             ("NumberParameters", wt.DWORD), ("ExceptionInformation", ctypes.c_uint64 * 15)]


class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", wt.DWORD)]


class DEBUG_EVENT(ctypes.Structure):
    class U(ctypes.Union):
        _fields_ = [("Exception", EXCEPTION_DEBUG_INFO), ("raw", ctypes.c_byte * 160)]
    _fields_ = [("dwDebugEventCode", wt.DWORD), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD), ("u", U)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [("P1Home", ctypes.c_uint64 * 6), ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
                ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD), ("SegFs", wt.WORD), ("SegGs", wt.WORD),
                ("SegSs", wt.WORD), ("EFlags", wt.DWORD), ("Dr", ctypes.c_uint64 * 6),
                ("Rax", ctypes.c_uint64), ("Rcx", ctypes.c_uint64), ("Rdx", ctypes.c_uint64), ("Rbx", ctypes.c_uint64),
                ("Rsp", ctypes.c_uint64), ("Rbp", ctypes.c_uint64), ("Rsi", ctypes.c_uint64), ("Rdi", ctypes.c_uint64),
                ("R", ctypes.c_uint64 * 8), ("Rip", ctypes.c_uint64), ("rest", ctypes.c_byte * 976)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG), ("Scope", wt.ULONG),
                ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG), ("Name", ctypes.c_char * 512)]


def main(argv):
    timeout = 120.0
    if argv and argv[0] == "--timeout":
        timeout = float(argv[1])
        argv = argv[2:]
    cmd = " ".join('"%s"' % a if " " in a else a for a in argv)
    si = STARTUPINFO(cb=ctypes.sizeof(STARTUPINFO))
    pi = PROCESS_INFORMATION()
    if not k32.CreateProcessW(None, ctypes.create_unicode_buffer(cmd), None, None, False, DEBUG_ONLY_THIS_PROCESS,
                              None, None, ctypes.byref(si), ctypes.byref(pi)):
        sys.exit("CreateProcess failed: %d" % ctypes.get_last_error())
    dbghelp.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES
    sym_ready = False
    seen_loader_bp = False
    start = time.time()
    ev = DEBUG_EVENT()
    while True:
        if not k32.WaitForDebugEvent(ctypes.byref(ev), 1000):
            if time.time() - start > timeout:
                print("[minidbg] timeout, killing")
                k32.TerminateProcess(pi.hProcess, 1)
                return
            continue
        status = DBG_CONTINUE
        code = ev.dwDebugEventCode
        if code == CREATE_PROCESS_DEBUG_EVENT:
            pass
        elif code == LOAD_DLL_DEBUG_EVENT:
            pass
        elif code == EXIT_PROCESS_DEBUG_EVENT:
            exit_code = ctypes.c_uint32.from_buffer_copy(bytes(b & 0xFF for b in ev.u.raw[:4])).value
            print("[minidbg] process exited with 0x%08x after %.1fs" % (exit_code, time.time() - start))
            return
        elif code == EXCEPTION_DEBUG_EVENT:
            rec = ev.u.Exception.ExceptionRecord
            exc = rec.ExceptionCode & 0xFFFFFFFF
            first = ev.u.Exception.dwFirstChance
            fatal = exc in FATAL and (exc != 0x80000003 or seen_loader_bp)
            if exc == 0x80000003:
                seen_loader_bp = True
            if fatal and (not first or exc in (0xC0000409, 0x80000003)):
                if not sym_ready:
                    dbghelp.SymInitialize(pi.hProcess, None, True)
                    sym_ready = True
                print("[minidbg] %s exception 0x%08x (%s) at 0x%x thread %d" % (
                    "first-chance" if first else "second-chance", exc, FATAL.get(exc, "?"), rec.ExceptionAddress or 0,
                    ev.dwThreadId))
                if exc == 0xC0000409 and rec.NumberParameters:
                    r = rec.ExceptionInformation[0]
                    print("[minidbg]   __fastfail code %d %s" % (r, FASTFAIL.get(r, "")))
                th = k32.OpenThread(THREAD_ALL_ACCESS, False, ev.dwThreadId)
                ctx = CONTEXT()
                ctx.ContextFlags = CONTEXT_FULL
                k32.GetThreadContext(th, ctypes.byref(ctx))

                def sym(addr):
                    s = SYMBOL_INFO(SizeOfStruct=ctypes.sizeof(SYMBOL_INFO) - 512 + 1, MaxNameLen=500)
                    disp = ctypes.c_uint64()
                    if dbghelp.SymFromAddr(pi.hProcess, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(s)):
                        return "%s+0x%x" % (s.Name.decode(errors="replace"), disp.value)
                    base = dbghelp.SymGetModuleBase64(pi.hProcess, ctypes.c_uint64(addr))
                    if base:
                        name = ctypes.create_unicode_buffer(260)
                        psapi = ctypes.WinDLL("psapi")
                        psapi.GetModuleBaseNameW(pi.hProcess, ctypes.c_void_p(base), name, 260)
                        return "%s+0x%x" % (name.value, addr - base)
                    return None

                print("[minidbg]   rip 0x%x %s" % (ctx.Rip, sym(ctx.Rip)))
                # Scan the stack for return addresses that symbolize.
                buf = (ctypes.c_uint64 * 4096)()
                got = ctypes.c_size_t()
                k32.ReadProcessMemory(pi.hProcess, ctypes.c_void_p(ctx.Rsp), buf, ctypes.sizeof(buf), ctypes.byref(got))
                shown = 0
                for i in range(got.value // 8):
                    name = sym(buf[i])
                    if name and "+0x0" != name[-4:]:
                        print("[minidbg]   [rsp+0x%04x] %s" % (i * 8, name))
                        shown += 1
                        if shown >= 40:
                            break
                k32.TerminateProcess(pi.hProcess, 1)
                status = DBG_EXCEPTION_NOT_HANDLED
            elif not first:
                status = DBG_EXCEPTION_NOT_HANDLED
            else:
                if exc not in (0x406D1388, 0x40010006, 0x4001000A):  # thread naming / debug strings
                    print("[minidbg] first-chance 0x%08x at 0x%x thread %d" % (exc, rec.ExceptionAddress or 0, ev.dwThreadId))
                status = DBG_EXCEPTION_NOT_HANDLED  # let the program's own handlers run
        k32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status)


if __name__ == "__main__":
    main(sys.argv[1:])
