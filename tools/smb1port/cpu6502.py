"""Minimal NMOS 6502 interpreter (official opcodes) for running SMB1's own
level parser offline. Decimal mode is ignored (the 2A03 has none)."""


class CPU:
    def __init__(self, read, write):
        self.read, self.write = read, write
        self.a = self.x = self.y = 0
        self.s = 0xFD
        self.p = 0x24
        self.pc = 0
        self.cycles = 0
        self.hooks = {}  # pc -> callable(cpu), run before the instruction there

    # -- helpers ---------------------------------------------------------
    def rd16(self, a):
        return self.read(a) | self.read((a + 1) & 0xFFFF) << 8

    def rd16zp(self, a):
        return self.read(a & 0xFF) | self.read((a + 1) & 0xFF) << 8

    def push(self, v):
        self.write(0x100 | self.s, v & 0xFF)
        self.s = (self.s - 1) & 0xFF

    def pull(self):
        self.s = (self.s + 1) & 0xFF
        return self.read(0x100 | self.s)

    def nz(self, v):
        self.p = (self.p & 0x7D) | (v & 0x80) | (0 if v else 0x02)
        return v

    def reset(self):
        self.pc = self.rd16(0xFFFC)
        self.s = 0xFD
        self.p = 0x24

    def nmi(self):
        self.push(self.pc >> 8)
        self.push(self.pc)
        self.push((self.p | 0x20) & ~0x10)
        self.p |= 0x04
        self.pc = self.rd16(0xFFFA)

    def jsr_to(self, addr, ret_marker=0xFFF0):
        """Call `addr` as a subroutine; run() stops when it returns."""
        r = ret_marker - 1
        self.push(r >> 8)
        self.push(r)
        self.pc = addr

    # -- addressing ------------------------------------------------------
    def addr(self, mode):
        pc = self.pc
        if mode == 'imm':
            self.pc += 1
            return pc
        if mode == 'zp':
            self.pc += 1
            return self.read(pc)
        if mode == 'zpx':
            self.pc += 1
            return (self.read(pc) + self.x) & 0xFF
        if mode == 'zpy':
            self.pc += 1
            return (self.read(pc) + self.y) & 0xFF
        if mode == 'abs':
            self.pc += 2
            return self.rd16(pc)
        if mode == 'abx':
            self.pc += 2
            return (self.rd16(pc) + self.x) & 0xFFFF
        if mode == 'aby':
            self.pc += 2
            return (self.rd16(pc) + self.y) & 0xFFFF
        if mode == 'izx':
            self.pc += 1
            return self.rd16zp((self.read(pc) + self.x) & 0xFF)
        if mode == 'izy':
            self.pc += 1
            return (self.rd16zp(self.read(pc)) + self.y) & 0xFFFF
        raise ValueError(mode)

    def branch(self, cond):
        off = self.read(self.pc)
        self.pc += 1
        if cond:
            self.pc = (self.pc + (off - 256 if off & 0x80 else off)) & 0xFFFF

    def adc(self, v):
        c = self.p & 1
        r = self.a + v + c
        self.p = (self.p & ~0x41) | (1 if r > 0xFF else 0) | \
            (0x40 if (~(self.a ^ v) & (self.a ^ r) & 0x80) else 0)
        self.a = self.nz(r & 0xFF)

    def cmp(self, reg, v):
        r = (reg - v) & 0x1FF
        self.p = (self.p & ~1) | (1 if reg >= v else 0)
        self.nz(r & 0xFF)

    # -- execution -------------------------------------------------------
    def step(self):
        h = self.hooks.get(self.pc)
        if h:
            h(self)
        op = self.read(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        self.cycles += 3
        e = OPS.get(op)
        if e is None:
            raise RuntimeError('bad opcode %02X at %04X' % (op, (self.pc - 1) & 0xFFFF))
        e(self)

    def run_until_return(self, marker=0xFFF0, limit=2_000_000):
        n = 0
        while self.pc != marker:
            self.step()
            n += 1
            if n > limit:
                raise RuntimeError('call did not return (pc=%04X)' % self.pc)


OPS = {}


def _ld(reg, mode):
    def f(c):
        setattr(c, reg, c.nz(c.read(c.addr(mode))))
    return f


def _st(reg, mode):
    def f(c):
        c.write(c.addr(mode), getattr(c, reg))
    return f


def _alu(kind, mode):
    def f(c):
        v = c.read(c.addr(mode))
        if kind == 'ORA':
            c.a = c.nz(c.a | v)
        elif kind == 'AND':
            c.a = c.nz(c.a & v)
        elif kind == 'EOR':
            c.a = c.nz(c.a ^ v)
        elif kind == 'ADC':
            c.adc(v)
        elif kind == 'SBC':
            c.adc(v ^ 0xFF)
        elif kind == 'CMP':
            c.cmp(c.a, v)
        elif kind == 'CPX':
            c.cmp(c.x, v)
        elif kind == 'CPY':
            c.cmp(c.y, v)
        elif kind == 'BIT':
            c.p = (c.p & 0x3D) | (v & 0xC0) | (0 if (c.a & v) else 0x02)
    return f


def _rmw(kind, mode):
    def calc(c, v):
        if kind == 'ASL':
            c.p = (c.p & ~1) | (v >> 7)
            v = (v << 1) & 0xFF
        elif kind == 'LSR':
            c.p = (c.p & ~1) | (v & 1)
            v >>= 1
        elif kind == 'ROL':
            cin = c.p & 1
            c.p = (c.p & ~1) | (v >> 7)
            v = ((v << 1) | cin) & 0xFF
        elif kind == 'ROR':
            cin = c.p & 1
            c.p = (c.p & ~1) | (v & 1)
            v = (v >> 1) | (cin << 7)
        elif kind == 'INC':
            v = (v + 1) & 0xFF
        elif kind == 'DEC':
            v = (v - 1) & 0xFF
        return c.nz(v)

    def f(c):
        if mode == 'acc':
            c.a = calc(c, c.a)
        else:
            a = c.addr(mode)
            c.write(a, calc(c, c.read(a)))
    return f


def _build():
    groups = {
        'ORA': {0x09: 'imm', 0x05: 'zp', 0x15: 'zpx', 0x0D: 'abs', 0x1D: 'abx', 0x19: 'aby', 0x01: 'izx', 0x11: 'izy'},
        'AND': {0x29: 'imm', 0x25: 'zp', 0x35: 'zpx', 0x2D: 'abs', 0x3D: 'abx', 0x39: 'aby', 0x21: 'izx', 0x31: 'izy'},
        'EOR': {0x49: 'imm', 0x45: 'zp', 0x55: 'zpx', 0x4D: 'abs', 0x5D: 'abx', 0x59: 'aby', 0x41: 'izx', 0x51: 'izy'},
        'ADC': {0x69: 'imm', 0x65: 'zp', 0x75: 'zpx', 0x6D: 'abs', 0x7D: 'abx', 0x79: 'aby', 0x61: 'izx', 0x71: 'izy'},
        'SBC': {0xE9: 'imm', 0xE5: 'zp', 0xF5: 'zpx', 0xED: 'abs', 0xFD: 'abx', 0xF9: 'aby', 0xE1: 'izx', 0xF1: 'izy'},
        'CMP': {0xC9: 'imm', 0xC5: 'zp', 0xD5: 'zpx', 0xCD: 'abs', 0xDD: 'abx', 0xD9: 'aby', 0xC1: 'izx', 0xD1: 'izy'},
        'CPX': {0xE0: 'imm', 0xE4: 'zp', 0xEC: 'abs'},
        'CPY': {0xC0: 'imm', 0xC4: 'zp', 0xCC: 'abs'},
        'BIT': {0x24: 'zp', 0x2C: 'abs'},
    }
    for k, m in groups.items():
        for o, mode in m.items():
            OPS[o] = _alu(k, mode)
    for reg, m in {'a': {0xA9: 'imm', 0xA5: 'zp', 0xB5: 'zpx', 0xAD: 'abs', 0xBD: 'abx', 0xB9: 'aby', 0xA1: 'izx', 0xB1: 'izy'},
                   'x': {0xA2: 'imm', 0xA6: 'zp', 0xB6: 'zpy', 0xAE: 'abs', 0xBE: 'aby'},
                   'y': {0xA0: 'imm', 0xA4: 'zp', 0xB4: 'zpx', 0xAC: 'abs', 0xBC: 'abx'}}.items():
        for o, mode in m.items():
            OPS[o] = _ld(reg, mode)
    for reg, m in {'a': {0x85: 'zp', 0x95: 'zpx', 0x8D: 'abs', 0x9D: 'abx', 0x99: 'aby', 0x81: 'izx', 0x91: 'izy'},
                   'x': {0x86: 'zp', 0x96: 'zpy', 0x8E: 'abs'},
                   'y': {0x84: 'zp', 0x94: 'zpx', 0x8C: 'abs'}}.items():
        for o, mode in m.items():
            OPS[o] = _st(reg, mode)
    rmw = {
        'ASL': {0x0A: 'acc', 0x06: 'zp', 0x16: 'zpx', 0x0E: 'abs', 0x1E: 'abx'},
        'LSR': {0x4A: 'acc', 0x46: 'zp', 0x56: 'zpx', 0x4E: 'abs', 0x5E: 'abx'},
        'ROL': {0x2A: 'acc', 0x26: 'zp', 0x36: 'zpx', 0x2E: 'abs', 0x3E: 'abx'},
        'ROR': {0x6A: 'acc', 0x66: 'zp', 0x76: 'zpx', 0x6E: 'abs', 0x7E: 'abx'},
        'INC': {0xE6: 'zp', 0xF6: 'zpx', 0xEE: 'abs', 0xFE: 'abx'},
        'DEC': {0xC6: 'zp', 0xD6: 'zpx', 0xCE: 'abs', 0xDE: 'abx'},
    }
    for k, m in rmw.items():
        for o, mode in m.items():
            OPS[o] = _rmw(k, mode)

    def br(mask, val):
        return lambda c: c.branch((c.p & mask) == val)
    OPS[0x10] = br(0x80, 0)
    OPS[0x30] = br(0x80, 0x80)
    OPS[0x50] = br(0x40, 0)
    OPS[0x70] = br(0x40, 0x40)
    OPS[0x90] = br(0x01, 0)
    OPS[0xB0] = br(0x01, 0x01)
    OPS[0xD0] = br(0x02, 0)
    OPS[0xF0] = br(0x02, 0x02)

    def jmp(c):
        c.pc = c.rd16(c.pc)

    def jmpi(c):
        a = c.rd16(c.pc)
        c.pc = c.read(a) | c.read((a & 0xFF00) | ((a + 1) & 0xFF)) << 8

    def jsr(c):
        t = c.rd16(c.pc)
        r = (c.pc + 1) & 0xFFFF
        c.push(r >> 8)
        c.push(r)
        c.pc = t

    def rts(c):
        lo = c.pull()
        c.pc = ((c.pull() << 8 | lo) + 1) & 0xFFFF

    def rti(c):
        c.p = (c.pull() | 0x20) & ~0x10
        lo = c.pull()
        c.pc = c.pull() << 8 | lo

    def brk(c):
        raise RuntimeError('BRK at %04X' % ((c.pc - 1) & 0xFFFF))
    OPS.update({0x4C: jmp, 0x6C: jmpi, 0x20: jsr, 0x60: rts, 0x40: rti, 0x00: brk})

    def setf(mask, on):
        def f(c):
            c.p = (c.p | mask) if on else (c.p & ~mask)
        return f
    OPS.update({0x18: setf(1, 0), 0x38: setf(1, 1), 0x58: setf(4, 0), 0x78: setf(4, 1),
                0xB8: setf(0x40, 0), 0xD8: setf(8, 0), 0xF8: setf(8, 1), 0xEA: lambda c: None})

    def t(src, dst, flags=True):
        def f(c):
            v = getattr(c, src)
            setattr(c, dst, c.nz(v) if flags else v)
        return f
    OPS.update({0xAA: t('a', 'x'), 0xA8: t('a', 'y'), 0x8A: t('x', 'a'), 0x98: t('y', 'a'),
                0xBA: t('s', 'x'), 0x9A: t('x', 's', False)})

    def inc(reg, d):
        def f(c):
            setattr(c, reg, c.nz((getattr(c, reg) + d) & 0xFF))
        return f
    OPS.update({0xE8: inc('x', 1), 0xC8: inc('y', 1), 0xCA: inc('x', -1), 0x88: inc('y', -1)})
    OPS[0x48] = lambda c: c.push(c.a)
    OPS[0x08] = lambda c: c.push(c.p | 0x30)

    def pla(c):
        c.a = c.nz(c.pull())

    def plp(c):
        c.p = (c.pull() | 0x20) & ~0x10
    OPS[0x68] = pla
    OPS[0x28] = plp


_build()
