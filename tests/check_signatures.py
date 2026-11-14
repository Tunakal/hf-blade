"""Start the game exe SUSPENDED (no game code runs), read the relocated image and check the
patch-site signatures exactly as the DLL does, then terminate the process.

Usage: py -3 tests/check_signatures.py "<path to METAL GEAR RISING REVENGEANCE.exe>"
"""
import ctypes, struct, sys
from ctypes import wintypes as w

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")

class STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", w.DWORD), ("r", w.LPWSTR), ("d", w.LPWSTR), ("t", w.LPWSTR)] + \
               [(n, w.DWORD) for n in ("x", "y", "xs", "ys", "xc", "yc", "fa", "fl")] + \
               [("sw", w.WORD), ("cb2", w.WORD), ("r2", ctypes.c_void_p),
                ("i", w.HANDLE), ("o", w.HANDLE), ("e", w.HANDLE)]

class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", w.HANDLE), ("hThread", w.HANDLE), ("pid", w.DWORD), ("tid", w.DWORD)]

class PBI(ctypes.Structure):
    _fields_ = [("r1", ctypes.c_void_p), ("Peb", ctypes.c_void_p), ("r2", ctypes.c_void_p * 2),
                ("pid", ctypes.c_void_p), ("r3", ctypes.c_void_p)]

PREFERRED = 0x400000
SITES = {"Limiter": (0xF98070, bytes.fromhex("558BEC83E4F8")),
         "Clamp":   (0xE03A07, bytes.fromhex("D9E8D8567C")),
         "Fixed":   (0xE039A9, bytes.fromhex("DDD8DDD9D9442410")),
         "ResTableH": (0x16EBF1C, bytes.fromhex("00008744")),
         "ResIdx5H":  (0xA30EB9, bytes.fromhex("B838040000")),
         "CamAspect": (0x16C3078, bytes.fromhex("398EE33F")),
         "ResTableW": (0x16EBF20, bytes.fromhex("0000F044")),
         "ResIdx5W":  (0xA30EAA, bytes.fromhex("B880070000")),
         "LB1": (0xA3A8CE, bytes.fromhex("2BC7")), "LB2": (0xA3A868, bytes.fromhex("2BC7")),
         "LB3": (0xF9B9FC, bytes.fromhex("2BC6")), "LB4": (0xF9B9A9, bytes.fromhex("2BC6")),
         "LB5": (0xF9D5C6, bytes.fromhex("2BC6")), "LB6": (0xF9D56F, bytes.fromhex("2BC6")),
         "QteDec948": (0x6127B7, bytes.fromhex("FF8E48090000")),
         "QteInc94C1": (0x612782, bytes.fromhex("FF864C090000")),
         "QteInc94C2": (0x611BA4, bytes.fromhex("FF864C090000")),
         "LerpWorld": (0xBC285A, bytes.fromhex("D88E30340000")),
         "LerpPlayer": (0xBC287C, bytes.fromhex("D88E30340000")),
         "BladeDrainRaiden": (0xBF256E, bytes.fromhex("D88E8C330000")),
         "EspKick": (0xF43A20, bytes.fromhex("83EC0C5333DB")),
         "FrameDelta": (0xE03999, bytes.fromhex("D9C0D8A680000000")),
         "CamPitch": (0xDB99CB, bytes.fromhex("D84C244CD91C24")),
         "CamYaw": (0xDB9A3D, bytes.fromhex("D84C244CD91C24")),
         "WsDetach": (0xB7D0E0, bytes.fromhex("568DB1F80F0000")),
         "WsBusy": (0xB7D0C0, bytes.fromhex("81C1F80F0000")),
         "WsOwned": (0x9C4900, bytes.fromhex("8B4424048D48F3")),
         "WsRequest": (0xB7F5E0, bytes.fromhex("E83B4309008B10")),
         "EspCallA": (0xF41412, bytes.fromhex("8B42088BCE")),
         "EspCallB": (0xF41470, bytes.fromhex("8B42088BCE")),
         "EspCallC": (0xF41513, bytes.fromhex("8B50088BCE")),
         "EspCtx": (0xEDFCA5, bytes.fromhex("D95E248B8F84000000")),
         "BladeDrainSam": (0x877147, bytes.fromhex("D88E8C330000")),
         "BladeDrainWolf": (0x8B8457, bytes.fromhex("D88E8C330000")),
         "TugPress": (0x584C3D, bytes.fromhex("D88E38090000")),
         "TugDecay": (0x584C4B, bytes.fromhex("D88E3C090000")),
         "DebrisPiece": (0x5DDA84, bytes.fromhex("399E280900000F8572030000")),
         "DebrisGroup": (0x942DCA, bytes.fromhex("8D44241050")),
         "DebrisReduce": (0x5D9560, bytes.fromhex("568BF183BE8008000000")),
         "UiExec": (0xF9AE76, bytes.fromhex("8B480C8B4008")),
         "EmSetDebrisGate": (0xCA7BFB, bytes.fromhex("395C2414750F")),
         "ZanRegister": (0x941450, bytes.fromhex("5153555657 8D6950".replace(" ", ""))),
         "Vanish0030": (0xB67710, bytes.fromhex("D9EE5683EC08")),
         "RepeatPad": (0xDDAE35, bytes.fromhex("FF412C8B412C")),
         "RepeatKb": (0xDDA23F, bytes.fromhex("FF402C8B482C")),
         "ChMaxHp": (0xB7C980, bytes.fromhex("56578BF1")),
         "ChConsume": (0x94D8D0, bytes.fromhex("FF49548B4154")),
         "SndPost": (0xE5E050, bytes.fromhex("83EC0C8B4424108B4C2414")),
         "SndPostBgm": (0xE5E1B0, bytes.fromhex("8B442404566A0050E8")),
         "CombatSet": (0xC4D1A0, bytes.fromhex("837C2404005 68BF1".replace(" ",""))),
         "DropTarget": (0xB2E550, bytes.fromhex("56578BF98D8F94100000"))}
RATE_CALLS = (0x5CC9B8, 0x5CD009, 0x5CD063, 0x5CD1DE, 0x5CD35E, 0x5CD4DF, 0x5CD64E, 0x5CD7CE, 0x5CDDE8,
              0x5CDF77, 0x5CE193, 0x5CE67E, 0x5CE7FE, 0x5CE97D, 0x5CEBFD, 0x8CE295, 0xAE44C7)
CORPSE_CALLS = {0xB3F668: 0xB346D0, 0xB3F8C7: 0xB346D0, 0xB3F9B8: 0xB346D0, 0xB35ABA: 0xB346D0,
                0xB6D1EF: 0xB67710, 0xB6D3EE: 0xB67710, 0xB6DBA6: 0xB67710}
PHOTO_CALLS = {0xDB8097: 0xDB7E00, 0xA1E8A7: 0xDF7C00}   # view build, pad gate
ACTOR_CALLS = (0x5DDFBE, 0x5DE4EE, 0x5E662E)   # -> 0xA8B7F0
PROJ_CALLS = (0x985F6C, 0xA28128, 0xA460F6, 0xCACB8D, 0xCACC2D, 0xD1E43E, 0xD1E47C, 0xDE5B18, 0xF43CFB)
RELOCATED = {"QteFsub%d" % i: (va, 0x0163CCE0) for i, va in
             enumerate((0x5AC120, 0x5AC5B3, 0x5AEEA5, 0x840AC9, 0x8410FA))}

def read(h, addr, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        raise OSError(ctypes.get_last_error())
    return buf.raw

exe = sys.argv[1]
si = STARTUPINFO(); si.cb = ctypes.sizeof(si); pi = PROCESS_INFORMATION()
CREATE_SUSPENDED = 0x4
cwd = exe.rsplit("\\", 1)[0] if "\\" in exe else None
if not k32.CreateProcessW(exe, None, None, None, False, CREATE_SUSPENDED, None, cwd, ctypes.byref(si), ctypes.byref(pi)):
    sys.exit("CreateProcess failed %d" % ctypes.get_last_error())
try:
    pbi = PBI()
    ntdll.NtQueryInformationProcess(pi.hProcess, 0, ctypes.byref(pbi), ctypes.sizeof(pbi), None)
    base = struct.unpack("<Q", read(pi.hProcess, pbi.Peb + 0x10, 8))[0]   # 64-bit PEB.ImageBaseAddress
    delta = base - PREFERRED
    print("image base 0x%08X (delta 0x%X)" % (base, delta & 0xFFFFFFFF))
    ts = struct.unpack("<I", read(pi.hProcess, base + struct.unpack("<I", read(pi.hProcess, base + 0x3C, 4))[0] + 8, 4))[0]
    print("TimeDateStamp 0x%08X %s" % (ts, "OK" if ts == 0x52E76F3A else "MISMATCH"))
    ok = True
    for name, (va, sig) in SITES.items():
        got = read(pi.hProcess, va + delta, len(sig))
        print("%-8s %s %s" % (name, got.hex(" ").upper(), "OK" if got == sig else "MISMATCH")); ok &= got == sig
    got = struct.unpack("<I", read(pi.hProcess, 0x1656CA0 + delta, 4))[0]   # cTitleStart vtable slot 5
    want = (0x9A7220 + delta) & 0xFFFFFFFF
    print("TitleStartSlot 0x%08X %s" % (got, "OK" if got == want else "MISMATCH")); ok &= got == want
    for name, (va, operand0) in RELOCATED.items():
        got = read(pi.hProcess, va + delta, 6)
        want = bytes([0xD8, 0x25]) + struct.pack("<I", (operand0 + delta) & 0xFFFFFFFF)
        print("%-8s %s %s" % (name, got.hex(" ").upper(), "OK" if got == want else "MISMATCH")); ok &= got == want
    good_calls = 0
    for va in RATE_CALLS:
        c = read(pi.hProcess, va + delta, 5)
        good_calls += c[0] == 0xE8 and va + 5 + struct.unpack("<i", c[1:])[0] == 0xE049B0
    print("RateDiv  %d/%d calls -> getRate %s" % (good_calls, len(RATE_CALLS), "OK" if good_calls == len(RATE_CALLS) else "MISMATCH"))
    ok &= good_calls == len(RATE_CALLS)
    good_proj = 0
    for va in PROJ_CALLS:
        c = read(pi.hProcess, va + delta, 5)
        good_proj += c[0] == 0xE8 and va + 5 + struct.unpack("<i", c[1:])[0] == 0xDDCCC0
    print("ProjCall %d/%d calls -> perspective %s" % (good_proj, len(PROJ_CALLS), "OK" if good_proj == len(PROJ_CALLS) else "MISMATCH"))
    ok &= good_proj == len(PROJ_CALLS)
    good_act = 0
    for va in ACTOR_CALLS:
        c = read(pi.hProcess, va + delta, 5)
        good_act += c[0] == 0xE8 and va + 5 + struct.unpack("<i", c[1:])[0] == 0xA8B7F0
    print("DebrisActor %d/%d calls %s" % (good_act, len(ACTOR_CALLS), "OK" if good_act == len(ACTOR_CALLS) else "MISMATCH"))
    ok &= good_act == len(ACTOR_CALLS)
    good_c = 0
    for va, tgt in CORPSE_CALLS.items():
        c = read(pi.hProcess, va + delta, 5)
        good_c += c[0] == 0xE8 and va + 5 + struct.unpack("<i", c[1:])[0] == tgt
    print("Corpse   %d/%d vanish calls %s" % (good_c, len(CORPSE_CALLS), "OK" if good_c == len(CORPSE_CALLS) else "MISMATCH"))
    ok &= good_c == len(CORPSE_CALLS)
    good_p = 0
    for va, tgt in PHOTO_CALLS.items():
        c = read(pi.hProcess, va + delta, 5)
        good_p += c[0] == 0xE8 and va + 5 + struct.unpack("<i", c[1:])[0] == tgt
    print("Photo    %d/%d calls %s" % (good_p, len(PHOTO_CALLS), "OK" if good_p == len(PHOTO_CALLS) else "MISMATCH"))
    ok &= good_p == len(PHOTO_CALLS)
    p = read(pi.hProcess, 0xF97F90 + delta, 5)
    operand = struct.unpack("<I", p[1:])[0]
    good = p[0] == 0xA1 and operand == (0x01F206D4 + delta) & 0xFFFFFFFF
    print("Present  %02X %08X (expect A1 %08X) %s" % (p[0], operand, (0x01F206D4 + delta) & 0xFFFFFFFF, "OK" if good else "MISMATCH"))
    ok &= good
    p = read(pi.hProcess, 0x1436F9E + delta, 6)   # D3DXCreateTextureFromFileInMemoryEx thunk: JMP [iat]
    operand = struct.unpack("<I", p[2:])[0]
    want = (0x015FD378 + delta) & 0xFFFFFFFF
    good = p[:2] == bytes.fromhex("FF25") and operand == want
    print("TexThunk %s (expect FF25 %08X) %s" % (p.hex(" "), want, "OK" if good else "MISMATCH"))
    ok &= good
    p = read(pi.hProcess, 0xAAC310 + delta, 6)   # Em0600 (EXCELSUS) ctor: MOV [ESI], vtable
    want = (0x0167793C + delta) & 0xFFFFFFFF
    good = p[:2] == bytes.fromhex("C706") and struct.unpack("<I", p[2:])[0] == want
    print("Em0600Ctor %s (expect C706 %08X) %s" % (p.hex(" "), want, "OK" if good else "MISMATCH"))
    ok &= good
    print("RESULT:", "PASS" if ok else "FAIL")
finally:
    k32.TerminateProcess(pi.hProcess, 0)
    k32.CloseHandle(pi.hThread); k32.CloseHandle(pi.hProcess)
