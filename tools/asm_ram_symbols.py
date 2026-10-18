"""Resolve RAM label addresses from the SMB3 disassembly's .org/.ds layout.

    python tools/asm_ram_symbols.py smb3-disassembly/smb3.asm [LABEL ...]

Walks smb3.asm top to bottom tracking `.org` and `.ds N`, and prints
`LABEL $ADDR` for each `label: .ds n` (all of them, or just those asked for).
Only the RAM declaration block matters, so parsing stops at the first
`.bank` directive.
"""
import re
import sys

ORG = re.compile(r"^\s*\.org\s+\$([0-9A-Fa-f]+)")
DS = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*\.ds\s+(\$?[0-9A-Fa-f]+)")
BARE_DS = re.compile(r"^\s*\.ds\s+(\$?[0-9A-Fa-f]+)")


def num(tok: str) -> int:
    return int(tok[1:], 16) if tok.startswith("$") else int(tok)


def main() -> int:
    path, wanted = sys.argv[1], set(sys.argv[2:])
    pc = 0
    syms = {}
    with open(path, encoding="latin-1") as f:
        for line in f:
            code = line.split(";", 1)[0]
            if re.match(r"^\s*\.bank\b", code):
                break
            if m := ORG.match(code):
                pc = int(m.group(1), 16)
            elif m := DS.match(code):
                syms[m.group(1)] = pc
                pc += num(m.group(2))
            elif m := BARE_DS.match(code):
                pc += num(m.group(1))
    for name, addr in syms.items():
        if not wanted or name in wanted:
            print(f"{name} ${addr:04X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
