#!/usr/bin/env python3
"""OEM scene and roll balance (Kodak Ansel SBA + FOS) from oem/PakonIMAu.dll.

The OEM colour engine's balance code is plain 32-bit x86 C. Rather than port
it, this module runs it under Unicorn: the DLL is mapped at its preferred base,
every import goes through a stub (calloc/free, setjmp, the x87 maths helpers),
and the C functions are called with the structures the OEM C++ wrapper
(AnsSbaCapabilityImpl) would build. Function addresses and structure offsets
come from the decompilation; see docs/IMAGING.md, "OEM scene balance".

Per roll:
  1. each frame: createAlgData (24x36 sample grid) + Sba() pass 1;
  2. SbaCalcFosResults over the first `maxFramesToFos` frames (roll film base
     and grey axis);
  3. each frame: Dmin lowered to the roll's, Sba() pass 2 with the FOS
     results, then Preference() -> the frame's RPD shift (R, G, B).
The shift is added to the frame's 12-bit RPD before rpd.pf.

Needs `unicorn` and `pefile` (web/requirements.txt) and oem/. Inputs are
12-bit RPD analysis images (analysis_image()).
"""
import math
import os
import struct

import numpy as np

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
OEM_DIR = os.path.join(REPO, 'oem')
DLL = os.path.join(OEM_DIR, 'PakonIMAu.dll')
SBA_DIR = os.path.join(OEM_DIR, 'ansel', 'sba')

# Plain-C entry points in PakonIMAu.dll.
F_DECODE_PCODE = 0x102884B0   # SbaDecodePcode(bytes, len, &out)
F_MAKESFS = 0x102AC820        # Makesfs(rows, out)
F_CREATE_ALG = 0x1028CEB0     # createAlgData (image -> 24x36 grid)
F_SBA = 0x1028B8D0            # Sba(), modes 0 (no FOS) / 1 (FOS pass 1) / 2
F_FOS = 0x1028F570            # SbaCalcFosResults
F_PREFERENCE = 0x1028C780     # Preference() -> final shifts

# AnsSbaCapabilityImpl layout (only what the C core touches).
IMPL_SIZE = 0x5A00
ALG = 0x1A          # algData; FOS 'absDens'
DMIN_STATS = 0x289C  # createAlgData per-channel minima (frame Dmin)
WORK = 0x290C       # Sba() workspace; FOS 'workspace'
INFO = 0x388C       # command switches + results; FOS 'ixData'
FLAGS = 0x38A0
RESULT = 0x38A2
PREF_OUT = 0x3A30
SHIFTS = 0x3A38     # AnsSbaCapability::getShifts
FRAME_DMIN = 0x3BC8
DPI = 0x4CF0        # SbaParameterReader block
PCODE_PTR = 0x5978

ANALYSIS_W, ANALYSIS_H = 192, 128   # landscape SBA analysis image

# SbaParameterReader field offsets: (offset, count of shorts).
DPI_FIELDS = {
    'fpa': (0x00, 3), 'neu': (0x06, 3), 'neo': (0x0C, 3), 'foson': (0x12, 1),
    'usehistory': (0x14, 1), 'fmt': (0x16, 1), 'dmd': (0x18, 3), 'fpo': (0x1E, 3),
    'pcls': (0x24, 1), 'pcwf': (0x26, 1), 'ix_pcwf': (0x28, 1), 'nonflashadj': (0x2A, 1),
    'cmm': (0x2C, 1), 'fog': (0x2E, 1), 'fxr': (0x30, 1), 'blk': (0x32, 1), 'bxr': (0x34, 1),
    'll1': (0x36, 1), 'll2': (0x38, 1), 'll3': (0x3A, 1), 'mff': (0x3C, 1),
    'maxframestofos': (0x3E, 1), 'splicepct': (0x40, 1), 'orderfpolowsatlimit': (0x42, 1),
    'orderfpohighsatlimit': (0x44, 1), 'orderfpominweight': (0x46, 1),
    'orderfpomaxweight': (0x48, 1), 'orderfpouseexpandedsatlimit': (0x4A, 1),
    'minpeakheight': (0x4C, 1), 'minpeakdist': (0x4E, 1), 'neutralbutton': (0x60, 1),
    'neutralbalancepoint': (0x62, 1), 'writedebugimages': (0x64, 1), 'mindmin': (0x68, 3),
}
DPI_DOUBLES = {'neutralunderconstraint': 0x50, 'neutraloverconstraint': 0x58}

# FUN_10214e30: Sba()'s 56-byte input block, (block offset, DPI offset, shorts).
SBA_BLOCK = [(0x00, 0x1E, 3), (0x06, 0x18, 3), (0x0C, 0x26, 1), (0x0E, 0x28, 1),
             (0x10, 0x2A, 1), (0x12, 0x2E, 1), (0x14, 0x30, 1), (0x16, 0x32, 1),
             (0x18, 0x34, 1), (0x1A, 0x3C, 1), (0x1C, 0x42, 1), (0x1E, 0x44, 1),
             (0x20, 0x46, 1), (0x22, 0x48, 1), (0x24, 0x4A, 1)]

# FUN_10214f20: Preference()'s parameter block, (block offset, DPI offset, shorts).
PREF_BLOCK = [(0x00, 0x1E, 3), (0x06, 0x00, 3), (0x0C, 0x06, 3), (0x12, 0x0C, 3),
              (0x18, 0x18, 3), (0x1E, 0x24, 1), (0x20, 0x26, 1), (0x22, 0x28, 1),
              (0x24, 0x2A, 1), (0x26, 0x16, 1), (0x30, 0x2C, 1), (0x32, 0x2E, 1),
              (0x34, 0x30, 1), (0x36, 0x32, 1), (0x38, 0x34, 1), (0x3A, 0x36, 1),
              (0x3C, 0x38, 1), (0x40, 0x3C, 1)]

# Constructor defaults for the 20 command-switch bytes at INFO when the scene
# context has no camera metadata (FUN_10217d00): 0xfe/0xff = unknown, then a
# fixed date 1995-08-29 12:30 (+0xf..+0x13; Sba() validates their ranges).
CMD_DEFAULTS = bytes([0xFE, 0xFE, 0xFE, 0xFF, 0xFF, 0xFE, 0x00, 0xFE, 0xFE, 0xFE,
                      0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0x5F, 0x08, 0x1D, 0x0C, 0x1E])


def available():
    """True when the emulator dependencies and oem/ are present."""
    try:
        import pefile  # noqa: F401
        import unicorn  # noqa: F401
    except ImportError:
        return False
    return os.path.exists(DLL) and os.path.isdir(SBA_DIR)


# -- x87 80-bit values -----------------------------------------------------------

def _f80_to_float(v):
    mant, se = v & ((1 << 64) - 1), (v >> 64) & 0xFFFF
    sign, exp = se >> 15, se & 0x7FFF
    if exp == 0 and mant == 0:
        return -0.0 if sign else 0.0
    if exp == 0x7FFF:
        return math.nan if mant << 1 & ((1 << 64) - 1) else (-math.inf if sign else math.inf)
    x = math.ldexp(mant, exp - 16383 - 63)
    return -x if sign else x


def _float_to_f80(x):
    if x == 0:
        return (0x8000 << 64) if math.copysign(1, x) < 0 else 0
    if math.isnan(x):
        return (0x7FFF << 64) | (3 << 62)
    if math.isinf(x):
        return ((0xFFFF if x < 0 else 0x7FFF) << 64) | (1 << 63)
    m, e = math.frexp(abs(x))                 # abs(x) = m * 2**e, 0.5 <= m < 1
    return (((1 if x < 0 else 0) << 15 | (e - 1 + 16383)) << 64) | int(m * (1 << 64))


# -- the emulator ----------------------------------------------------------------

STUB_BASE = 0x7F000000
STACK_TOP, STACK_SIZE = 0x7E000000, 0x00800000
HEAP_BASE, HEAP_SIZE = 0x60000000, 0x10000000
RET_MAGIC = 0x7F7FFFF0
_X87_UNARY = {'_CIacos': math.acos, '_CIasin': math.asin, '_CItanh': math.tanh}


def _align(x, a=0x1000):
    return (x + a - 1) & ~(a - 1)


class Emulator:
    """PakonIMAu.dll mapped into a 32-bit Unicorn machine. Functions are called
    cdecl with integer arguments; memory comes from a bump heap (one Emulator
    per roll, so nothing is ever freed)."""

    def __init__(self, path=DLL):
        import pefile
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
        pe = pefile.PE(path)
        base = pe.OPTIONAL_HEADER.ImageBase
        mu = self.mu = Uc(UC_ARCH_X86, UC_MODE_32)
        mu.mem_map(base, _align(pe.OPTIONAL_HEADER.SizeOfImage))
        mu.mem_write(base, pe.header)
        for s in pe.sections:
            mu.mem_write(base + s.VirtualAddress, s.get_data())
        mu.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE)
        mu.mem_map(HEAP_BASE, HEAP_SIZE)
        mu.mem_map(STUB_BASE, 0x01000000)
        self.heap = HEAP_BASE
        self.stubs = {}
        n = 0
        for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            for imp in entry.imports:
                addr = STUB_BASE + 0x10 * n
                n += 1
                self.stubs[addr] = (imp.name or f'ord{imp.ordinal}'.encode()).decode()
                mu.mem_write(imp.address, struct.pack('<I', addr))
                mu.mem_write(addr, b'\xC3')           # ret (cdecl: caller cleans)
        mu.mem_write(RET_MAGIC, b'\xF4')              # hlt
        mu.hook_add(UC_HOOK_CODE, self._stub, begin=STUB_BASE, end=STUB_BASE + 0x00FFFFFF)

    # memory
    def alloc(self, n, data=None):
        p = _align(self.heap, 16)
        self.heap = p + max(n, 1)
        if self.heap > HEAP_BASE + HEAP_SIZE - 0x40000:
            raise MemoryError('emulated heap exhausted')
        self.mu.mem_write(p, bytes(data) if data is not None else b'\0' * max(n, 1))
        return p

    def write(self, addr, data):
        self.mu.mem_write(addr, bytes(data))

    def read(self, addr, n):
        return bytes(self.mu.mem_read(addr, n))

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]

    def s16(self, addr, n):
        return struct.unpack(f'<{n}h', self.read(addr, 2 * n))

    # x87 stack: Unicorn's STn reads give only the mantissa, so go through the
    # physical registers, ST(i) = FP[(TOP + i) & 7] as (mantissa, exponent).
    def _fp_reg(self, i):
        import unicorn.x86_const as X
        top = (self.mu.reg_read(X.UC_X86_REG_FPSW) >> 11) & 7
        return getattr(X, f'UC_X86_REG_FP{(top + i) & 7}')

    def _st(self, i):
        mant, se = self.mu.reg_read(self._fp_reg(i))
        return _f80_to_float((se << 64) | mant)

    def _set_st(self, i, x):
        v = _float_to_f80(x)
        self.mu.reg_write(self._fp_reg(i), (v & ((1 << 64) - 1), v >> 64))

    def _snippet(self, code):
        from unicorn.x86_const import UC_X86_REG_EIP
        at = STUB_BASE + 0x00F00000
        self.mu.mem_write(at, code + b'\xF4')
        eip = self.mu.reg_read(UC_X86_REG_EIP)
        self.mu.emu_start(at, at + len(code))
        self.mu.reg_write(UC_X86_REG_EIP, eip)

    def _fpop(self):
        self._snippet(b'\xDD\xD8')                    # fstp st(0)

    def _fpush(self, x):
        scratch = STUB_BASE + 0x00F01000
        self.write(scratch, struct.pack('<d', x))
        self._snippet(b'\xDD\x05' + struct.pack('<I', scratch))   # fld qword [m]

    def _arg(self, i):
        from unicorn.x86_const import UC_X86_REG_ESP
        return self.u32(self.mu.reg_read(UC_X86_REG_ESP) + 4 + 4 * i)

    def _stub(self, mu, addr, size, user):
        from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EDX, UC_X86_REG_ESP
        name = self.stubs.get(addr)
        if name is None:
            return
        if name == 'calloc':
            r = self.alloc(self._arg(0) * self._arg(1))
        elif name in ('malloc', '??2@YAPAXI@Z'):
            r = self.alloc(self._arg(0))
        elif name in ('free', '??3@YAXPAX@Z'):
            r = 0
        elif name == 'memset':
            d, c, n = self._arg(0), self._arg(1), self._arg(2)
            self.write(d, bytes([c & 0xFF]) * n)
            r = d
        elif name in ('memcpy', 'memmove'):
            d, s, n = self._arg(0), self._arg(1), self._arg(2)
            self.write(d, self.read(s, n))
            r = d
        elif name == '_setjmp3':                     # the normal path returns 0
            r = 0
        elif name == 'longjmp':
            mu.emu_stop()
            raise RuntimeError('OEM code took its longjmp error path')
        elif name in _X87_UNARY:                     # argument and result in ST0
            self._set_st(0, _X87_UNARY[name](self._st(0)))
            return
        elif name == '_CIpow':                       # ST1 ** ST0, pops one
            y, x = self._st(0), self._st(1)
            self._fpop()
            self._set_st(0, math.pow(x, y))
            return
        elif name in ('floor', 'ceil'):              # double on the stack -> ST0
            x = struct.unpack('<d', self.read(mu.reg_read(UC_X86_REG_ESP) + 4, 8))[0]
            self._fpush(math.floor(x) if name == 'floor' else math.ceil(x))
            return
        elif name == '_ftol':                        # ST0 -> EDX:EAX, truncated
            v = int(self._st(0))
            self._fpop()
            mu.reg_write(UC_X86_REG_EDX, (v >> 32) & 0xFFFFFFFF)
            r = v
        else:
            mu.emu_stop()
            raise RuntimeError(f'OEM code called an unhandled import: {name}')
        mu.reg_write(UC_X86_REG_EAX, r & 0xFFFFFFFF)

    def call(self, func, *args):
        from unicorn import UcError
        from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX, UC_X86_REG_EIP
        frame = struct.pack('<I', RET_MAGIC) + b''.join(
            struct.pack('<I', a & 0xFFFFFFFF) for a in args)
        esp = STACK_TOP - 0x1000 - len(frame)
        self.write(esp, frame)
        self.mu.reg_write(UC_X86_REG_ESP, esp)
        try:
            self.mu.emu_start(func, RET_MAGIC)
        except UcError as e:
            raise RuntimeError(f'emulation fault at '
                               f'{self.mu.reg_read(UC_X86_REG_EIP):#x}: {e}') from None
        return self.mu.reg_read(UC_X86_REG_EAX)


# -- SBA / FOS -------------------------------------------------------------------

def read_dpi(path):
    """An Ansel .dpi file as {lower-case key: [values]}."""
    vals = {}
    with open(path, encoding='latin1') as f:
        for line in f:
            line = line.split('#')[0].strip()
            if '=' in line:
                k, v = (s.strip() for s in line.split('=', 1))
                vals[k.lower()] = v.split()
    return vals


class Sba:
    """One roll's worth of OEM scene balance (a fresh emulator per instance)."""

    def __init__(self, dpi='sba-CN-default.dpi'):
        e = self.e = Emulator()
        self.dpi = read_dpi(os.path.join(SBA_DIR, 'SbaDPI', dpi))
        with open(os.path.join(SBA_DIR, 'Pcode', self.dpi['pcode'][0]), 'rb') as f:
            pcode = f.read()
        out = e.alloc(4)
        self._check(e.call(F_DECODE_PCODE, e.alloc(len(pcode) + 2, pcode), len(pcode), out),
                    'SbaDecodePcode')
        self.pcode = e.u32(out)
        rows = []
        with open(os.path.join(SBA_DIR, 'Sfs', self.dpi['sfstable'][0])) as f:
            for line in f:
                rows += [int(x) for x in line.split()[:4]]
        # Makesfs(rows, out): out is the table object's +0x2c, which Sba() reads.
        self.sfs = e.alloc(0x8000)
        self._check(e.call(F_MAKESFS, e.alloc(2 * len(rows) + 8,
                                              struct.pack(f'<{len(rows)}h', *rows)),
                           self.sfs + 0x2C), 'Makesfs')
        self.dpi_bytes = self._dpi_bytes()
        self.block = e.alloc(56, self._pack(self.dpi_bytes, SBA_BLOCK, 56))

    @staticmethod
    def _check(rc, what):
        if rc:
            raise RuntimeError(f'{what} failed, return code {rc:#x}')

    @staticmethod
    def _pack(src, layout, size):
        b = bytearray(size)
        for bo, so, n in layout:
            b[bo:bo + 2 * n] = src[so:so + 2 * n]
        return b

    def _dpi_bytes(self):
        b = bytearray(0x70)
        for k, (off, n) in DPI_FIELDS.items():
            if k in self.dpi:
                struct.pack_into(f'<{n}h', b, off, *[int(float(x)) for x in self.dpi[k][:n]])
        for k, off in DPI_DOUBLES.items():
            if k in self.dpi:
                struct.pack_into('<d', b, off, float(self.dpi[k][0]))
        return bytes(b)

    def _dpi_short(self, off):
        return struct.unpack_from('<h', self.dpi_bytes, off)[0]

    def _pref_block(self):
        d = self.dpi_bytes
        b = self._pack(d, PREF_BLOCK, 0x48)
        struct.pack_into('<hh', b, 0x28, 0x32, 0x53)
        struct.pack_into('<I', b, 0x2C, self.sfs + 0x2C)
        struct.pack_into('<h', b, 0x3E, 0x8C)
        nb = self._dpi_short(0x60)
        lo, hi = (struct.unpack_from('<d', d, o)[0] for o in (0x50, 0x58))
        struct.pack_into('<hh', b, 0x42, int(nb * lo), int(nb * hi))
        v = self._dpi_short(0x62) * 0x2A495                    # x sqrt(3)
        struct.pack_into('<h', b, 0x46, int(((v + 50000) if v >= 0 else (v - 50000)) / 100000))
        return self.e.alloc(0x48, b)

    def pass1(self, rpd, fos_on=True):
        """createAlgData + Sba() for one frame. rpd: (rows, cols, 3) 12-bit RPD,
        landscape. Returns the frame's object address."""
        e = self.e
        rows, cols = rpd.shape[:2]
        impl = e.alloc(IMPL_SIZE)
        e.write(impl + 0x10, struct.pack('<I', impl + ALG))
        e.write(impl + DPI, self.dpi_bytes)
        e.write(impl + PCODE_PTR, struct.pack('<I', self.pcode))
        e.write(impl + INFO, CMD_DEFAULTS)
        planes = np.ascontiguousarray(np.moveaxis(np.asarray(rpd, '<i2'), -1, 0))
        data = e.alloc(planes.nbytes, planes.tobytes())
        n = rows * cols * 2
        image = e.alloc(16, struct.pack('<4I', 0, data, data + n, data + 2 * n))  # planar
        geometry = e.alloc(64, struct.pack('<11i', rows, cols, 99999, 99999, 99999, 99999,
                                           24, 36, 99999, 99999, 0))
        offsets = e.alloc(64, struct.pack('<4I', 0, e.alloc(8), 0, 0))
        aim = e.alloc(8, struct.pack('<3h', *[int(x) for x in self.dpi['mindmin'][:3]]))
        self._check(e.call(F_CREATE_ALG, image, impl + 0x10, 0, 0, impl + DMIN_STATS,
                           geometry, offsets, e.alloc(64), impl + 0x14, 0, 0, aim),
                    'createAlgData')
        e.write(impl + FRAME_DMIN, e.read(impl + DMIN_STATS, 6))
        self._check(e.call(F_SBA, impl + ALG, impl + FRAME_DMIN, impl + INFO,
                           1 if fos_on else 0, 0, self.block, self.pcode, self.sfs + 0x2C,
                           0, 0, e.alloc(64), impl + WORK, impl + RESULT), 'Sba() pass 1')
        return impl

    def fos(self, impls):
        """SbaCalcFosResults over the frames. Returns the result address."""
        e = self.e
        n = len(impls)
        arr = lambda off: e.alloc(4 * n, struct.pack(f'<{n}I', *[p + off for p in impls]))
        ref = e.alloc(8, self.dpi_bytes[0x1E:0x24])            # fpo
        out = e.alloc(0x40)
        self._check(e.call(F_FOS, n, 0, ref, self.pcode, 0, 0, arr(ALG), arr(INFO),
                           arr(WORK), out), 'SbaCalcFosResults')
        return out

    def pass2(self, impl, fos_out):
        """Sba() mode 2 with the FOS results, then Preference(). Returns the
        frame's RPD shift (R, G, B)."""
        e = self.e
        fdmin = [min(a, b) for a, b in zip(e.s16(impl + FRAME_DMIN, 3), e.s16(fos_out + 0xC, 3))]
        e.write(impl + FRAME_DMIN, struct.pack('<3h', *fdmin))
        self._check(e.call(F_SBA, impl + ALG, impl + FRAME_DMIN, impl + INFO, 2, 0, self.block,
                           self.pcode, self.sfs + 0x2C, 0, 0, fos_out, impl + WORK,
                           impl + RESULT), 'Sba() pass 2')
        return self.preference(impl, fos_out)

    def preference(self, impl, fos_out=0):
        self._check(self.e.call(F_PREFERENCE, impl + RESULT, fos_out, impl + PREF_OUT,
                                self._pref_block(), 0), 'Preference()')
        return self.e.s16(impl + SHIFTS, 3)


def roll_shifts(frames, dpi='sba-CN-default.dpi'):
    """OEM per-frame RPD shifts (n, 3) for a roll of analysis images (12-bit
    RPD, landscape). With at least `mff` frames: pass 1, FOS over the first
    `maxFramesToFos`, pass 2. With fewer, the OEM would use its scan history,
    which we do not have, so each frame is balanced on its own (FOS off)."""
    s = Sba(dpi)
    mff = s._dpi_short(0x3C)
    max_fos = s._dpi_short(0x3E)
    if len(frames) < max(mff, 1):
        return np.array([s.preference(s.pass1(f, fos_on=False)) for f in frames], np.float32)
    impls = [s.pass1(f) for f in frames]
    out = s.fos(impls[:max_fos])
    return np.array([s.pass2(i, out) for i in impls], np.float32)


def analysis_image(raw16, negmat, lut, rpd_fn):
    """A frame crop (raw 16-bit RGB) -> the SBA analysis image: landscape,
    area-averaged to ANALYSIS_H x ANALYSIS_W in raw space, then to 12-bit RPD
    with `rpd_fn` (pakon_image.oem_rpd)."""
    a = np.asarray(raw16)
    if a.shape[0] > a.shape[1]:
        a = np.rot90(a)
    h, w = ANALYSIS_H, ANALYSIS_W
    sy, sx = a.shape[0] // h, a.shape[1] // w
    y0, x0 = (a.shape[0] - sy * h) // 2, (a.shape[1] - sx * w) // 2
    a = a[y0:y0 + sy * h, x0:x0 + sx * w].astype(np.float32)
    small = a.reshape(h, sy, w, sx, 3).mean(axis=(1, 3))
    rpd = rpd_fn(np.clip(np.rint(small), 0, 65535).astype(np.uint16), negmat, lut)
    return np.rint(rpd).astype(np.int16)


if __name__ == '__main__':
    print('OEM SBA available:', available())
