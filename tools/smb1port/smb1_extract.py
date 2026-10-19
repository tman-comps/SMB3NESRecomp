"""Extract SMB1 levels by running SMB1's own area parser.

    python tools/smb1port/smb1_extract.py SMB1.nes out.json [W-L ...]

Boots the owner's Super Mario Bros. ROM in a minimal 6502 interpreter (no
PPU, no APU: register reads are stubbed just enough for the game to run its
frame loop), starts the requested level, and records every metatile column
SMB1's AreaParserCore builds, straight from MetatileBuffer, before the block
buffer filter drops background scenery. Enemies are decoded from the level's
own enemy data. Nothing Nintendo-owned is written except the level geometry
and object placements the converter needs.

Accepts the standalone cartridge (NROM, 32K PRG) or the Super Mario Bros. +
Duck Hunt multicart (mapper 66, SMB in the first 32K PRG bank).
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpu6502 import CPU  # noqa: E402

# ---- SMB1 (USA) addresses, checked against the ROM in find_layout() ------
AREA_PARSER_TASKS = 0x92B0      # AreaParserTaskHandler
AREA_PARSER_CORE = 0x93FC
CORE_AFTER_OBJECTS = 0x94D6     # after JSR ProcessAreaData: MetatileBuffer is final
INCREMENT_COLUMN = 0x92DB       # IncrementColumnPos
METATILE_BUFFER = 0x06A1        # 13 rows, top to bottom
OPER_MODE, OPER_TASK = 0x0770, 0x0772
WORLD, LEVEL, AREA_NUM = 0x075F, 0x075C, 0x0760
PAGE, COLUMN = 0x0725, 0x0726
AREA_TYPE = 0x074E
AREA_DATA, ENEMY_DATA = 0xE7, 0xE9

SIGNATURES = {
    # address: bytes that must be there
    0x92B0: 'ac 1f 07 d0 05 a0 08',
    0x93FC: 'ad 28 07 f0 03 20 08 95',
    0x94D3: '20 08 95',
    0x92DB: None,
}


def load_prg(path):
    d = open(path, 'rb').read()
    if d[:4] != b'NES\x1a':
        raise SystemExit('not an iNES file')
    prg_banks = d[4]
    mapper = (d[6] >> 4) | (d[7] & 0xF0)
    prg = d[16:16 + prg_banks * 0x4000]
    if mapper == 0 and len(prg) == 0x8000:
        return bytearray(prg)
    if mapper == 66:
        for b in range(len(prg) // 0x8000):
            bank = prg[b * 0x8000:(b + 1) * 0x8000]
            if bank[0x13FC:0x1404] == bytes.fromhex('ad2807f0032008 95'.replace(' ', '')):
                return bytearray(bank)
    raise SystemExit('no Super Mario Bros. PRG found (mapper %d)' % mapper)


class Machine:
    def __init__(self, prg):
        self.prg = prg
        self.ram = bytearray(0x800)
        self.pad = 0
        self.pad_shift = 0
        self.ppu_toggle = 0
        self.cpu = CPU(self.read, self.write)
        for addr, sig in SIGNATURES.items():
            if sig and prg[addr - 0x8000:addr - 0x8000 + len(bytes.fromhex(sig.replace(' ', '')))] \
                    != bytes.fromhex(sig.replace(' ', '')):
                raise SystemExit('ROM layout differs from SMB1 (USA) at $%04X' % addr)

    def read(self, a):
        if a < 0x2000:
            return self.ram[a & 0x7FF]
        if a < 0x4000:
            if (a & 7) == 2:
                # vblank and sprite-0 hit alternate so both of SMB1's wait loops finish
                self.ppu_toggle ^= 1
                return 0xC0 if self.ppu_toggle else 0x00
            return 0
        if a == 0x4016:
            v = (self.pad_shift >> 7) & 1
            self.pad_shift = (self.pad_shift << 1) & 0xFF
            return v | 0x40
        if a < 0x8000:
            return 0
        return self.prg[a - 0x8000]

    def write(self, a, v):
        if a < 0x2000:
            self.ram[a & 0x7FF] = v
        elif a == 0x4016:
            if v & 1:
                self.pad_shift = self.pad  # A B Sel St U D L R, MSB first

    def boot(self):
        c = self.cpu
        c.reset()
        if self.prg[:9] == bytes.fromhex('78a9118d02804c0080'):
            # Multicart: the menu's stub replaced SEI/CLD/LDA #$10/STA $2000/
            # LDX #$FF; resume at the stock TXS with those registers.
            c.pc, c.x, c.a = 0x8009, 0xFF, 0x10
        self.loop_pc = None
        for _ in range(200000):
            pc = c.pc
            if self.prg[pc - 0x8000] == 0x4C and (self.prg[pc - 0x7FFF] | self.prg[pc - 0x7FFE] << 8) == pc:
                self.loop_pc = pc
                break
            c.step()
        if self.loop_pc is None:
            raise SystemExit('SMB1 never reached its idle loop')

    def frame(self, pad=0):
        self.pad = pad
        c = self.cpu
        c.nmi()
        n = 0
        while c.pc != self.loop_pc:
            c.step()
            n += 1
            if n > 500000:
                raise SystemExit('NMI did not return')

    def call(self, addr):
        self.cpu.jsr_to(addr)
        self.cpu.run_until_return()


START = 0x10


def extract(prg, world, level, max_pages=32):
    m = Machine(prg)
    m.boot()
    columns = {}

    def grab(cpu):
        x = m.ram[PAGE] * 16 + m.ram[COLUMN]
        columns[x] = list(m.ram[METATILE_BUFFER:METATILE_BUFFER + 13])
    m.cpu.hooks[CORE_AFTER_OBJECTS] = grab

    for _ in range(40):
        m.frame()
    # The title screen's world select writes these before the game starts.
    m.ram[WORLD] = world - 1
    m.ram[LEVEL] = level - 1
    for i in range(400):
        m.frame(START if 10 <= i < 14 else 0)
        if i > 20 and m.ram[OPER_MODE] == 1 and m.ram[OPER_TASK] == 3:
            break
    else:
        raise SystemExit('level %d-%d did not start' % (world, level))
    for _ in range(5):
        m.frame()

    info = {
        'world': world, 'level': level,
        'area_type': m.ram[AREA_TYPE],
        'area_number': m.ram[AREA_NUM],
        'area_data': m.ram[AREA_DATA] | m.ram[AREA_DATA + 1] << 8,
        'enemy_data': m.ram[ENEMY_DATA] | m.ram[ENEMY_DATA + 1] << 8,
    }
    # Keep building columns past whatever the first screens loaded.
    while m.ram[PAGE] < max_pages:
        m.call(AREA_PARSER_CORE)
        m.call(INCREMENT_COLUMN)
    width = max(columns) + 1
    info['columns'] = [columns.get(x, [0] * 13) for x in range(width)]
    info['area_header'] = list(prg[info['area_data'] - 0x8000:info['area_data'] - 0x8000 + 2])
    info['last_object_page'] = last_object_page(prg, info['area_data'])
    info['enemies'] = decode_enemies(prg, info['enemy_data'])
    return info


def last_object_page(prg, addr):
    o = addr - 0x8000 + 2
    page = 0
    last = 0
    while prg[o] != 0xFD:
        a, b = prg[o], prg[o + 1]
        row = a & 0x0F
        if b & 0x80:
            page += 1
        if row == 0x0D and not (b & 0x40) and not (b & 0x80):
            page = b & 0x1F
        last = max(last, page)
        o += 2
    return last


def decode_enemies(prg, addr):
    o = addr - 0x8000
    page = 0
    out = []
    while prg[o] != 0xFF:
        a, b = prg[o], prg[o + 1]
        col, row = a >> 4, a & 0x0F
        if row == 0x0E:  # area pointer (pipe / vine destination): 3 bytes
            c = prg[o + 2]
            if b & 0x80:
                page += 1
            out.append({'kind': 'area_pointer', 'x': page * 16 + col, 'area': b & 0x7F,
                        'world_mask': c >> 5, 'dest_page': c & 0x1F})
            o += 3
            continue
        if row == 0x0F:  # page skip
            page = b & 0x1F
            o += 2
            continue
        if b & 0x80:
            page += 1
        out.append({'kind': 'enemy', 'x': page * 16 + col, 'row': row,
                    'id': b & 0x3F, 'hard_only': bool(b & 0x40)})
        o += 2
    return out


def main():
    rom, out = sys.argv[1], sys.argv[2]
    levels = sys.argv[3:] or ['1-1']
    prg = load_prg(rom)
    result = []
    for lv in levels:
        w, l = (int(t) for t in lv.split('-'))
        result.append(extract(prg, w, l))
        print('%s: %d columns, area type %d, %d enemies' % (
            lv, len(result[-1]['columns']), result[-1]['area_type'], len(result[-1]['enemies'])))
    with open(out, 'w') as f:
        json.dump(result, f)


if __name__ == '__main__':
    main()
