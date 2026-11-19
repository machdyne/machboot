#!/usr/bin/env python3
"""
Binary -> $readmemh file of 32-bit little-endian words.
Usage: bin2hex.py in.bin out.hex WORDS [--at BYTEADDR in2.bin ...]
"""
import sys

def main():
    src, dst, words = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
    mem = bytearray(words * 4)
    data = open(src, 'rb').read()
    mem[0:len(data)] = data
    args = sys.argv[4:]
    while args:
        assert args[0] == '--at'
        addr, f = int(args[1], 0), args[2]
        d = open(f, 'rb').read()
        mem[addr:addr + len(d)] = d
        args = args[3:]
    with open(dst, 'w') as o:
        for i in range(words):
            o.write('%08x\n' % int.from_bytes(mem[i * 4:i * 4 + 4], 'little'))

if __name__ == '__main__':
    main()
